/* Internal PDF reading, rasterisation, merging and CAJ JP2 support.
 *
 * Implemented behaviour:
 *  - `qw_pdf_reader_open` resolves objects through the cross-reference table
 *    (classic, xref streams, hybrid, /Prev chains) with a text-scan fallback,
 *    expands `/Type /ObjStm` object streams, walks the page tree and decrypts
 *    RC4/AES-128/AES-256 protected documents;
 *  - `qw_pdf_reader_extract_text` decodes page content streams (FlateDecode or
 *    raw), interprets the text operators (Tj, TJ, ', ") and returns UTF-8
 *    text; glyph codes are mapped through each font's /ToUnicode CMap
 *    (bfchar, bfrange, surrogate pairs), falling back to UCS-2 big endian for
 *    Type0 fonts and single-byte WinAnsi/ASCII for simple fonts;
 *  - `qw_pdf_reader_render_page` / `qw_pdf_render_page_memory` rasterise pages with
 *    RGB24 output and report unimplemented drawing operations through the
 *    route-loss note;
 *  - `qw_pdf_merge_documents` merges classic-xref inputs;
 *  - `qw_doc_decode_caj_jp2` decodes a CAJ JP2 payload through the internal
 *    image decoder.
 *
 * Unsupported inputs and security handlers return a stable
 * `QW_STATUS_ERR_UNSUPPORTED`. No MuPDF code, header or library is involved. */

#include "document_backend.h"

#include <quirewise/compression.h>
#include <quirewise/image.h>
#include <quirewise/crypto.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QW_PDF_READER_MAX_PAGES 100000u
#define QW_PDF_READER_MAX_PAGE_DEPTH 128u
#define QW_PDF_READER_MAX_TREE_NODES 4096u
#define QW_PDF_READER_MAX_STREAM ((size_t)64u * 1024u * 1024u)
#define QW_PDF_READER_MAX_TEXT ((size_t)256u * 1024u * 1024u)

typedef struct QwPdfBuffer {
    char *data;
    size_t size;
    size_t capacity;
} QwPdfBuffer;

typedef struct QwPdfObjectSlice {
    size_t object;
    const char *data;
    size_t size;
} QwPdfObjectSlice;

typedef struct QwPdfXrefEntry {
    size_t object;
    size_t sequence;   /* parse order: smaller means newer revision */
    size_t offset;     /* type 1: absolute object offset */
    size_t generation;
    size_t stream_object; /* type 2: containing object stream */
    size_t stream_index;
    unsigned type;     /* 0 free, 1 at offset, 2 inside an object stream */
} QwPdfXrefEntry;

struct QwPdfReader {
    const uint8_t *data;
    size_t size;
    int encrypted;
    int encryption_unsupported;
    int encryption_aes;
    int encryption_aes256;
    uint8_t encryption_key[32];
    size_t encryption_key_size;
    size_t *page_objects;
    size_t page_count;
    QwPdfObjectSlice *slices;
    size_t slice_count;
    size_t slice_capacity;
    QwPdfXrefEntry *xref;
    size_t xref_count;
    size_t xref_capacity;
    size_t xref_sequence;
    int xref_valid;
    char **buffers;
    size_t buffer_count;
    char *text;
    size_t text_size;
    int text_ready;
};

static void pdf_reader_free(QwPdfReader *reader)
{
    size_t index;
    if (!reader) return;
    for (index = 0u; index < reader->buffer_count; ++index) {
        free(reader->buffers[index]);
    }
    free(reader->buffers);
    free(reader->slices);
    free(reader->page_objects);
    free(reader->xref);
    free(reader->text);
    free(reader);
}

static int pdf_buffer_reserve(QwPdfBuffer *buffer, size_t needed)
{
    size_t capacity;
    char *grown;
    if (needed <= buffer->capacity) return 1;
    capacity = buffer->capacity ? buffer->capacity : 256u;
    while (capacity < needed) {
        if (capacity > QW_PDF_READER_MAX_TEXT / 2u) { capacity = needed; break; }
        capacity *= 2u;
    }
    if (capacity > QW_PDF_READER_MAX_TEXT) return 0;
    grown = (char *)realloc(buffer->data, capacity);
    if (!grown) return 0;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 1;
}

static int pdf_buffer_append(QwPdfBuffer *buffer, const char *bytes, size_t size)
{
    if (!size) return 1;
    if (!pdf_buffer_reserve(buffer, buffer->size + size + 1u)) return 0;
    memcpy(buffer->data + buffer->size, bytes, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return 1;
}

static int pdf_buffer_append_utf8(QwPdfBuffer *buffer, unsigned int code)
{
    char encoded[4];
    size_t length;
    if (code == 0x0Au || code == 0x0Du) return pdf_buffer_append(buffer, "\n", 1u);
    if (code < 0x80u) {
        encoded[0] = (char)code;
        length = 1u;
    } else if (code < 0x800u) {
        encoded[0] = (char)(0xC0u | (code >> 6));
        encoded[1] = (char)(0x80u | (code & 0x3Fu));
        length = 2u;
    } else if (code < 0x10000u) {
        encoded[0] = (char)(0xE0u | (code >> 12));
        encoded[1] = (char)(0x80u | ((code >> 6) & 0x3Fu));
        encoded[2] = (char)(0x80u | (code & 0x3Fu));
        length = 3u;
    } else {
        encoded[0] = (char)(0xF0u | (code >> 18));
        encoded[1] = (char)(0x80u | ((code >> 12) & 0x3Fu));
        encoded[2] = (char)(0x80u | ((code >> 6) & 0x3Fu));
        encoded[3] = (char)(0x80u | (code & 0x3Fu));
        length = 4u;
    }
    return pdf_buffer_append(buffer, encoded, length);
}

static const char *pdf_memmem(const char *haystack, size_t haystack_size,
                              const char *needle, size_t needle_size)
{
    size_t index;
    if (!needle_size || haystack_size < needle_size) return NULL;
    for (index = 0u; index + needle_size <= haystack_size; ++index) {
        if (memcmp(haystack + index, needle, needle_size) == 0) {
            return haystack + index;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Cross-reference tables                                              */
/*                                                                     */
/* Text scanning remains the fallback, but a document with a readable  */
/* xref chain is resolved through it: incremental updates then yield   */
/* the newest revision of an object instead of the first copy in the   */
/* file, and lookups become O(log n) instead of repeated full scans.   */
/* ------------------------------------------------------------------ */

enum {
    QW_PDF_XREF_MAX_SECTIONS = 64u,
    QW_PDF_XREF_MAX_ENTRIES = 5000000u,
    QW_PDF_XREF_MAX_DECODED = 64u * 1024u * 1024u
};

static int pdf_skip_ws(const char *text, size_t size, size_t *position)
{
    while (*position < size && isspace((unsigned char)text[*position])) {
        ++*position;
    }
    return *position < size;
}

static int pdf_parse_uint(const char *text, size_t size, size_t *position,
                          size_t *out_value)
{
    size_t index = *position;
    size_t value = 0u;
    if (!pdf_skip_ws(text, size, &index)) return 0;
    if (!isdigit((unsigned char)text[index])) return 0;
    while (index < size && isdigit((unsigned char)text[index])) {
        if (value > (SIZE_MAX - 9u) / 10u) return 0;
        value = value * 10u + (size_t)(text[index] - '0');
        ++index;
    }
    *position = index;
    *out_value = value;
    return 1;
}

static int pdf_add_xref(QwPdfReader *reader, size_t object, unsigned type,
                        size_t offset, size_t generation,
                        size_t stream_object, size_t stream_index)
{
    QwPdfXrefEntry *entry;
    if (reader->xref_count >= QW_PDF_XREF_MAX_ENTRIES) return 0;
    if (reader->xref_count == reader->xref_capacity) {
        size_t grown = reader->xref_capacity ? reader->xref_capacity * 2u : 256u;
        QwPdfXrefEntry *resized = (QwPdfXrefEntry *)realloc(
            reader->xref, grown * sizeof(*resized));
        if (!resized) return 0;
        reader->xref = resized;
        reader->xref_capacity = grown;
    }
    entry = &reader->xref[reader->xref_count++];
    entry->object = object;
    entry->sequence = reader->xref_sequence++;
    entry->offset = offset;
    entry->generation = generation;
    entry->stream_object = stream_object;
    entry->stream_index = stream_index;
    entry->type = type;
    return 1;
}

static int pdf_xref_compare(const void *left, const void *right)
{
    const QwPdfXrefEntry *a = (const QwPdfXrefEntry *)left;
    const QwPdfXrefEntry *b = (const QwPdfXrefEntry *)right;
    if (a->object < b->object) return -1;
    if (a->object > b->object) return 1;
    if (a->sequence < b->sequence) return -1;
    if (a->sequence > b->sequence) return 1;
    return 0;
}

static void pdf_xref_sort(QwPdfReader *reader)
{
    size_t read_index;
    size_t write_index = 0u;
    if (!reader->xref_count) return;
    qsort(reader->xref, reader->xref_count, sizeof(*reader->xref),
          pdf_xref_compare);
    for (read_index = 0u; read_index < reader->xref_count; ++read_index) {
        if (write_index == 0u ||
            reader->xref[write_index - 1u].object !=
                reader->xref[read_index].object) {
            reader->xref[write_index++] = reader->xref[read_index];
        }
    }
    reader->xref_count = write_index;
}

static const QwPdfXrefEntry *pdf_xref_lookup(const QwPdfReader *reader,
                                             size_t object)
{
    size_t low = 0u;
    size_t high = reader->xref_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (reader->xref[middle].object == object) return &reader->xref[middle];
        if (reader->xref[middle].object < object) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    return NULL;
}

/* PNG-style predictor used by xref streams and Flate image data
 * (/Predictor 10-15). bpp is the number of bytes per pixel. */
static int pdf_png_unpredict(const uint8_t *input, size_t rows,
                             size_t columns, size_t bpp, uint8_t *out)
{
    size_t row;
    size_t column;
    if (!bpp) bpp = 1u;
    for (row = 0u; row < rows; ++row) {
        const uint8_t *source = input + row * (columns + 1u);
        uint8_t *current = out + row * columns;
        const uint8_t *previous = row ? out + (row - 1u) * columns : NULL;
        unsigned filter = source[0];
        for (column = 0u; column < columns; ++column) {
            unsigned raw = source[1u + column];
            unsigned left = column >= bpp ? current[column - bpp] : 0u;
            unsigned up = previous ? previous[column] : 0u;
            unsigned upleft =
                (previous && column >= bpp) ? previous[column - bpp] : 0u;
            unsigned value;
            switch (filter) {
            case 0: value = raw; break;
            case 1: value = raw + left; break;
            case 2: value = raw + up; break;
            case 3: value = raw + ((left + up) >> 1); break;
            case 4: {
                int p = (int)left + (int)up - (int)upleft;
                int pa = p > (int)left ? p - (int)left : (int)left - p;
                int pb = p > (int)up ? p - (int)up : (int)up - p;
                int pc = p > (int)upleft ? p - (int)upleft : (int)upleft - p;
                unsigned predictor = (pa <= pb && pa <= pc) ? left
                                     : (pb <= pc) ? up
                                                  : upleft;
                value = raw + predictor;
                break;
            }
            default:
                return 0;
            }
            current[column] = (uint8_t)value;
        }
    }
    return 1;
}

static size_t pdf_xref_field(const uint8_t *bytes, size_t width)
{
    size_t value = 0u;
    size_t index;
    for (index = 0u; index < width; ++index) {
        value = (value << 8) | bytes[index];
    }
    return value;
}

/* Trailer/stream-dictionary helper: integer value after a key. */
static int pdf_dict_int(const char *dict, size_t size, const char *key,
                        size_t *out_value)
{
    const char *found = pdf_memmem(dict, size, key, strlen(key));
    size_t position;
    if (!found) return 0;
    position = (size_t)(found - dict) + strlen(key);
    return pdf_parse_uint(dict, size, &position, out_value);
}

static int pdf_xref_parse_classic(QwPdfReader *reader, size_t offset,
                                  size_t revision, size_t *out_prev,
                                  size_t *out_xrefstm)
{
    const char *data = (const char *)reader->data;
    size_t size = reader->size;
    size_t position = offset;
    *out_prev = 0u;
    *out_xrefstm = 0u;
    if (offset >= size || size - offset < 4u ||
        memcmp(data + offset, "xref", 4u) != 0) {
        return 0;
    }
    position = offset + 4u;
    for (;;) {
        size_t start;
        size_t count;
        size_t index;
        size_t guard = position;
        if (!pdf_skip_ws(data, size, &position)) return 0;
        if (position + 7u <= size &&
            memcmp(data + position, "trailer", 7u) == 0) {
            position += 7u;
            break;
        }
        if (!pdf_parse_uint(data, size, &position, &start) ||
            !pdf_parse_uint(data, size, &position, &count)) {
            return 0;
        }
        if (count > QW_PDF_XREF_MAX_ENTRIES) return 0;
        for (index = 0u; index < count; ++index) {
            size_t entry_offset;
            size_t generation;
            char kind;
            if (!pdf_parse_uint(data, size, &position, &entry_offset) ||
                !pdf_parse_uint(data, size, &position, &generation)) {
                return 0;
            }
            if (!pdf_skip_ws(data, size, &position)) return 0;
            kind = data[position];
            if (kind != 'n' && kind != 'f') return 0;
            ++position;
            if (!pdf_add_xref(reader, start + index, kind == 'n' ? 1u : 0u,
                              entry_offset, generation, 0u, 0u)) {
                return 0;
            }
        }
        if (position == guard) return 0;
    }
    {
        size_t trailer_end = size;
        const char *stop = pdf_memmem(data + position, size - position,
                                      "startxref", 9u);
        if (!stop) stop = pdf_memmem(data + position, size - position, "%%EOF",
                                     5u);
        if (stop) trailer_end = (size_t)(stop - data);
        (void)pdf_dict_int(data + position, trailer_end - position, "/Prev",
                           out_prev);
        (void)pdf_dict_int(data + position, trailer_end - position, "/XRefStm",
                           out_xrefstm);
    }
    (void)revision;
    return 1;
}

static int pdf_xref_parse_stream(QwPdfReader *reader, size_t offset,
                                 size_t revision, size_t *out_prev)
{
    const char *data = (const char *)reader->data;
    size_t size = reader->size;
    size_t position = offset;
    size_t object;
    size_t generation;
    size_t widths[3] = {0u, 0u, 0u};
    size_t total_objects = 0u;
    size_t rows = 0u;
    size_t row_bytes = 0u;
    size_t index_starts[64];
    size_t index_counts[64];
    size_t index_count = 0u;
    size_t predictor = 0u;
    size_t stream_length = 0u;
    size_t payload_start;
    const char *dict;
    const char *stream_keyword;
    size_t dict_size;
    uint8_t *decoded = NULL;
    uint8_t *rows_buffer = NULL;
    const uint8_t *entries;
    size_t entry_index = 0u;
    size_t section;
    int flate;

    *out_prev = 0u;
    if (!pdf_parse_uint(data, size, &position, &object) ||
        !pdf_parse_uint(data, size, &position, &generation)) {
        return 0;
    }
    if (!pdf_skip_ws(data, size, &position) || position + 3u > size ||
        memcmp(data + position, "obj", 3u) != 0) {
        return 0;
    }
    (void)object;
    (void)generation;
    position += 3u;
    dict = data + position;
    stream_keyword = pdf_memmem(dict, size - position, "stream", 6u);
    if (!stream_keyword) return 0;
    dict_size = (size_t)(stream_keyword - dict);
    if (!pdf_memmem(dict, dict_size, "/Type", 5u) ||
        !pdf_memmem(dict, dict_size, "XRef", 4u)) {
        /* Still accept a stream that declares /W; some producers omit /Type. */
        if (!pdf_memmem(dict, dict_size, "/W", 2u)) return 0;
    }
    {
        const char *w_key = pdf_memmem(dict, dict_size, "/W", 2u);
        size_t w_position;
        size_t field;
        const char *w_open;
        if (!w_key) return 0;
        w_position = (size_t)(w_key - dict) + 2u;
        if (!pdf_skip_ws(dict, dict_size, &w_position)) return 0;
        w_open = memchr(dict + w_position, '[', dict_size - w_position);
        if (!w_open) return 0;
        w_position = (size_t)(w_open - dict) + 1u;
        for (field = 0u; field < 3u; ++field) {
            if (!pdf_parse_uint(dict, dict_size, &w_position, &widths[field])) {
                return 0;
            }
        }
    }
    if (widths[0] > 8u || widths[1] > 8u || widths[2] > 8u) return 0;
    row_bytes = widths[0] + widths[1] + widths[2];
    if (!row_bytes || row_bytes > 32u) return 0;
    (void)pdf_dict_int(dict, dict_size, "/Size", &total_objects);
    {
        const char *index_key = pdf_memmem(dict, dict_size, "/Index", 6u);
        if (index_key) {
            size_t index_position = (size_t)(index_key - dict) + 6u;
            const char *open;
            if (!pdf_skip_ws(dict, dict_size, &index_position)) return 0;
            open = memchr(dict + index_position, '[',
                          dict_size - index_position);
            if (!open) return 0;
            index_position = (size_t)(open - dict) + 1u;
            while (index_count < 64u) {
                size_t start;
                size_t count;
                size_t check = index_position;
                if (!pdf_skip_ws(dict, dict_size, &index_position)) break;
                if (dict[index_position] == ']') break;
                if (!pdf_parse_uint(dict, dict_size, &index_position, &start) ||
                    !pdf_parse_uint(dict, dict_size, &index_position, &count)) {
                    return 0;
                }
                index_starts[index_count] = start;
                index_counts[index_count] = count;
                ++index_count;
                if (index_position == check) return 0;
            }
        }
    }
    if (!index_count) {
        if (!total_objects) return 0;
        index_starts[0] = 0u;
        index_counts[0] = total_objects;
        index_count = 1u;
    }
    for (section = 0u; section < index_count; ++section) {
        if (index_counts[section] > QW_PDF_XREF_MAX_ENTRIES - rows) return 0;
        rows += index_counts[section];
    }
    if (!rows) return 0;
    flate = pdf_memmem(dict, dict_size, "/FlateDecode", 12u) != NULL ||
            pdf_memmem(dict, dict_size, "/Fl", 3u) != NULL;
    (void)pdf_dict_int(dict, dict_size, "/Predictor", &predictor);
    payload_start = (size_t)(stream_keyword - (const char *)reader->data) + 6u;
    while (payload_start < size &&
           (reader->data[payload_start] == '\r' ||
            reader->data[payload_start] == '\n')) {
        ++payload_start;
    }
    if (!pdf_dict_int(dict, dict_size, "/Length", &stream_length)) {
        const char *endstream = pdf_memmem(
            (const char *)reader->data + payload_start, size - payload_start,
            "endstream", 9u);
        if (!endstream) return 0;
        stream_length = (size_t)(endstream - ((const char *)reader->data + payload_start));
    }
    if (stream_length > size - payload_start) return 0;
    {
        size_t decoded_bytes = rows * row_bytes;
        size_t inflated_bytes = decoded_bytes;
        size_t produced = 0u;
        if (predictor >= 10u) {
            if (rows > SIZE_MAX / (row_bytes + 1u)) return 0;
            inflated_bytes = rows * (row_bytes + 1u);
        }
        if (inflated_bytes > QW_PDF_XREF_MAX_DECODED || decoded_bytes > QW_PDF_XREF_MAX_DECODED ||
            inflated_bytes < decoded_bytes) {
            return 0;
        }
        decoded = (uint8_t *)malloc(inflated_bytes);
        if (!decoded) return 0;
        if (flate) {
            QwCompressionInflateStatus status = qw_compression_inflate_zlib_bounded(
                reader->data + payload_start, stream_length, decoded,
                inflated_bytes, &produced);
            if (status != QW_COMPRESSION_INFLATE_OK ||
                produced != inflated_bytes) {
                free(decoded);
                return 0;
            }
        } else {
            if (stream_length < inflated_bytes) {
                free(decoded);
                return 0;
            }
            memcpy(decoded, reader->data + payload_start, inflated_bytes);
        }
        if (predictor >= 10u) {
            rows_buffer = (uint8_t *)malloc(decoded_bytes);
            if (!rows_buffer ||
                !pdf_png_unpredict(decoded, rows, row_bytes, 1u, rows_buffer)) {
                free(decoded);
                free(rows_buffer);
                return 0;
            }
            entries = rows_buffer;
        } else {
            entries = decoded;
        }
    }
    for (section = 0u; section < index_count; ++section) {
        size_t start = index_starts[section];
        size_t count = index_counts[section];
        size_t index;
        for (index = 0u; index < count; ++index, ++entry_index) {
            const uint8_t *entry = entries + entry_index * row_bytes;
            size_t type = widths[0] ? pdf_xref_field(entry, widths[0]) : 1u;
            size_t field1 = pdf_xref_field(entry + widths[0], widths[1]);
            size_t field2 = pdf_xref_field(entry + widths[0] + widths[1],
                                           widths[2]);
            if (type == 0u) {
                (void)pdf_add_xref(reader, start + index, 0u, 0u, field2, 0u,
                                   0u);
            } else if (type == 1u) {
                (void)pdf_add_xref(reader, start + index, 1u, field1, field2,
                                   0u, 0u);
            } else if (type == 2u) {
                (void)pdf_add_xref(reader, start + index, 2u, 0u, 0u, field1,
                                   field2);
            }
        }
    }
    (void)pdf_dict_int(dict, dict_size, "/Prev", out_prev);
    free(decoded);
    free(rows_buffer);
    (void)revision;
    return 1;
}

/* Build the xref index from the last startxref; a failure leaves whatever was
 * parsed so far, and object lookups fall back to scanning per object. */
static void pdf_xref_build(QwPdfReader *reader)
{
    const char *data = (const char *)reader->data;
    size_t size = reader->size;
    size_t position = 0u;
    size_t offset = 0u;
    size_t visited[QW_PDF_XREF_MAX_SECTIONS];
    size_t visited_count = 0u;
    size_t revision = 0u;
    const char *last = NULL;
    while (position + 9u <= size) {
        const char *found = pdf_memmem(data + position, size - position,
                                       "startxref", 9u);
        if (!found) break;
        last = found;
        position = (size_t)(found - data) + 9u;
    }
    if (!last) return;
    {
        size_t parse = (size_t)(last - data) + 9u;
        if (!pdf_parse_uint(data, size, &parse, &offset)) return;
    }
    while (offset && revision < QW_PDF_XREF_MAX_SECTIONS) {
        size_t previous = 0u;
        size_t xrefstm = 0u;
        size_t index;
        for (index = 0u; index < visited_count; ++index) {
            if (visited[index] == offset) break;
        }
        if (index < visited_count) break; /* cycle */
        visited[visited_count++] = offset;
        if (pdf_xref_parse_classic(reader, offset, revision, &previous,
                                   &xrefstm)) {
            if (xrefstm) {
                size_t ignored = 0u;
                (void)pdf_xref_parse_stream(reader, xrefstm, revision,
                                            &ignored);
            }
        } else if (!pdf_xref_parse_stream(reader, offset, revision,
                                          &previous)) {
            break;
        }
        if (!previous) break;
        offset = previous;
        ++revision;
    }
    pdf_xref_sort(reader);
    reader->xref_valid = reader->xref_count > 0u;
}

/* Return the body pointer after "<num> <gen> obj" when it matches. */
static const char *pdf_object_at(const QwPdfReader *reader, size_t offset,
                                 size_t object)
{
    const char *data = (const char *)reader->data;
    size_t size = reader->size;
    size_t position = offset;
    size_t number;
    size_t generation;
    if (!pdf_parse_uint(data, size, &position, &number) || number != object) {
        return NULL;
    }
    if (!pdf_parse_uint(data, size, &position, &generation)) return NULL;
    (void)generation;
    if (!pdf_skip_ws(data, size, &position) || position + 3u > size ||
        memcmp(data + position, "obj", 3u) != 0) {
        return NULL;
    }
    return data + position + 3u;
}

/* Find "<number> 0 obj" with digit-boundary checks and return the offset of the
 * dictionary body. */
static const char *pdf_find_object(const QwPdfReader *reader, size_t object)
{
    char pattern[32];
    int length = snprintf(pattern, sizeof(pattern), "%zu 0 obj", object);
    const char *cursor = (const char *)reader->data;
    size_t remaining = reader->size;
    if (length <= 0) return NULL;
    if (reader->xref_valid) {
        const QwPdfXrefEntry *entry = pdf_xref_lookup(reader, object);
        if (entry) {
            if (entry->type == 0u) return NULL;
            if (entry->type == 1u) {
                const char *body = pdf_object_at(reader, entry->offset, object);
                if (body) return body;
            }
            /* type 2 or a stale offset: fall back to the scan below. */
        }
    }
    while (remaining >= (size_t)length) {
        const char *found = pdf_memmem(cursor, remaining, pattern, (size_t)length);
        if (!found) return NULL;
        if (found == (const char *)reader->data ||
            !isdigit((unsigned char)found[-1])) {
            return found + length;
        }
        cursor = found + 1;
        remaining = reader->size - (size_t)(cursor - (const char *)reader->data);
    }
    return NULL;
}

/* Walk backwards from a "/Type /Page" match to the owning object number. */
static int pdf_object_number_before(const QwPdfReader *reader, const char *position,
                                    size_t *out_object)
{
    const char *start = (const char *)reader->data;
    const char *cursor = position;
    while (cursor > start) {
        if (cursor - start >= 6 && memcmp(cursor - 6, " 0 obj", 6) == 0) {
            const char *digits_end = cursor - 6;
            const char *digits_start = digits_end;
            size_t value = 0u;
            size_t scale = 1u;
            while (digits_start > start && isdigit((unsigned char)digits_start[-1]) &&
                   digits_end - digits_start < 9) {
                --digits_start;
            }
            if (digits_start == digits_end) return 0;
            {
                const char *scan = digits_end;
                while (scan > digits_start) {
                    --scan;
                    value += (size_t)(*scan - '0') * scale;
                    scale *= 10u;
                }
            }
            *out_object = value;
            return 1;
        }
        --cursor;
    }
    return 0;
}

static int pdf_find_reference(const char *dict, size_t dict_size, const char *key,
                              size_t *out_object)
{
    const char *position = pdf_memmem(dict, dict_size, key, strlen(key));
    size_t value = 0u;
    if (!position) return 0;
    position += strlen(key);
    while ((size_t)(position - dict) < dict_size && isspace((unsigned char)*position)) ++position;
    if (!isdigit((unsigned char)*position)) return 0;
    while ((size_t)(position - dict) < dict_size && isdigit((unsigned char)*position)) {
        value = value * 10u + (size_t)(*position - '0');
        ++position;
    }
    *out_object = value;
    return 1;
}

/* Tolerant name check: PDF producers write "/Type /Page", "/Type/Page" or
 * "/Type\n/Page" interchangeably. */
static int pdf_name_is(const char *position, size_t available, const char *name)
{
    size_t length = strlen(name);
    size_t index = 0u;
    while (index < available && (position[index] == ' ' || position[index] == '\t' ||
                                 position[index] == '\r' || position[index] == '\n')) {
        ++index;
    }
    /* "/Type/Page" has no space before the name slash. */
    if (index < available && position[index] == '/') ++index;
    if (available - index < length) return 0;
    if (memcmp(position + index, name, length) != 0) return 0;
    if (available - index == length) return 1;
    {
        char next = position[index + length];
        return !isalnum((unsigned char)next) && next != '_' && next != '-';
    }
}

/* Find "\/Type" followed by the given name; returns pointer after the name. */
static const char *pdf_find_typed(const char *data, size_t size, const char *name)
{
    size_t index = 0u;
    while (index + 5u < size) {
        if (data[index] == '/' && memcmp(data + index, "/Type", 5u) == 0 &&
            pdf_name_is(data + index + 5u, size - index - 5u, name)) {
            return data + index + 5u;
        }
        ++index;
    }
    return NULL;
}

/* Raw object body: dictionary bytes between "N 0 obj" and "endobj". */
static int pdf_raw_object_body(const QwPdfReader *reader, size_t object,
                               const char **out_body, size_t *out_size)
{
    const char *body = pdf_find_object(reader, object);
    size_t offset;
    const char *endobj;
    if (!body) return 0;
    offset = (size_t)(body - (const char *)reader->data);
    endobj = pdf_memmem(body, reader->size - offset, "endobj", 6u);
    if (!endobj) return 0;
    *out_body = body;
    *out_size = (size_t)(endobj - body);
    return 1;
}

/* Object body from the main buffer or from a decompressed object stream. */
static int pdf_object_body(const QwPdfReader *reader, size_t object,
                           const char **out_body, size_t *out_size)
{
    size_t index;
    for (index = 0u; index < reader->slice_count; ++index) {
        if (reader->slices[index].object == object) {
            *out_body = reader->slices[index].data;
            *out_size = reader->slices[index].size;
            return 1;
        }
    }
    return pdf_raw_object_body(reader, object, out_body, out_size);
}

static int pdf_decrypt_object_data(const QwPdfReader *reader, size_t object,
                                   uint8_t *data, size_t *size);

/* Locate a stream payload inside an object body.
 *
 * The dictionary /Length is authoritative when the declared byte count lands
 * exactly on "endstream": scanning for the marker truncates payloads that
 * happen to contain those bytes, and trimming trailing EOLs corrupts streams
 * whose last data byte is a newline. When /Length is missing, indirect or
 * inconsistent, fall back to marker scanning with EOL trimming. */
static int pdf_stream_extent(const char *body, size_t body_size,
                             const char **out_payload, size_t *out_size,
                             int *out_flate)
{
    const char *stream = pdf_memmem(body, body_size, "stream", 6u);
    const char *endstream = NULL;
    size_t dict_size;
    size_t payload;
    long declared = -1;
    if (!stream) return 0;
    dict_size = (size_t)(stream - body);
    payload = dict_size + 6u;
    if (payload >= body_size) return 0;
    if (body[payload] == '\r') {
        ++payload;
        if (payload < body_size && body[payload] == '\n') ++payload;
    } else if (body[payload] == '\n') {
        ++payload;
    }
    {
        const char *key = pdf_memmem(body, dict_size, "/Length", 7u);
        if (key) {
            const char *scan = key + 7u;
            const char *limit = body + dict_size;
            size_t value = 0u;
            int digits = 0;
            while (scan < limit && isspace((unsigned char)*scan)) ++scan;
            while (scan < limit && isdigit((unsigned char)*scan)) {
                value = value * 10u + (size_t)(*scan - '0');
                ++digits;
                ++scan;
            }
            if (digits) {
                const char *after = scan;
                while (after < limit && isspace((unsigned char)*after)) ++after;
                /* A trailing "<gen> R" means /Length is an indirect object. */
                if (!(after < limit && isdigit((unsigned char)*after))) {
                    declared = (long)value;
                }
            }
        }
    }
    if (declared >= 0 && (size_t)declared <= body_size - payload) {
        size_t after = payload + (size_t)declared;
        while (after < body_size && isspace((unsigned char)body[after])) {
            ++after;
        }
        if (after + 9u <= body_size &&
            memcmp(body + after, "endstream", 9u) == 0) {
            endstream = body + after;
        }
    }
    if (endstream) {
        *out_size = (size_t)declared;
    } else {
        size_t trimmed;
        endstream = pdf_memmem(body + payload, body_size - payload,
                               "endstream", 9u);
        if (!endstream) return 0;
        trimmed = (size_t)(endstream - (body + payload));
        while (trimmed &&
               (body[payload + trimmed - 1u] == '\n' ||
                body[payload + trimmed - 1u] == '\r')) {
            --trimmed;
        }
        *out_size = trimmed;
    }
    *out_payload = body + payload;
    if (out_flate) {
        *out_flate =
            pdf_memmem(body, dict_size, "/FlateDecode", 12u) != NULL;
    }
    return *out_size > 0u;
}

/* Register every object stored inside a decompressed /Type /ObjStm stream. */
static int pdf_register_object_stream(QwPdfReader *reader, size_t owner)
{
    const char *body;
    size_t body_size;
    const char *stream_payload = NULL;
    size_t payload_size = 0u;
    uint8_t *inflated = NULL;
    size_t inflated_size = 0u;
    size_t count = 0u;
    size_t first = 0u;
    const char *cursor;
    size_t index;

    if (!pdf_raw_object_body(reader, owner, &body, &body_size)) return 1;
    if (!pdf_stream_extent(body, body_size, &stream_payload, &payload_size,
                           NULL) ||
        payload_size == 0u || payload_size > QW_PDF_READER_MAX_STREAM) {
        return 1;
    }
    {
        const char *n_key = pdf_memmem(body, body_size, "/N", 2u);
        const char *first_key = pdf_memmem(body, body_size, "/First", 6u);
        if (!n_key || !first_key) return 1;
        count = (size_t)strtoul(n_key + 2, NULL, 10);
        first = (size_t)strtoul(first_key + 6, NULL, 10);
    }
    if (!count || count > 100000u) return 1;
    {
        /* Streams inside an encrypted document are ciphertext until the
         * per-object key is applied; inflating them first can never succeed. */
        const uint8_t *stream_bytes = (const uint8_t *)stream_payload;
        uint8_t *decrypted = NULL;
        size_t capacity_guess = payload_size * 6u + 4096u;
        size_t attempt;
        if (reader->encrypted && payload_size) {
            decrypted = (uint8_t *)malloc(payload_size);
            if (!decrypted) return 0;
            memcpy(decrypted, stream_bytes, payload_size);
            if (!pdf_decrypt_object_data(reader, owner, decrypted,
                                         &payload_size)) {
                free(decrypted);
                return 1;
            }
            stream_bytes = decrypted;
        }
        for (attempt = 0u; attempt < 4u; ++attempt) {
            size_t produced = 0u;
            QwCompressionInflateStatus status;
            if (capacity_guess > QW_PDF_READER_MAX_STREAM) {
                free(decrypted);
                return 1;
            }
            inflated = (uint8_t *)malloc(capacity_guess);
            if (!inflated) {
                free(decrypted);
                return 0;
            }
            status = qw_compression_inflate_zlib_bounded(
                stream_bytes, payload_size, inflated, capacity_guess,
                &produced);
            if (status == QW_COMPRESSION_INFLATE_OK) {
                inflated_size = produced;
                break;
            }
            free(inflated);
            inflated = NULL;
            if (status != QW_COMPRESSION_INFLATE_OUTPUT_FULL) {
                free(decrypted);
                return 1;
            }
            capacity_guess *= 4u;
        }
        free(decrypted);
    }
    if (!inflated || inflated_size <= first) {
        free(inflated);
        return 1;
    }
    if (reader->buffer_count >= 64u) { free(inflated); return 1; }
    /* Header pairs are "num offset" repeated, followed by the objects. */
    cursor = (const char *)inflated;
    {
        size_t offsets[1024];
        size_t numbers[1024];
        size_t parsed = count < 1024u ? count : 1024u;
        for (index = 0u; index < parsed; ++index) {
            char *after = NULL;
            numbers[index] = (size_t)strtoul(cursor, &after, 10);
            cursor = after;
            offsets[index] = (size_t)strtoul(cursor, &after, 10);
            cursor = after;
        }
        if (reader->buffer_count >= 64u) { free(inflated); return 1; }
        reader->buffers = (char **)realloc(reader->buffers,
                                           (reader->buffer_count + 1u) * sizeof(char *));
        if (!reader->buffers) { free(inflated); return 0; }
        reader->buffers[reader->buffer_count++] = (char *)inflated;
        for (index = 0u; index < parsed; ++index) {
            size_t stop = (index + 1u < parsed) ? offsets[index + 1u]
                                                : inflated_size - first;
            if (first + offsets[index] >= inflated_size) break;
            if (stop <= offsets[index]) stop = inflated_size - first;
            if (reader->slice_count >= reader->slice_capacity) {
                size_t grown = reader->slice_capacity
                                   ? reader->slice_capacity * 2u
                                   : 64u;
                QwPdfObjectSlice *resized = (QwPdfObjectSlice *)realloc(
                    reader->slices, grown * sizeof(*resized));
                if (!resized) return 0;
                reader->slices = resized;
                reader->slice_capacity = grown;
            }
            reader->slices[reader->slice_count].object = numbers[index];
            reader->slices[reader->slice_count].data =
                (const char *)inflated + first + offsets[index];
            reader->slices[reader->slice_count].size = stop - offsets[index];
            reader->slice_count += 1u;
        }
    }
    return 1;
}

/* Return the raw stream bytes for an object, decompressed when FlateDecode. */

static const uint8_t pdf_password_padding[32] = {
    0x28, 0xbf, 0x4e, 0x5e, 0x4e, 0x75, 0x8a, 0x41, 0x64, 0x00, 0x4e, 0x56,
    0xff, 0xfa, 0x01, 0x08, 0x2e, 0x2e, 0x00, 0xb6, 0xd0, 0x68, 0x3e, 0x80,
    0x2f, 0x0c, 0xa9, 0xfe, 0x64, 0x53, 0x69, 0x7a
};

static void pdf_rc4(const uint8_t *key, size_t key_size, uint8_t *data,
                    size_t size)
{
    uint8_t state[256];
    size_t i, j = 0u;
    for (i = 0u; i < 256u; ++i) state[i] = (uint8_t)i;
    for (i = 0u; i < 256u; ++i) {
        uint8_t swap;
        j = (j + state[i] + key[i % key_size]) & 0xffu;
        swap = state[i];
        state[i] = state[j];
        state[j] = swap;
    }
    i = 0u;
    j = 0u;
    {
        size_t index;
        for (index = 0u; index < size; ++index) {
            uint8_t swap;
            i = (i + 1u) & 0xffu;
            j = (j + state[i]) & 0xffu;
            swap = state[i];
            state[i] = state[j];
            state[j] = swap;
            data[index] ^= state[(state[i] + state[j]) & 0xffu];
        }
    }
}

static int pdf_parse_hex_field(const char *dict, size_t size, const char *tag,
                               uint8_t *out, size_t capacity, size_t *out_size)
{
    size_t tag_size = strlen(tag);
    size_t position = 0u;
    const char *found = NULL;
    const char *cursor;
    size_t count = 0u;
    while (position + tag_size <= size) {
        const char *candidate =
            pdf_memmem(dict + position, size - position, tag, tag_size);
        size_t after;
        if (!candidate) return 0;
        after = (size_t)(candidate - dict) + tag_size;
        if (tag_size == 1u && tag[0] == '<') {
            found = candidate;
            break;
        }
        while (after < size && isspace((unsigned char)dict[after])) ++after;
        if (after < size && dict[after] == '<') {
            found = candidate;
            break;
        }
        position = (size_t)(candidate - dict) + 1u;
    }
    if (!found) return 0;
    cursor = strchr(found, '<');
    if (!cursor) return 0;
    ++cursor;
    while (*cursor && *cursor != '>' && count < capacity) {
        int hi, lo;
        if (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' ||
            *cursor == '\t') {
            ++cursor;
            continue;
        }
        hi = isdigit((unsigned char)cursor[0])
                 ? cursor[0] - '0'
                 : (tolower((unsigned char)cursor[0]) >= 'a'
                        ? tolower((unsigned char)cursor[0]) - 'a' + 10
                        : -1);
        lo = isdigit((unsigned char)cursor[1])
                 ? cursor[1] - '0'
                 : (tolower((unsigned char)cursor[1]) >= 'a'
                        ? tolower((unsigned char)cursor[1]) - 'a' + 10
                        : -1);
        if (hi < 0 || lo < 0) return 0;
        out[count++] = (uint8_t)((hi << 4) | lo);
        cursor += 2;
    }
    *out_size = count;
    return count > 0u;
}

static long pdf_parse_int_field(const char *dict, size_t size, const char *tag,
                                long fallback)
{
    size_t tag_size = strlen(tag);
    size_t position = 0u;
    const char *found = NULL;
    char *end = NULL;
    long value;
    while (position + tag_size <= size) {
        const char *candidate =
            pdf_memmem(dict + position, size - position, tag, tag_size);
        size_t after;
        if (!candidate) return fallback;
        after = (size_t)(candidate - dict) + tag_size;
        while (after < size && isspace((unsigned char)dict[after])) ++after;
        if (after < size && (isdigit((unsigned char)dict[after]) ||
                             dict[after] == '-' || dict[after] == '+')) {
            found = candidate;
            break;
        }
        position = (size_t)(candidate - dict) + 1u;
    }
    if (!found) return fallback;
    value = strtol(found + tag_size, &end, 10);
    if (end == found + tag_size) return fallback;
    return value;
}

static int pdf_bool_field(const char *dict, size_t size, const char *key,
                          int fallback)
{
    const char *found = pdf_memmem(dict, size, key, strlen(key));
    size_t position;
    if (!found) return fallback;
    position = (size_t)(found - dict) + strlen(key);
    while (position < size && isspace((unsigned char)dict[position])) ++position;
    if (position + 5u <= size && memcmp(dict + position, "false", 5u) == 0) {
        return 0;
    }
    if (position + 4u <= size && memcmp(dict + position, "true", 4u) == 0) {
        return 1;
    }
    return fallback;
}

static int pdf_r6_hash(const uint8_t *password, size_t password_size,
                       const uint8_t salt[8], const uint8_t *udata,
                       size_t udata_size, int revision, uint8_t out[32]);
static int pdf_aes256_cbc_decrypt_raw(const uint8_t key[32],
                                      const uint8_t iv[16],
                                      uint8_t *data, size_t size);

/* Copy a direct dictionary that starts with "<<" up to its matching ">>".
 * Mutool (among others) writes the encryption dictionary inline in the
 * trailer instead of as an indirect object. */
static int pdf_direct_dictionary(const char *start, size_t size,
                                 const char **out, size_t *out_size)
{
    size_t position = 2u;
    int depth = 1;
    if (!start || size < 4u || start[0] != '<' || start[1] != '<') return 0;
    while (position + 1u < size) {
        if (start[position] == '<' && start[position + 1u] == '<') {
            ++depth;
            position += 2u;
            continue;
        }
        if (start[position] == '>' && start[position + 1u] == '>') {
            --depth;
            position += 2u;
            if (depth == 0) {
                *out = start;
                *out_size = position;
                return 1;
            }
            continue;
        }
        ++position;
    }
    return 0;
}

static int pdf_setup_encryption(QwPdfReader *reader)
{
    const char *found;
    size_t encrypt_object = 0u;
    const char *dict = NULL;
    size_t dict_size = 0u;
    char *copy;
    uint8_t o_value[48], u_value[48], id_value[64];
    size_t o_size = 0u, u_size = 0u, id_size = 0u;
    long revision, version, length_bits, permissions;
    int aes = 0;
    int encrypt_metadata = 1;
    size_t key_size;
    uint8_t md5_input[128];
    uint8_t digest[16];
    size_t md5_size;
    size_t i;
    found = pdf_memmem((const char *)reader->data, reader->size, "/Encrypt",
                       8u);
    if (!found) return 1;
    {
        const char *cursor = found + 8u;
        size_t offset = (size_t)(cursor - (const char *)reader->data);
        while (offset < reader->size &&
               isspace((unsigned char)reader->data[offset])) {
            ++cursor;
            ++offset;
        }
        if (offset + 1u < reader->size && cursor[0] == '<' && cursor[1] == '<') {
            if (!pdf_direct_dictionary(cursor, reader->size - offset, &dict,
                                       &dict_size)) {
                return -1;
            }
        } else {
            char *end = NULL;
            encrypt_object = (size_t)strtoul(cursor, &end, 10);
            if (!encrypt_object) return -1;
            if (!pdf_object_body(reader, encrypt_object, &dict, &dict_size)) {
                return -1;
            }
        }
    }
    copy = (char *)malloc(dict_size + 1u);
    if (!copy) return -1;
    memcpy(copy, dict, dict_size);
    copy[dict_size] = '\0';
    version = pdf_parse_int_field(copy, dict_size, "/V", -1);
    revision = pdf_parse_int_field(copy, dict_size, "/R", -1);
    length_bits = pdf_parse_int_field(copy, dict_size, "/Length", 40);
    permissions = pdf_parse_int_field(copy, dict_size, "/P", 0);
    aes = pdf_memmem(copy, dict_size, "AESV2", 5u) != NULL;
    encrypt_metadata = pdf_bool_field(copy, dict_size, "/EncryptMetadata", 1);
    if (version == 5 && (revision == 5 || revision == 6)) {
        /* PDF 2.0 AES-256 (/V 5, /R 5 or 6, /AESV3). Only the empty user
         * password is attempted, matching the RC4 path's behaviour. */
        uint8_t oe_value[32], ue_value[32];
        uint8_t intermediate[32];
        uint8_t zero_iv[16] = {0};
        size_t oe_size = 0u, ue_size = 0u;
        if (!pdf_memmem(copy, dict_size, "AESV3", 5u) ||
            !pdf_parse_hex_field(copy, dict_size, "/O", o_value,
                                 sizeof(o_value), &o_size) ||
            !pdf_parse_hex_field(copy, dict_size, "/U", u_value,
                                 sizeof(u_value), &u_size) ||
            !pdf_parse_hex_field(copy, dict_size, "/OE", oe_value,
                                 sizeof(oe_value), &oe_size) ||
            !pdf_parse_hex_field(copy, dict_size, "/UE", ue_value,
                                 sizeof(ue_value), &ue_size) ||
            o_size < 48u || u_size < 48u || oe_size < 32u || ue_size < 32u) {
            free(copy);
            return -1;
        }
        free(copy);
        (void)oe_value;
        (void)oe_size; /* owner password is not tried */
        if (!pdf_r6_hash(NULL, 0u, u_value + 32u, NULL, 0u, (int)revision,
                         intermediate) ||
            memcmp(intermediate, u_value, 32u) != 0) {
            return -1;
        }
        if (!pdf_r6_hash(NULL, 0u, u_value + 40u, NULL, 0u, (int)revision,
                         intermediate)) {
            return -1;
        }
        memcpy(reader->encryption_key, ue_value, 32u);
        if (!pdf_aes256_cbc_decrypt_raw(intermediate, zero_iv,
                                        reader->encryption_key, 32u)) {
            return -1;
        }
        reader->encryption_key_size = 32u;
        reader->encryption_aes256 = 1;
        reader->encrypted = 1;
        return 0;
    }
    if (!pdf_parse_hex_field(copy, dict_size, "/O", o_value, sizeof(o_value),
                             &o_size) ||
        !pdf_parse_hex_field(copy, dict_size, "/U", u_value, sizeof(u_value),
                             &u_size) ||
        !((version == 1 && revision == 2) ||
          (version == 2 && (revision == 3 || revision == 4)) ||
          (version == 4 && revision == 4)) ||
        o_size < 32u || u_size < 32u) {
        free(copy);
        return -1;
    }
    free(copy);
    if (length_bits < 40 || length_bits > 128 || (length_bits % 8) != 0)
        return -1;
    /* AESV2 (PDF 1.6 /V 4) always derives a 128-bit file key. */
    if (aes && length_bits != 128) return -1;
    key_size = (size_t)length_bits / 8u;
    if (key_size > sizeof(reader->encryption_key)) return -1;
    found = pdf_memmem((const char *)reader->data, reader->size, "/ID", 3u);
    if (!found ||
        !pdf_parse_hex_field(found, reader->size - (size_t)(found - (const char *)reader->data),
                             "<", id_value, sizeof(id_value), &id_size) ||
        id_size < 16u)
        return -1;
    memcpy(md5_input, pdf_password_padding, 32u);
    memcpy(md5_input + 32u, o_value, 32u);
    md5_input[64] = (uint8_t)(permissions & 0xff);
    md5_input[65] = (uint8_t)((permissions >> 8) & 0xff);
    md5_input[66] = (uint8_t)((permissions >> 16) & 0xff);
    md5_input[67] = (uint8_t)((permissions >> 24) & 0xff);
    memcpy(md5_input + 68u, id_value, 16u);
    md5_size = 84u;
    /* ISO 32000-1 algorithm 2 step (f): metadata is not encrypted. */
    if (revision >= 4 && !encrypt_metadata) {
        md5_input[84] = 0xffu;
        md5_input[85] = 0xffu;
        md5_input[86] = 0xffu;
        md5_input[87] = 0xffu;
        md5_size = 88u;
    }
    if (qw_crypto_md5_digest(md5_input, md5_size, digest) != 0) return -1;
    if (revision >= 3) {
        for (i = 0u; i < 50u; ++i) {
            if (qw_crypto_md5_digest(digest, key_size, digest) != 0) return -1;
        }
    }
    memcpy(reader->encryption_key, digest, key_size);
    reader->encryption_key_size = key_size;
    {
        uint8_t check[32];
        memcpy(check, pdf_password_padding, 32u);
        if (revision == 2) {
            pdf_rc4(reader->encryption_key, key_size, check, 32u);
            if (memcmp(check, u_value, 32u) != 0) return -1;
        } else {
            uint8_t x[16];
            memcpy(md5_input, pdf_password_padding, 32u);
            memcpy(md5_input + 32u, id_value, 16u);
            if (qw_crypto_md5_digest(md5_input, 48u, x) != 0) return -1;
            pdf_rc4(reader->encryption_key, key_size, x, 16u);
            /* ISO 32000-1 Algorithm 5: each round uses key XOR round index,
             * not the original file key. */
            for (i = 1u; i < 20u; ++i) {
                uint8_t round_key[16];
                size_t byte_index;
                for (byte_index = 0u; byte_index < key_size; ++byte_index) {
                    round_key[byte_index] =
                        (uint8_t)(reader->encryption_key[byte_index] ^
                                  (uint8_t)i);
                }
                pdf_rc4(round_key, key_size, x, 16u);
            }
            if (memcmp(x, u_value, 16u) != 0) return -1;
        }
    }
    reader->encrypted = 1;
    reader->encryption_aes = aes;
    return 0;
}

/* AES-CBC block decryption for both /AESV2 (128-bit object keys) and
 * /AESV3 (256-bit file key). */
static int pdf_aes_block_decrypt(int key_bits, const uint8_t *key,
                                 uint8_t block[16])
{
    if (key_bits == 128) {
        return qw_crypto_aes128_ecb_decrypt(key, block, 16u) == QW_CRYPTO_OK;
    }
    if (key_bits == 256) {
        return qw_crypto_aes256_ecb_decrypt(key, block, 16u) == QW_CRYPTO_OK;
    }
    return 0;
}

/* AES-CBC with a 16-byte IV prefix and PKCS#5 padding, used by /AESV2 and
 * /AESV3 stream and string encryption. Decrypts in place and reports the
 * unpadded size. */
static int pdf_aes_cbc_decrypt(int key_bits, const uint8_t *key,
                               uint8_t *data, size_t *size)
{
    uint8_t previous[16];
    uint8_t cipher[16];
    size_t offset;
    uint8_t padding;
    size_t index;
    if (!size || *size < 32u || (*size % 16u) != 0u) return 0;
    memcpy(previous, data, 16u);
    for (offset = 16u; offset < *size; offset += 16u) {
        size_t byte_index;
        memcpy(cipher, data + offset, 16u);
        if (!pdf_aes_block_decrypt(key_bits, key, data + offset)) return 0;
        for (byte_index = 0u; byte_index < 16u; ++byte_index) {
            data[offset + byte_index] ^= previous[byte_index];
        }
        memcpy(previous, cipher, 16u);
    }
    /* Drop the IV prefix so callers see the plaintext at offset 0. */
    memmove(data, data + 16u, *size - 16u);
    *size -= 16u;
    padding = data[*size - 1u];
    if (padding == 0u || padding > 16u) return 0;
    for (index = 0u; index < padding; ++index) {
        if (data[*size - 1u - index] != padding) return 0;
    }
    *size -= padding;
    return 1;
}

/* Raw AES-256-CBC decryption without IV prefix or padding (/UE, /OE). */
static int pdf_aes256_cbc_decrypt_raw(const uint8_t key[32],
                                      const uint8_t iv[16],
                                      uint8_t *data, size_t size)
{
    uint8_t previous[16];
    size_t offset;
    if (!data || !size || (size % 16u) != 0u) return 0;
    memcpy(previous, iv, 16u);
    for (offset = 0u; offset < size; offset += 16u) {
        uint8_t cipher[16];
        size_t index;
        memcpy(cipher, data + offset, 16u);
        if (qw_crypto_aes256_ecb_decrypt(key, data + offset, 16u) !=
            QW_CRYPTO_OK) {
            return 0;
        }
        for (index = 0u; index < 16u; ++index) {
            data[offset + index] ^= previous[index];
        }
        memcpy(previous, cipher, 16u);
    }
    return 1;
}

/* AES-128-CBC encryption without padding, used by the revision 6 hash. */
static int pdf_aes128_cbc_encrypt(const uint8_t key[16], const uint8_t iv[16],
                                  const uint8_t *input, uint8_t *output,
                                  size_t size)
{
    uint8_t previous[16];
    size_t offset;
    if (!input || !output || (size % 16u) != 0u) return 0;
    memcpy(previous, iv, 16u);
    for (offset = 0u; offset < size; offset += 16u) {
        uint8_t block[16];
        size_t index;
        for (index = 0u; index < 16u; ++index) {
            block[index] = input[offset + index] ^ previous[index];
        }
        if (qw_crypto_aes128_ecb_encrypt(key, block, 16u) != QW_CRYPTO_OK) {
            return 0;
        }
        memcpy(output + offset, block, 16u);
        memcpy(previous, block, 16u);
    }
    return 1;
}

/* ISO 32000-2 algorithm 2.B: /R 5 and /R 6 password hash. The empty user
 * password is the only credential this reader tries; udata carries /U for the
 * owner-password variant. */
static int pdf_r6_hash(const uint8_t *password, size_t password_size,
                       const uint8_t salt[8], const uint8_t *udata,
                       size_t udata_size, int revision, uint8_t out[32])
{
    uint8_t k[64];
    size_t k_size = 32u;
    uint8_t *seed = NULL;
    uint8_t *k1 = NULL;
    uint8_t *encrypted = NULL;
    size_t seed_size = password_size + 8u + udata_size;
    size_t max_size = (password_size + 64u + udata_size) * 64u;
    size_t iterations = 0u;
    int ok = 0;

    seed = (uint8_t *)malloc(seed_size ? seed_size : 1u);
    if (!seed) return 0;
    if (password_size) memcpy(seed, password, password_size);
    memcpy(seed + password_size, salt, 8u);
    if (udata_size) memcpy(seed + password_size + 8u, udata, udata_size);
    if (qw_crypto_sha256(seed, seed_size, k) != QW_CRYPTO_OK) goto cleanup;
    if (revision == 5) {
        memcpy(out, k, 32u);
        ok = 1;
        goto cleanup;
    }
    k1 = (uint8_t *)malloc(max_size);
    encrypted = (uint8_t *)malloc(max_size);
    if (!k1 || !encrypted) goto cleanup;
    for (;;) {
        size_t copy;
        size_t offset = 0u;
        unsigned int remainder = 0u;
        size_t index;
        for (copy = 0u; copy < 64u; ++copy) {
            if (password_size) {
                memcpy(k1 + offset, password, password_size);
                offset += password_size;
            }
            memcpy(k1 + offset, k, k_size);
            offset += k_size;
            if (udata_size) {
                memcpy(k1 + offset, udata, udata_size);
                offset += udata_size;
            }
        }
        if (!pdf_aes128_cbc_encrypt(k, k + 16u, k1, encrypted, offset)) {
            goto cleanup;
        }
        for (index = 0u; index < 16u; ++index) {
            remainder = (remainder * 256u + encrypted[index]) % 3u;
        }
        if (remainder == 0u) {
            if (qw_crypto_sha256(encrypted, offset, k) != QW_CRYPTO_OK) {
                goto cleanup;
            }
            k_size = 32u;
        } else if (remainder == 1u) {
            if (qw_crypto_sha384(encrypted, offset, k) != QW_CRYPTO_OK) {
                goto cleanup;
            }
            k_size = 48u;
        } else {
            if (qw_crypto_sha512(encrypted, offset, k) != QW_CRYPTO_OK) {
                goto cleanup;
            }
            k_size = 64u;
        }
        ++iterations;
        if (iterations >= 64u &&
            (int)encrypted[offset - 1u] <= (int)iterations - 32) {
            break;
        }
        if (iterations >= 2048u) break; /* defensive bound */
    }
    memcpy(out, k, 32u);
    ok = 1;
cleanup:
    free(seed);
    free(k1);
    free(encrypted);
    return ok;
}

static int pdf_decrypt_object_data(const QwPdfReader *reader, size_t object,
                                   uint8_t *data, size_t *size)
{
    uint8_t extra[5];
    uint8_t digest[16];
    uint8_t input[32 + 5 + 4];
    size_t digest_size;
    if (!reader->encrypted || !size || !*size) return 1;
    if (reader->encryption_aes256) {
        /* /AESV3 uses the file encryption key directly for every stream. */
        return pdf_aes_cbc_decrypt(256, reader->encryption_key, data, size);
    }
    extra[0] = (uint8_t)(object & 0xffu);
    extra[1] = (uint8_t)((object >> 8) & 0xffu);
    extra[2] = (uint8_t)((object >> 16) & 0xffu);
    extra[3] = 0u;
    extra[4] = 0u;
    digest_size = reader->encryption_key_size + 5u;
    if (digest_size > sizeof(digest)) digest_size = sizeof(digest);
    memcpy(input, reader->encryption_key, reader->encryption_key_size);
    memcpy(input + reader->encryption_key_size, extra, 5u);
    if (reader->encryption_aes) {
        uint8_t object_key[16];
        /* ISO 32000-1 algorithm 1 step (b): AES object keys append "sAlT". */
        memcpy(input + reader->encryption_key_size + 5u, "sAlT", 4u);
        if (qw_crypto_md5_digest(input, reader->encryption_key_size + 9u,
                                 digest) != 0) {
            return 0;
        }
        memcpy(object_key, digest, digest_size);
        return pdf_aes_cbc_decrypt(128, object_key, data, size);
    }
    {
        uint8_t final_digest[16];
        if (qw_crypto_md5_digest(input, reader->encryption_key_size + 5u,
                                 final_digest) != 0) {
            return 0;
        }
        memcpy(digest, final_digest, sizeof(digest));
    }
    pdf_rc4(digest, digest_size, data, *size);
    return 1;
}

static uint8_t *pdf_object_stream(const QwPdfReader *reader, size_t object,
                                  size_t *out_size)
{
    const char *body;
    size_t body_size;
    const char *payload;
    size_t payload_size;
    int flate = 0;
    uint8_t *output;

    *out_size = 0u;
    if (!pdf_object_body(reader, object, &body, &body_size)) return NULL;
    if (!pdf_stream_extent(body, body_size, &payload, &payload_size, &flate)) {
        return NULL;
    }
    if (!payload_size || payload_size > QW_PDF_READER_MAX_STREAM) return NULL;
    if (reader->encrypted) {
        uint8_t *decrypted = (uint8_t *)malloc(payload_size);
        if (!decrypted) return NULL;
        memcpy(decrypted, payload, payload_size);
        if (!pdf_decrypt_object_data(reader, object, decrypted,
                                     &payload_size)) {
            free(decrypted);
            return NULL;
        }
        if (!flate) {
            *out_size = payload_size;
            return decrypted;
        }
        {
            size_t capacity = payload_size * 4u + 1024u;
            size_t attempt;
            for (attempt = 0u; attempt < 4u &&
                               capacity <= QW_PDF_READER_MAX_STREAM;
                 ++attempt) {
                size_t produced = 0u;
                QwCompressionInflateStatus status;
                output = (uint8_t *)malloc(capacity);
                if (!output) {
                    free(decrypted);
                    return NULL;
                }
                status = qw_compression_inflate_zlib_bounded(
                    decrypted, payload_size, output, capacity, &produced);
                if (status == QW_COMPRESSION_INFLATE_OK) {
                    free(decrypted);
                    *out_size = produced;
                    return output;
                }
                free(output);
                if (status != QW_COMPRESSION_INFLATE_OUTPUT_FULL) {
                    free(decrypted);
                    return NULL;
                }
                capacity *= 2u;
            }
            free(decrypted);
            return NULL;
        }
    }
    if (!flate) {
        output = (uint8_t *)malloc(payload_size);
        if (!output) return NULL;
        memcpy(output, payload, payload_size);
        *out_size = payload_size;
        return output;
    }
    {
        size_t capacity = payload_size * 4u + 1024u;
        size_t attempt;
        for (attempt = 0u; attempt < 4u && capacity <= QW_PDF_READER_MAX_STREAM; ++attempt) {
            size_t produced = 0u;
            QwCompressionInflateStatus status;
            output = (uint8_t *)malloc(capacity);
            if (!output) return NULL;
            status = qw_compression_inflate_zlib_bounded(
                payload, payload_size, output, capacity, &produced);
            if (status == QW_COMPRESSION_INFLATE_OK) {
                *out_size = produced;
                return output;
            }
            free(output);
            if (status != QW_COMPRESSION_INFLATE_OUTPUT_FULL) return NULL;
            capacity *= 4u;
        }
    }
    return NULL;
}

static int pdf_object_is_type0(const QwPdfReader *reader, size_t object)
{
    const char *body;
    size_t body_size;
    size_t index = 0u;
    if (!pdf_object_body(reader, object, &body, &body_size)) return 0;
    while (index + 8u < body_size) {
        if (body[index] == '/' && memcmp(body + index, "/Subtype", 8u) == 0 &&
            pdf_name_is(body + index + 8u, body_size - index - 8u, "Type0")) {
            return 1;
        }
        ++index;
    }
    return 0;
}

/* Map a font resource name (e.g. "/F2") to a Type0 flag and its ToUnicode
 * CMap. The CMap is the only reliable way to turn glyph codes into text for
 * embedded CID fonts, so it is parsed once per font and used while decoding
 * content strings. */
typedef struct QwPdfToUnicodeEntry {
    unsigned int code;
    unsigned int utf16[4];
    size_t utf16_length;
} QwPdfToUnicodeEntry;

typedef struct QwPdfFontMap {
    char names[32][24];
    int type0[32];
    QwPdfToUnicodeEntry *entries[32];
    size_t entry_count[32];
    size_t code_bytes[32];
    size_t count;
} QwPdfFontMap;

/* ------------------------------------------------------------------ */
/* ToUnicode CMaps                                                     */
/* ------------------------------------------------------------------ */

#define QW_PDF_TOUNICODE_MAX 4096u

/* Read one "<hex>" token into bytes. Returns 0 when no token starts here. */
static int pdf_cmap_hex(const char *text, size_t size, size_t *position,
                        uint8_t *out, size_t capacity, size_t *out_size)
{
    size_t index = *position;
    size_t count = 0u;
    int high = -1;
    while (index < size && isspace((unsigned char)text[index])) ++index;
    if (index >= size || text[index] != '<') return 0;
    ++index;
    while (index < size && text[index] != '>') {
        char value = text[index++];
        int digit;
        if (value >= '0' && value <= '9') digit = value - '0';
        else if (value >= 'a' && value <= 'f') digit = value - 'a' + 10;
        else if (value >= 'A' && value <= 'F') digit = value - 'A' + 10;
        else if (isspace((unsigned char)value)) continue;
        else return 0;
        if (high < 0) {
            high = digit;
        } else {
            if (count < capacity) out[count++] = (uint8_t)((high << 4) | digit);
            high = -1;
        }
    }
    if (index >= size) return 0;
    *position = index + 1u;
    if (high >= 0 && count < capacity) out[count++] = (uint8_t)(high << 4);
    *out_size = count;
    return count > 0u;
}

static unsigned int pdf_cmap_code(const uint8_t *bytes, size_t size)
{
    unsigned int value = 0u;
    size_t index;
    if (size > 4u) size = 4u;
    for (index = 0u; index < size; ++index) value = (value << 8) | bytes[index];
    return value;
}

static void pdf_cmap_utf16(const uint8_t *bytes, size_t size,
                           QwPdfToUnicodeEntry *entry)
{
    size_t units = size / 2u;
    size_t index;
    if (units > 4u) units = 4u;
    entry->utf16_length = units;
    for (index = 0u; index < units; ++index) {
        entry->utf16[index] =
            ((unsigned int)bytes[index * 2u] << 8) | bytes[index * 2u + 1u];
    }
}

static int pdf_tounicode_add(QwPdfToUnicodeEntry **entries, size_t *count,
                             size_t *capacity, size_t *code_bytes,
                             size_t code_size, unsigned int code,
                             const uint8_t *dst, size_t dst_size)
{
    QwPdfToUnicodeEntry *grown;
    if (*count >= QW_PDF_TOUNICODE_MAX) return 0;
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2u : 32u;
        if (next > QW_PDF_TOUNICODE_MAX) next = QW_PDF_TOUNICODE_MAX;
        grown = (QwPdfToUnicodeEntry *)realloc(
            *entries, next * sizeof(**entries));
        if (!grown) return 0;
        *entries = grown;
        *capacity = next;
    }
    (*entries)[*count].code = code;
    pdf_cmap_utf16(dst, dst_size, &(*entries)[*count]);
    *count += 1u;
    if (code_size > *code_bytes) *code_bytes = code_size;
    return 1;
}

static int pdf_tounicode_compare(const void *left, const void *right)
{
    const QwPdfToUnicodeEntry *a = (const QwPdfToUnicodeEntry *)left;
    const QwPdfToUnicodeEntry *b = (const QwPdfToUnicodeEntry *)right;
    if (a->code < b->code) return -1;
    if (a->code > b->code) return 1;
    return 0;
}

static const QwPdfToUnicodeEntry *pdf_tounicode_lookup(
    const QwPdfToUnicodeEntry *entries, size_t count, unsigned int code)
{
    size_t low = 0u;
    size_t high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (entries[middle].code == code) {
            while (middle > 0u && entries[middle - 1u].code == code) --middle;
            return &entries[middle];
        }
        if (entries[middle].code < code) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    return NULL;
}

static void pdf_parse_tounicode(const char *cmap, size_t size,
                                QwPdfToUnicodeEntry **out_entries,
                                size_t *out_count, size_t *out_code_bytes)
{
    QwPdfToUnicodeEntry *entries = NULL;
    size_t count = 0u;
    size_t capacity = 0u;
    size_t code_bytes = 0u;
    *out_entries = NULL;
    *out_count = 0u;
    *out_code_bytes = 0u;
    {
        const char *begin = pdf_memmem(cmap, size, "begincodespacerange", 19u);
        const char *end = pdf_memmem(cmap, size, "endcodespacerange", 17u);
        if (begin && end && end > begin) {
            size_t cursor = (size_t)(begin - cmap) + 19u;
            size_t limit = (size_t)(end - cmap);
            while (cursor < limit) {
                uint8_t low[4], high[4];
                size_t low_size = 0u, high_size = 0u;
                if (!pdf_cmap_hex(cmap, limit, &cursor, low, sizeof(low),
                                  &low_size)) {
                    break;
                }
                if (!pdf_cmap_hex(cmap, limit, &cursor, high, sizeof(high),
                                  &high_size)) {
                    break;
                }
                (void)high_size;
                if (low_size > code_bytes) code_bytes = low_size;
            }
        }
    }
    {
        const char *begin = pdf_memmem(cmap, size, "beginbfchar", 11u);
        const char *end = pdf_memmem(cmap, size, "endbfchar", 9u);
        if (begin && end && end > begin) {
            size_t cursor = (size_t)(begin - cmap) + 11u;
            size_t limit = (size_t)(end - cmap);
            while (cursor < limit) {
                uint8_t source[4], destination[64];
                size_t source_size = 0u, destination_size = 0u;
                if (!pdf_cmap_hex(cmap, limit, &cursor, source,
                                  sizeof(source), &source_size)) {
                    break;
                }
                if (!pdf_cmap_hex(cmap, limit, &cursor, destination,
                                  sizeof(destination), &destination_size)) {
                    ++cursor;
                    continue;
                }
                if (!pdf_tounicode_add(&entries, &count, &capacity,
                                       &code_bytes, source_size,
                                       pdf_cmap_code(source, source_size),
                                       destination, destination_size)) {
                    break;
                }
            }
        }
    }
    {
        const char *begin = pdf_memmem(cmap, size, "beginbfrange", 12u);
        const char *end = pdf_memmem(cmap, size, "endbfrange", 10u);
        if (begin && end && end > begin) {
            size_t cursor = (size_t)(begin - cmap) + 12u;
            size_t limit = (size_t)(end - cmap);
            while (cursor < limit) {
                uint8_t low[4], high[4], destination[64];
                size_t low_size = 0u, high_size = 0u, destination_size = 0u;
                size_t probe;
                unsigned int first;
                unsigned int last;
                if (!pdf_cmap_hex(cmap, limit, &cursor, low, sizeof(low),
                                  &low_size)) {
                    break;
                }
                if (!pdf_cmap_hex(cmap, limit, &cursor, high, sizeof(high),
                                  &high_size)) {
                    break;
                }
                first = pdf_cmap_code(low, low_size);
                last = pdf_cmap_code(high, high_size);
                if (last < first) continue;
                probe = cursor;
                while (probe < limit && isspace((unsigned char)cmap[probe])) {
                    ++probe;
                }
                if (probe < limit && cmap[probe] == '[') {
                    unsigned int code = first;
                    cursor = probe + 1u;
                    while (code <= last && cursor < limit) {
                        if (!pdf_cmap_hex(cmap, limit, &cursor, destination,
                                          sizeof(destination),
                                          &destination_size)) {
                            break;
                        }
                        if (!pdf_tounicode_add(&entries, &count, &capacity,
                                               &code_bytes, low_size, code,
                                               destination,
                                               destination_size)) {
                            break;
                        }
                        ++code;
                    }
                    continue;
                }
                if (!pdf_cmap_hex(cmap, limit, &cursor, destination,
                                  sizeof(destination), &destination_size)) {
                    break;
                }
                {
                    unsigned int code = first;
                    while (code <= last) {
                        uint8_t adjusted[64];
                        memcpy(adjusted, destination, destination_size);
                        if (destination_size >= 2u && code != first) {
                            unsigned int unit =
                                ((unsigned int)adjusted[destination_size - 2u]
                                 << 8) |
                                adjusted[destination_size - 1u];
                            unit = (unit + (code - first)) & 0xFFFFu;
                            adjusted[destination_size - 2u] =
                                (uint8_t)(unit >> 8);
                            adjusted[destination_size - 1u] = (uint8_t)unit;
                        }
                        if (!pdf_tounicode_add(&entries, &count, &capacity,
                                               &code_bytes, low_size, code,
                                               adjusted,
                                               destination_size)) {
                            break;
                        }
                        ++code;
                    }
                }
            }
        }
    }
    if (count) {
        qsort(entries, count, sizeof(*entries), pdf_tounicode_compare);
    }
    *out_entries = entries;
    *out_count = count;
    *out_code_bytes = code_bytes;
}

/* Resolve a font's /ToUnicode stream and parse it. Returns 0 when absent. */
static int pdf_load_tounicode(const QwPdfReader *reader, size_t object,
                              QwPdfToUnicodeEntry **out_entries,
                              size_t *out_count, size_t *out_code_bytes)
{
    const char *body;
    size_t body_size;
    const char *key;
    size_t reference = 0u;
    uint8_t *cmap;
    size_t cmap_size = 0u;
    *out_entries = NULL;
    *out_count = 0u;
    *out_code_bytes = 0u;
    if (!pdf_object_body(reader, object, &body, &body_size)) return 0;
    key = pdf_memmem(body, body_size, "/ToUnicode", 10u);
    if (!key) return 0;
    {
        const char *scan = key + 10u;
        const char *limit = body + body_size;
        while (scan < limit && isspace((unsigned char)*scan)) ++scan;
        while (scan < limit && isdigit((unsigned char)*scan)) {
            reference = reference * 10u + (size_t)(*scan - '0');
            ++scan;
        }
    }
    if (!reference) return 0;
    cmap = pdf_object_stream(reader, reference, &cmap_size);
    if (!cmap) return 0;
    pdf_parse_tounicode((const char *)cmap, cmap_size, out_entries, out_count,
                        out_code_bytes);
    free(cmap);
    return *out_count > 0u;
}

static void pdf_free_font_maps(QwPdfFontMap *fonts)
{
    size_t index;
    for (index = 0u; index < fonts->count; ++index) {
        free(fonts->entries[index]);
        fonts->entries[index] = NULL;
        fonts->entry_count[index] = 0u;
    }
}

static void pdf_collect_fonts(const QwPdfReader *reader, const char *dict,
                              size_t dict_size, QwPdfFontMap *fonts)
{
    const char *font = pdf_memmem(dict, dict_size, "/Font", 5u);
    const char *cursor;
    const char *end;
    fonts->count = 0u;
    if (!font) return;
    cursor = pdf_memmem(font, dict_size - (size_t)(font - dict), "<<", 2u);
    if (!cursor) return;
    cursor += 2;
    /* The font resource dictionary ends at the next ">>"; stop there so page
     * keys such as /Contents are not mistaken for font names. */
    {
        const char *font_end = pdf_memmem(cursor, dict_size - (size_t)(cursor - dict),
                                          ">>", 2u);
        end = font_end ? font_end : dict + dict_size;
    }
    while (cursor < end && fonts->count < 32u) {
        const char *slash = memchr(cursor, '/', (size_t)(end - cursor));
        size_t name_length;
        size_t object = 0u;
        const char *scan;
        if (!slash) break;
        name_length = 0u;
        while (slash + 1u + name_length < end &&
               (isalnum((unsigned char)slash[1u + name_length]) ||
                slash[1u + name_length] == '_' || slash[1u + name_length] == '-')) {
            ++name_length;
        }
        if (!name_length || name_length >= sizeof(fonts->names[0])) { ++cursor; continue; }
        scan = slash + 1u + name_length;
        while (scan < end && isspace((unsigned char)*scan)) ++scan;
        if (scan < end && isdigit((unsigned char)*scan)) {
            while (scan < end && isdigit((unsigned char)*scan)) {
                object = object * 10u + (size_t)(*scan - '0');
                ++scan;
            }
            memcpy(fonts->names[fonts->count], slash, name_length + 1u);
            fonts->names[fonts->count][name_length + 1u] = '\0';
            fonts->type0[fonts->count] = pdf_object_is_type0(reader, object);
            fonts->entries[fonts->count] = NULL;
            fonts->entry_count[fonts->count] = 0u;
            {
                size_t code_bytes = 0u;
                int has_map = pdf_load_tounicode(
                    reader, object, &fonts->entries[fonts->count],
                    &fonts->entry_count[fonts->count], &code_bytes);
                fonts->code_bytes[fonts->count] =
                    has_map && code_bytes
                        ? code_bytes
                        : (fonts->type0[fonts->count] ? 2u : 1u);
            }
            fonts->count += 1u;
        }
        cursor = slash + 1u;
    }
}

/* Index of a font resource name, or SIZE_MAX when it is unknown. */
static size_t pdf_font_index(const QwPdfFontMap *fonts, const char *name)
{
    size_t index;
    if (!fonts || !name) return SIZE_MAX;
    for (index = 0u; index < fonts->count; ++index) {
        if (strcmp(fonts->names[index], name) == 0) return index;
    }
    return SIZE_MAX;
}

/* Parse a literal string after '(' into bytes. */
static const char *pdf_read_literal(const char *cursor, const char *end,
                                    QwPdfBuffer *out)
{
    int depth = 1;
    while (cursor < end && depth > 0) {
        char value = *cursor++;
        if (value == '\\') {
            char escape;
            if (cursor >= end) break;
            escape = *cursor++;
            switch (escape) {
            case 'n': pdf_buffer_append(out, "\n", 1u); break;
            case 'r': pdf_buffer_append(out, "\r", 1u); break;
            case 't': pdf_buffer_append(out, "\t", 1u); break;
            case 'b': pdf_buffer_append(out, "\b", 1u); break;
            case 'f': pdf_buffer_append(out, "\f", 1u); break;
            case '(': pdf_buffer_append(out, "(", 1u); break;
            case ')': pdf_buffer_append(out, ")", 1u); break;
            case '\\': pdf_buffer_append(out, "\\", 1u); break;
            default:
                if (escape >= '0' && escape <= '7') {
                    unsigned int code = (unsigned int)(escape - '0');
                    int extra = 0;
                    unsigned char byte;
                    while (extra < 2 && cursor < end && *cursor >= '0' && *cursor <= '7') {
                        code = code * 8u + (unsigned int)(*cursor - '0');
                        ++cursor;
                        ++extra;
                    }
                    byte = (unsigned char)code;
                    pdf_buffer_append(out, (const char *)&byte, 1u);
                } else {
                    pdf_buffer_append(out, &escape, 1u);
                }
            }
            continue;
        }
        if (value == '(') { ++depth; continue; }
        if (value == ')') { --depth; continue; }
        pdf_buffer_append(out, &value, 1u);
    }
    return cursor;
}

static const char *pdf_read_hex_string(const char *cursor, const char *end,
                                       QwPdfBuffer *out)
{
    int high = -1;
    while (cursor < end && *cursor != '>') {
        char value = *cursor++;
        int digit;
        if (value >= '0' && value <= '9') digit = value - '0';
        else if (value >= 'a' && value <= 'f') digit = value - 'a' + 10;
        else if (value >= 'A' && value <= 'F') digit = value - 'A' + 10;
        else continue;
        if (high < 0) {
            high = digit;
        } else {
            unsigned char byte = (unsigned char)((high << 4) | digit);
            pdf_buffer_append(out, (const char *)&byte, 1u);
            high = -1;
        }
    }
    if (high >= 0) {
        unsigned char byte = (unsigned char)(high << 4);
        pdf_buffer_append(out, (const char *)&byte, 1u);
    }
    return cursor < end ? cursor + 1 : cursor;
}

static int pdf_emit_utf16(QwPdfBuffer *text, const unsigned int *units,
                          size_t count)
{
    size_t index = 0u;
    while (index < count) {
        unsigned int code = units[index++];
        if (code >= 0xD800u && code <= 0xDBFFu && index < count &&
            units[index] >= 0xDC00u && units[index] <= 0xDFFFu) {
            code = 0x10000u + ((code - 0xD800u) << 10) +
                   (units[index] - 0xDC00u);
            ++index;
        }
        if (!pdf_buffer_append_utf8(text, code)) return 0;
    }
    return 1;
}

static int pdf_emit_string(QwPdfBuffer *text, const QwPdfBuffer *raw, int type0,
                           const QwPdfToUnicodeEntry *entries,
                           size_t entry_count, size_t code_bytes)
{
    size_t index = 0u;
    if (entries && entry_count) {
        size_t step = code_bytes ? code_bytes : (type0 ? 2u : 1u);
        if (step > 4u) step = 4u;
        while (index + step <= raw->size) {
            unsigned int code = pdf_cmap_code(
                (const uint8_t *)raw->data + index, step);
            const QwPdfToUnicodeEntry *entry =
                pdf_tounicode_lookup(entries, entry_count, code);
            if (entry && entry->utf16_length) {
                if (!pdf_emit_utf16(text, entry->utf16,
                                    entry->utf16_length)) {
                    return 0;
                }
            }
            index += step;
        }
        return 1;
    }
    if (type0) {
        while (index + 1u < raw->size) {
            unsigned int code = ((unsigned int)(unsigned char)raw->data[index] << 8) |
                                (unsigned int)(unsigned char)raw->data[index + 1u];
            if (code == 0u) { index += 2u; continue; }
            if (!pdf_buffer_append_utf8(text, code)) return 0;
            index += 2u;
        }
        return 1;
    }
    while (index < raw->size) {
        unsigned int code = (unsigned int)(unsigned char)raw->data[index++];
        if (!pdf_buffer_append_utf8(text, code)) return 0;
    }
    return 1;
}

static int pdf_extract_content_text(const QwPdfReader *reader, const char *content,
                                    size_t content_size, const QwPdfFontMap *fonts,
                                    QwPdfBuffer *text)
{
    const char *cursor = content;
    const char *end = content + content_size;
    char font_name[24] = "/F1";
    int type0 = 0;
    size_t font_index = SIZE_MAX;
    const QwPdfToUnicodeEntry *unicode_entries = NULL;
    size_t unicode_count = 0u;
    size_t code_bytes = 0u;
    while (cursor < end) {
        const char *token;
        size_t token_length;
        while (cursor < end && (isspace((unsigned char)*cursor) ||
                                *cursor == '\0')) {
            ++cursor;
        }
        if (cursor >= end) break;
        if (*cursor == '(') {
            QwPdfBuffer raw = {NULL, 0u, 0u};
            cursor = pdf_read_literal(cursor + 1, end, &raw);
            if (!pdf_emit_string(text, &raw, type0, unicode_entries,
                                 unicode_count, code_bytes)) {
                free(raw.data);
                return 0;
            }
            free(raw.data);
            continue;
        }
        if (*cursor == '<' && cursor + 1 < end && cursor[1] != '<') {
            QwPdfBuffer raw = {NULL, 0u, 0u};
            cursor = pdf_read_hex_string(cursor + 1, end, &raw);
            if (!pdf_emit_string(text, &raw, type0, unicode_entries,
                                 unicode_count, code_bytes)) {
                free(raw.data);
                return 0;
            }
            free(raw.data);
            continue;
        }
        if (*cursor == '[' || *cursor == ']' || *cursor == '<' || *cursor == '>') {
            ++cursor;
            continue;
        }
        if (*cursor == '/') {
            size_t length = 0u;
            while (cursor + 1u + length < end &&
                   (isalnum((unsigned char)cursor[1u + length]) ||
                    cursor[1u + length] == '_' || cursor[1u + length] == '-')) {
                ++length;
            }
            if (length + 2u < sizeof(font_name) && length > 0u) {
                memcpy(font_name, cursor, length + 1u);
                font_name[length + 1u] = '\0';
            }
            cursor += 1u + length;
            token = font_name;
            (void)token;
            continue;
        }
        token = cursor;
        token_length = 0u;
        while (cursor + token_length < end &&
               !isspace((unsigned char)cursor[token_length]) &&
               cursor[token_length] != '(' && cursor[token_length] != '<' &&
               cursor[token_length] != '/') {
            ++token_length;
        }
        cursor += token_length ? token_length : 1u;
        if ((token_length == 2u && memcmp(token, "Tf", 2u) == 0)) {
            font_index = pdf_font_index(fonts, font_name);
            if (font_index != SIZE_MAX) {
                type0 = fonts->type0[font_index];
                unicode_entries = fonts->entries[font_index];
                unicode_count = fonts->entry_count[font_index];
                code_bytes = fonts->code_bytes[font_index];
            } else {
                type0 = 0;
                unicode_entries = NULL;
                unicode_count = 0u;
                code_bytes = 0u;
            }
        } else if (token_length == 2u && memcmp(token, "Tj", 2u) == 0) {
            /* handled above; nothing to do */
        } else if (token_length == 2u && memcmp(token, "TJ", 2u) == 0) {
            /* array strings were already emitted */
        } else if (token_length == 2u &&
                   (memcmp(token, "Td", 2u) == 0 || memcmp(token, "TD", 2u) == 0)) {
            if (!pdf_buffer_append(text, "\n", 1u)) return 0;
        } else if (token_length == 2u && memcmp(token, "T*", 2u) == 0) {
            if (!pdf_buffer_append(text, "\n", 1u)) return 0;
        } else if (token_length == 2u && memcmp(token, "Tm", 2u) == 0) {
            if (!pdf_buffer_append(text, "\n", 1u)) return 0;
        } else if (token_length == 1u && (token[0] == '\'' || token[0] == '"')) {
            if (!pdf_buffer_append(text, "\n", 1u)) return 0;
        }
    }
    (void)reader;
    return 1;
}

static int pdf_extract_page_text(const QwPdfReader *reader, size_t page_object,
                                 QwPdfBuffer *text)
{
    const char *body;
    size_t dict_size;
    size_t contents = 0u;
    uint8_t *stream;
    size_t stream_size = 0u;
    QwPdfFontMap fonts;
    char *dict_copy;
    int ok;

    if (!pdf_object_body(reader, page_object, &body, &dict_size)) return 0;
    dict_copy = (char *)malloc(dict_size + 1u);
    if (!dict_copy) return 0;
    memcpy(dict_copy, body, dict_size);
    dict_copy[dict_size] = '\0';
    memset(&fonts, 0, sizeof(fonts));
    pdf_collect_fonts(reader, dict_copy, dict_size, &fonts);
    if (!pdf_find_reference(dict_copy, dict_size, "/Contents", &contents)) {
        pdf_free_font_maps(&fonts);
        free(dict_copy);
        return 1; /* page without content: valid, contributes no text */
    }
    free(dict_copy);
    stream = pdf_object_stream(reader, contents, &stream_size);
    if (!stream) {
        pdf_free_font_maps(&fonts);
        return 0;
    }
    ok = pdf_extract_content_text(reader, (const char *)stream, stream_size,
                                  &fonts, text);
    free(stream);
    pdf_free_font_maps(&fonts);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Page tree walk                                                      */
/*                                                                     */
/* Enumerating pages by their /Type /Page markers yields file order,    */
/* which differs from document order whenever a producer writes the     */
/* objects out of order. The catalog page tree is authoritative, so it  */
/* is walked first and the marker scan only remains as a fallback for   */
/* files without a usable /Root -> /Pages chain.                        */
/* ------------------------------------------------------------------ */

static int pdf_skip_space(const char *text, size_t size, size_t *position)
{
    while (*position < size && isspace((unsigned char)text[*position])) {
        ++*position;
    }
    return *position < size;
}

/* Parse one "num gen R" reference starting at *position and advance past it. */
static int pdf_parse_reference_at(const char *text, size_t size,
                                  size_t *position, size_t *out_object)
{
    size_t index = *position;
    size_t object = 0u;
    size_t generation = 0u;
    if (!pdf_skip_space(text, size, &index)) return 0;
    if (!isdigit((unsigned char)text[index])) return 0;
    while (index < size && isdigit((unsigned char)text[index])) {
        object = object * 10u + (size_t)(text[index] - '0');
        ++index;
    }
    if (!pdf_skip_space(text, size, &index) ||
        !isdigit((unsigned char)text[index])) {
        return 0;
    }
    while (index < size && isdigit((unsigned char)text[index])) {
        generation = generation * 10u + (size_t)(text[index] - '0');
        ++index;
    }
    (void)generation;
    if (!pdf_skip_space(text, size, &index) || text[index] != 'R') return 0;
    if (index + 1u < size) {
        unsigned char next = (unsigned char)text[index + 1u];
        if (isalnum(next) || next == '_' || next == '-') return 0;
    }
    *position = index + 1u;
    *out_object = object;
    return 1;
}

static int pdf_append_page(size_t **objects, size_t *count, size_t *capacity,
                           size_t object)
{
    if (*count >= QW_PDF_READER_MAX_PAGES) return 0;
    if (*count >= *capacity) {
        size_t grown = *capacity ? *capacity * 2u : 16u;
        size_t *resized = (size_t *)realloc(*objects, grown * sizeof(*resized));
        if (!resized) return 0;
        *objects = resized;
        *capacity = grown;
    }
    (*objects)[(*count)++] = object;
    return 1;
}

static int pdf_node_is_branch(const char *body, size_t size)
{
    const char *kids;
    if (pdf_find_typed(body, size, "Pages")) return 1;
    kids = pdf_memmem(body, size, "/Kids", 5u);
    if (kids && !pdf_find_typed(body, size, "Page")) return 1;
    return 0;
}

static int pdf_walk_page_tree(QwPdfReader *reader, size_t object,
                              unsigned depth, size_t *stack,
                              size_t stack_size, size_t *visited,
                              size_t *visited_count, size_t **pages,
                              size_t *page_count, size_t *page_capacity)
{
    const char *body = NULL;
    size_t body_size = 0u;
    size_t index;
    if (depth > QW_PDF_READER_MAX_PAGE_DEPTH) return 0;
    for (index = 0u; index < stack_size; ++index) {
        if (stack[index] == object) return 0;
    }
    for (index = 0u; index < *visited_count; ++index) {
        if (visited[index] == object) return 0;
    }
    if (*visited_count >= QW_PDF_READER_MAX_TREE_NODES) return 0;
    visited[(*visited_count)++] = object;
    if (!pdf_object_body(reader, object, &body, &body_size)) return 0;
    if (pdf_node_is_branch(body, body_size)) {
        const char *kids = pdf_memmem(body, body_size, "/Kids", 5u);
        const char *open;
        size_t position;
        if (!kids) return 0;
        open = (const char *)memchr(kids, '[',
                                    body_size - (size_t)(kids - body));
        if (!open) return 0;
        position = (size_t)(open - body) + 1u;
        stack[stack_size] = object;
        for (;;) {
            size_t child = 0u;
            if (!pdf_skip_space(body, body_size, &position)) break;
            if (body[position] == ']') break;
            if (!pdf_parse_reference_at(body, body_size, &position, &child)) {
                break;
            }
            if (!pdf_walk_page_tree(reader, child, depth + 1u, stack,
                                    stack_size + 1u, visited, visited_count,
                                    pages, page_count, page_capacity)) {
                return 0;
            }
        }
        return 1;
    }
    return pdf_append_page(pages, page_count, page_capacity, object);
}

/* Locate the newest trailer /Root reference. */
static int pdf_find_catalog_root(const QwPdfReader *reader, size_t *out_root)
{
    const char *data = (const char *)reader->data;
    size_t size = reader->size;
    size_t offset = 0u;
    const char *last = NULL;
    while (offset + 5u <= size) {
        const char *found = pdf_memmem(data + offset, size - offset, "/Root",
                                       5u);
        if (!found) break;
        if ((size_t)(found - data) + 5u < size) {
            unsigned char next = (unsigned char)found[5];
            if (!isalnum(next) && next != '_' && next != '-') last = found;
        }
        offset = (size_t)(found - data) + 5u;
    }
    if (!last) return 0;
    {
        size_t position = (size_t)(last - data) + 5u;
        return pdf_parse_reference_at(data, size, &position, out_root);
    }
}

static int pdf_reader_open_impl(const uint8_t *data, size_t size,
                                int allow_structure,
                                QwPdfReader **out_reader,
                                size_t *out_page_count,
                                QwConvertReport *report)
{
    QwPdfReader *reader;
    size_t capacity = 0u;
    size_t count = 0u;
    if (!data || !size || !out_reader || !out_page_count) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_reader = NULL;
    *out_page_count = 0u;
    if (size < 8u || memcmp(data, "%PDF-", 5u) != 0) {
        return QW_STATUS_ERR_FORMAT;
    }
    reader = (QwPdfReader *)calloc(1u, sizeof(*reader));
    if (!reader) return QW_STATUS_ERR_OUT_OF_MEMORY;
    reader->data = data;
    reader->size = size;
    /* Resolve object offsets before anything else: encrypted object streams
     * need the security handler, and the handler's /Encrypt dictionary is
     * located through this index. A failed build leaves the text scan in
     * charge, so this can only improve on the fallback. */
    pdf_xref_build(reader);
    /* The security handler must be established before object streams are
     * expanded: their payloads are encrypted per object, and inflating
     * ciphertext would silently drop every page stored in them. */
    if (pdf_memmem((const char *)data, size, "/Encrypt", 8u)) {
        if (pdf_setup_encryption(reader) == -1) {
            if (!allow_structure) {
                pdf_reader_free(reader);
                if (report) {
                    snprintf(report->note, sizeof(report->note),
                             "Encrypted PDF: unsupported security handler or wrong user password");
                }
                return QW_STATUS_ERR_UNSUPPORTED;
            }
            /* Structure-only mode: page dictionaries are not encrypted, so
             * the page tree and boxes stay readable without a password. */
            reader->encrypted = 1;
            reader->encryption_unsupported = 1;
            if (report) {
                snprintf(report->note, sizeof(report->note),
                         "Encrypted PDF: structure only (content requires the user password)");
            }
        }
    }
    if (!reader->encryption_unsupported) {
        const char *cursor = (const char *)data;
        size_t remaining = size;
        while (remaining > 6u) {
            const char *found = pdf_find_typed(cursor, remaining, "ObjStm");
            size_t owner = 0u;
            if (!found) break;
            if (pdf_object_number_before(reader, found, &owner)) {
                if (!pdf_register_object_stream(reader, owner)) {
                    pdf_reader_free(reader);
                    return QW_STATUS_ERR_OUT_OF_MEMORY;
                }
            }
            cursor = found;
            remaining = size - (size_t)(cursor - (const char *)data);
        }
    }
    {
        /* Authoritative order: walk /Root -> /Pages -> /Kids. */
        size_t root_object = 0u;
        if (pdf_find_catalog_root(reader, &root_object)) {
            const char *root_body = NULL;
            size_t root_size = 0u;
            size_t pages_object = 0u;
            if (pdf_object_body(reader, root_object, &root_body, &root_size) &&
                pdf_find_reference(root_body, root_size, "/Pages",
                                   &pages_object)) {
                size_t *stack = (size_t *)calloc(
                    QW_PDF_READER_MAX_PAGE_DEPTH + 1u, sizeof(size_t));
                size_t *visited = (size_t *)calloc(
                    QW_PDF_READER_MAX_TREE_NODES, sizeof(size_t));
                size_t visited_count = 0u;
                if (stack && visited) {
                    (void)pdf_walk_page_tree(reader, pages_object, 0u, stack,
                                             0u, visited, &visited_count,
                                             &reader->page_objects, &count,
                                             &capacity);
                }
                free(stack);
                free(visited);
            }
        }
    }
    /* Fallback for files without a usable catalog: marker scan. */
    if (!count) {
        const char *cursor = (const char *)data;
        size_t remaining = size;
        while (remaining > 6u) {
            const char *found = pdf_find_typed(cursor, remaining, "Page");
            const char *stream = pdf_memmem(cursor, remaining, "stream", 6u);
            size_t object = 0u;
            if (stream &&
                (!found || (size_t)(stream - cursor) <
                               (size_t)(found - cursor))) {
                const char *end = pdf_memmem(
                    stream, remaining - (size_t)(stream - cursor),
                    "endstream", 9u);
                if (!end) break;
                cursor = end + 9;
                remaining = size - (size_t)(cursor - (const char *)data);
                continue;
            }
            if (!found) break;
            if (pdf_object_number_before(reader, found, &object) &&
                count < QW_PDF_READER_MAX_PAGES) {
                if (count >= capacity) {
                    size_t grown = capacity ? capacity * 2u : 16u;
                    size_t *resized =
                        (size_t *)realloc(reader->page_objects, grown * sizeof(*resized));
                    if (!resized) {
                        free(reader->page_objects);
                        free(reader);
                        return QW_STATUS_ERR_OUT_OF_MEMORY;
                    }
                    reader->page_objects = resized;
                    capacity = grown;
                }
                reader->page_objects[count++] = object;
            }
            cursor = found;
            remaining = size - (size_t)(cursor - (const char *)data);
        }
    }
    /* Pages stored inside object streams (fallback path only). */
    if (!count && count < QW_PDF_READER_MAX_PAGES) {
        size_t slice_index;
        for (slice_index = 0u; slice_index < reader->slice_count; ++slice_index) {
            const char *found;
            {
                const char *body = NULL;
                size_t dict_size = 0u;
                if (pdf_object_body(reader, reader->slices[slice_index].object,
                                    &body, &dict_size) &&
                    pdf_memmem(body, dict_size, "/ObjStm", 7u))
                    continue;
            }
            found = pdf_find_typed(reader->slices[slice_index].data,
                                   reader->slices[slice_index].size, "Page");
            if (!found) continue;
            if (count >= capacity) {
                size_t grown = capacity ? capacity * 2u : 16u;
                size_t *resized = (size_t *)realloc(reader->page_objects,
                                                    grown * sizeof(*resized));
                if (!resized) {
                    pdf_reader_free(reader);
                    return QW_STATUS_ERR_OUT_OF_MEMORY;
                }
                reader->page_objects = resized;
                capacity = grown;
            }
            reader->page_objects[count++] = reader->slices[slice_index].object;
        }
    }
    if (!count) {
        pdf_reader_free(reader);
        if (report) snprintf(report->note, sizeof(report->note),
                             "No /Type /Page objects were found in the file");
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    reader->page_count = count;
    *out_reader = reader;
    *out_page_count = count;
    if (report) snprintf(report->note, sizeof(report->note),
                         "Opened %zu PDF page(s) with the internal Quirewise reader", count);
    return QW_STATUS_OK;
}

int qw_pdf_reader_open(const uint8_t *data, size_t size,
                       QwPdfReader **out_reader,
                       size_t *out_page_count,
                       QwConvertReport *report)
{
    return pdf_reader_open_impl(data, size, 0, out_reader, out_page_count,
                                report);
}

int qw_pdf_reader_open_structure(const uint8_t *data, size_t size,
                                 QwPdfReader **out_reader,
                                 size_t *out_page_count,
                                 QwConvertReport *report)
{
    return pdf_reader_open_impl(data, size, 1, out_reader, out_page_count,
                                report);
}

int qw_pdf_reader_extract_text(QwPdfReader *reader, uint8_t **out_text,
                                     size_t *out_size, QwConvertReport *report)
{
    QwPdfBuffer text;
    size_t index;
    if (!reader || !out_text || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_text = NULL;
    *out_size = 0u;
    if (reader->encryption_unsupported) {
        if (report) {
            snprintf(report->note, sizeof(report->note),
                     "Encrypted PDF: content requires the user password (structure-only open)");
        }
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (!reader->text_ready) {
        memset(&text, 0, sizeof(text));
        for (index = 0u; index < reader->page_count; ++index) {
            if (index) pdf_buffer_append(&text, "\n", 1u);
            if (!pdf_extract_page_text(reader, reader->page_objects[index], &text)) {
                free(text.data);
                return QW_STATUS_ERR_DECODE;
            }
        }
        reader->text = text.data;
        reader->text_size = text.size;
        reader->text_ready = 1;
    }
    /* A page may legitimately have no content stream (empty page); extraction
     * then succeeds with zero bytes instead of a decode error. */
    if (!reader->text || reader->text_size == 0u) {
        *out_text = (uint8_t *)calloc(1u, 1u);
        if (!*out_text) return QW_STATUS_ERR_OUT_OF_MEMORY;
        *out_size = 0u;
        if (report) snprintf(report->note, sizeof(report->note),
                             "Document contains %zu page(s) without extractable text",
                             reader->page_count);
        return QW_STATUS_OK;
    }
    *out_text = (uint8_t *)malloc(reader->text_size + 1u);
    if (!*out_text) return QW_STATUS_ERR_OUT_OF_MEMORY;
    memcpy(*out_text, reader->text, reader->text_size);
    (*out_text)[reader->text_size] = '\0';
    *out_size = reader->text_size;
    if (report) snprintf(report->note, sizeof(report->note),
                         "Extracted %zu text byte(s) from %zu page(s)", reader->text_size,
                         reader->page_count);
    return QW_STATUS_OK;
}

int qw_pdf_reader_extract_text_to_stream(QwPdfReader *reader, FILE *stream,
                                               size_t max_output_bytes,
                                               size_t *out_size,
                                               QwConvertReport *report)
{
    uint8_t *text = NULL;
    size_t text_size = 0u;
    int rc;
    if (!stream) return QW_STATUS_ERR_INVALID_ARGUMENT;
    rc = qw_pdf_reader_extract_text(reader, &text, &text_size, report);
    if (rc != QW_STATUS_OK) return rc;
    if (max_output_bytes && text_size > max_output_bytes) {
        free(text);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (text_size && fwrite(text, 1u, text_size, stream) != text_size) {
        free(text);
        return QW_STATUS_ERR_IO;
    }
    if (out_size) *out_size = text_size;
    free(text);
    return QW_STATUS_OK;
}


typedef struct PdfMatrix {
    double a, b, c, d, e, f;
} PdfMatrix;

static void pdf_matrix_reset(PdfMatrix *m)
{
    m->a = 1.0; m->b = 0.0; m->c = 0.0; m->d = 1.0; m->e = 0.0; m->f = 0.0;
}

static void pdf_matrix_concat(PdfMatrix *m, const double n[6])
{
    PdfMatrix result;
    result.a = m->a * n[0] + m->b * n[2];
    result.b = m->a * n[1] + m->b * n[3];
    result.c = m->c * n[0] + m->d * n[2];
    result.d = m->c * n[1] + m->d * n[3];
    result.e = m->e * n[0] + m->f * n[2] + n[4];
    result.f = m->e * n[1] + m->f * n[3] + n[5];
    *m = result;
}

static void pdf_paint_rect(uint8_t *pixels, size_t width, size_t height,
                           const PdfMatrix *ctm, double x, double y,
                           double rect_width, double rect_height,
                           const uint8_t rgb[3])
{
    double x0 = x, y0 = y, x1 = x + rect_width, y1 = y + rect_height;
    double px0, py0, px1, py1;
    size_t left, right, top, bottom, row, column;
    if (ctm->b != 0.0 || ctm->c != 0.0) return;
    px0 = ctm->a * x0 + ctm->e;
    px1 = ctm->a * x1 + ctm->e;
    py0 = ctm->d * y0 + ctm->f;
    py1 = ctm->d * y1 + ctm->f;
    if (px0 != px0 || px1 != px1 || py0 != py0 || py1 != py1) return;
    if (px0 > px1) { double swap = px0; px0 = px1; px1 = swap; }
    if (py0 > py1) { double swap = py0; py0 = py1; py1 = swap; }
    /* Clamp before the cast: converting a negative double to size_t wraps to a
     * huge value and would turn the fill loop into an out-of-bounds walk. */
    if (px1 <= 0.0 || py1 <= 0.0 || px0 >= (double)width ||
        py0 >= (double)height) {
        return;
    }
    left = px0 <= 0.0 ? 0u : (size_t)px0;
    right = px1 >= (double)width ? width : (size_t)(px1 + 0.999);
    bottom = py0 <= 0.0 ? 0u : (size_t)py0;
    top = py1 >= (double)height ? height : (size_t)(py1 + 0.999);
    if (left >= right || bottom >= top) return;
    for (row = bottom; row < top; ++row) {
        size_t inverted = height - 1u - row;
        if (inverted >= height) continue;
        for (column = left; column < right; ++column) {
            size_t offset = (inverted * width + column) * 3u;
            pixels[offset + 0u] = rgb[0];
            pixels[offset + 1u] = rgb[1];
            pixels[offset + 2u] = rgb[2];
        }
    }
}


typedef struct PdfImageRef {
    char name[64];
    size_t object;
} PdfImageRef;

static size_t pdf_collect_image_xobjects(const QwPdfReader *reader,
                                         size_t page_object,
                                         PdfImageRef *refs, size_t capacity)
{
    const char *body = NULL;
    size_t dict_size = 0u;
    char *copy;
    char *cursor;
    char *end;
    size_t count = 0u;
    if (!pdf_object_body(reader, page_object, &body, &dict_size)) return 0u;
    copy = (char *)malloc(dict_size + 1u);
    if (!copy) return 0u;
    memcpy(copy, body, dict_size);
    copy[dict_size] = '\0';
    cursor = strstr(copy, "/XObject");
    if (cursor) {
        cursor = strchr(cursor, '<');
        end = cursor ? strstr(cursor, ">>") : NULL;
        while (cursor && end && cursor < end && count < capacity) {
            char *name = strchr(cursor, '/');
            if (!name || name >= end) break;
            cursor = name + 1;
            while (cursor < end && *cursor && *cursor != ' ' &&
                   *cursor != '\t' && *cursor != '\r' && *cursor != '\n' &&
                   *cursor != '/')
                ++cursor;
            {
                size_t name_length = (size_t)(cursor - name - 1);
                if (name_length > 0u && name_length < sizeof(refs[0].name)) {
                    memcpy(refs[count].name, name + 1, name_length);
                    refs[count].name[name_length] = '\0';
                    refs[count].object = (size_t)strtoul(cursor, &cursor, 10);
                    if (refs[count].object) ++count;
                }
            }
            cursor = strstr(cursor, "/");
        }
    }
    free(copy);
    return count;
}

static void pdf_blit_image(const QwImage *image, uint8_t *pixels, size_t width,
                           size_t height, const PdfMatrix *ctm)
{
    double x0 = ctm->e;
    double y0 = ctm->f;
    double x1 = ctm->e + ctm->a;
    double y1 = ctm->f + ctm->d;
    size_t left, right, top, bottom, row, column;
    if (ctm->b != 0.0 || ctm->c != 0.0 || !image->pixels || !image->width ||
        !image->height)
        return;
    if (x0 > x1) { double swap = x0; x0 = x1; x1 = swap; }
    if (y0 > y1) { double swap = y0; y0 = y1; y1 = swap; }
    left = x0 < 0.0 ? 0u : (size_t)x0;
    right = x1 > (double)width ? width : (size_t)(x1 + 0.999);
    bottom = y0 < 0.0 ? 0u : (size_t)y0;
    top = y1 > (double)height ? height : (size_t)(y1 + 0.999);
    if (left >= right || bottom >= top) return;
    for (column = left; column < right; ++column) {
        size_t source_x = (size_t)((column - left) * (size_t)image->width /
                                   (right - left));
        for (row = bottom; row < top; ++row) {
            size_t inverted = height - 1u - row;
            /* row counts PDF page units from the bottom while the image's
             * first row is its top, so the sampling runs backwards. */
            size_t source_y = (size_t)(((top - 1u) - row) * (size_t)image->height /
                                       (top - bottom));
            size_t destination = (inverted * width + column) * 3u;
            const uint8_t *source;
            if (inverted >= height) continue;
            if (source_y >= image->height) source_y = image->height - 1u;
            if (source_x >= image->width) source_x = image->width - 1u;
            if (image->format == QW_IMAGE_PIXEL_RGB24) {
                source = image->pixels + source_y * image->stride + source_x * 3u;
                pixels[destination + 0u] = source[0];
                pixels[destination + 1u] = source[1];
                pixels[destination + 2u] = source[2];
            } else if (image->format == QW_IMAGE_PIXEL_GRAY8) {
                source = image->pixels + source_y * image->stride + source_x;
                pixels[destination + 0u] = source[0];
                pixels[destination + 1u] = source[0];
                pixels[destination + 2u] = source[0];
            }
        }
    }
}

/* TIFF predictor 2 (horizontal differencing), used by Flate images. */
static void pdf_tiff_unpredict(uint8_t *data, size_t rows, size_t columns,
                               size_t bpp)
{
    size_t row;
    size_t column;
    if (!bpp) bpp = 1u;
    for (row = 0u; row < rows; ++row) {
        uint8_t *row_data = data + row * columns;
        for (column = bpp; column < columns; ++column) {
            row_data[column] =
                (uint8_t)(row_data[column] + row_data[column - bpp]);
        }
    }
}

/* Draw a raw (FlateDecode or uncompressed) DeviceRGB/DeviceGray image, with
 * optional PNG or TIFF predictors. Self-describing formats (JPEG/PNG files)
 * stay with the image package decoder. */
static int pdf_draw_raw_image(const char *dict, size_t dict_size,
                              uint8_t *stream, size_t stream_size,
                              uint8_t *pixels, size_t width, size_t height,
                              const PdfMatrix *ctm)
{
    const char *subtype = pdf_memmem(dict, dict_size, "/Subtype", 8u);
    long image_width;
    long image_height;
    long bits = 8;
    long predictor = 0;
    size_t components;
    size_t row_bytes;
    size_t expected;
    uint8_t *decoded = NULL;
    uint8_t *samples = stream;
    QwImage image;

    if (!subtype ||
        !pdf_name_is(subtype + 8u,
                     dict_size - (size_t)(subtype - dict) - 8u, "Image")) {
        return 0;
    }
    if (pdf_memmem(dict, dict_size, "/DCTDecode", 10u) ||
        pdf_memmem(dict, dict_size, "/JPXDecode", 10u) ||
        pdf_memmem(dict, dict_size, "/CCITTFaxDecode", 15u) ||
        pdf_memmem(dict, dict_size, "/JBIG2Decode", 12u) ||
        pdf_memmem(dict, dict_size, "/ImageMask", 10u)) {
        return 0;
    }
    if (pdf_memmem(dict, dict_size, "/DeviceRGB", 10u)) {
        components = 3u;
    } else if (pdf_memmem(dict, dict_size, "/DeviceGray", 11u)) {
        components = 1u;
    } else {
        return 0; /* ICCBased/Indexed/Separation are out of scope here */
    }
    image_width = pdf_parse_int_field(dict, dict_size, "/Width", -1);
    image_height = pdf_parse_int_field(dict, dict_size, "/Height", -1);
    bits = pdf_parse_int_field(dict, dict_size, "/BitsPerComponent", 8);
    if (image_width <= 0 || image_height <= 0 || bits != 8) return 0;
    row_bytes = (size_t)image_width * components;
    expected = row_bytes * (size_t)image_height;
    if (expected > QW_PDF_READER_MAX_STREAM) return 0;
    if (pdf_memmem(dict, dict_size, "/Predictor", 10u)) {
        predictor = pdf_parse_int_field(dict, dict_size, "/Predictor", 0);
    }
    if (predictor >= 10) {
        if (stream_size != (size_t)image_height * (row_bytes + 1u)) return 0;
        decoded = (uint8_t *)malloc(expected);
        if (!decoded) return 0;
        if (!pdf_png_unpredict(stream, (size_t)image_height, row_bytes,
                               components, decoded)) {
            free(decoded);
            return 0;
        }
        samples = decoded;
    } else if (predictor == 2) {
        if (stream_size != expected) return 0;
        decoded = (uint8_t *)malloc(expected);
        if (!decoded) return 0;
        memcpy(decoded, stream, expected);
        pdf_tiff_unpredict(decoded, (size_t)image_height, row_bytes,
                           components);
        samples = decoded;
    } else if (stream_size != expected) {
        return 0;
    }
    memset(&image, 0, sizeof(image));
    image.pixels = samples;
    image.size = expected;
    image.stride = row_bytes;
    image.width = (uint32_t)image_width;
    image.height = (uint32_t)image_height;
    image.format =
        components == 3u ? QW_IMAGE_PIXEL_RGB24 : QW_IMAGE_PIXEL_GRAY8;
    pdf_blit_image(&image, pixels, width, height, ctm);
    free(decoded);
    return 1;
}

/* A bilevel image may carry "/Decode [1 0]" to invert the sample polarity.
 * The raw-sample path applies /Decode itself; the CCITT and JBIG2 decoders
 * return device values, so they invert after the fact. */
static int pdf_decode_inverts(const char *dict, size_t dict_size)
{
    const char *decode = pdf_memmem(dict, dict_size, "/Decode", 7u);
    size_t index;
    if (!decode) return 0;
    for (index = 7u; index < 24u; ++index) {
        const char *cursor = decode + index;
        if (cursor >= dict + dict_size) break;
        if (*cursor == '[') {
            ++cursor;
            while (cursor < dict + dict_size && (*cursor == ' ' || *cursor == '\t')) ++cursor;
            if (cursor < dict + dict_size && *cursor == '1') {
                ++cursor;
                while (cursor < dict + dict_size && (*cursor == ' ' || *cursor == '\t')) ++cursor;
                if (cursor < dict + dict_size && *cursor == '0') return 1;
            }
            return 0;
        }
        if (*cursor == ']' || *cursor == '>') return 0;
    }
    return 0;
}

static void pdf_invert_gray(uint8_t *gray, size_t count)
{
    size_t index;
    for (index = 0u; index < count; ++index) {
        gray[index] = (uint8_t)(255u - gray[index]);
    }
}
/* JBIG2 image streams live in a segment stream; /JBIG2Globals, when present,
 * carries the shared segments in a separate object. */
static int pdf_draw_jbig2_image(const char *dict, size_t dict_size,
                                const uint8_t *stream, size_t stream_size,
                                uint8_t *pixels, size_t width, size_t height,
                                const PdfMatrix *ctm, const QwPdfReader *reader)
{
    long image_width;
    long image_height;
    uint8_t *gray = NULL;
    uint8_t *globals = NULL;
    size_t globals_size = 0u;
    QwImage image;
    int status;
    if (!pdf_memmem(dict, dict_size, "/JBIG2Decode", 12u)) return 0;
    image_width = pdf_parse_int_field(dict, dict_size, "/Width", -1);
    image_height = pdf_parse_int_field(dict, dict_size, "/Height", -1);
    if (image_width <= 0 || image_height <= 0 ||
        image_width > 20000 || image_height > 20000) {
        return 0;
    }
    {
        const char *ref = pdf_memmem(dict, dict_size, "/JBIG2Globals", 13u);
        if (ref) {
            const char *cursor = ref + 13u;
            const char *limit = dict + dict_size;
            long object = -1;
            while (cursor < limit && (*cursor == ' ' || *cursor == '\t' ||
                                      *cursor == '\r' || *cursor == '\n')) {
                ++cursor;
            }
            if (cursor < limit && *cursor >= '0' && *cursor <= '9') {
                object = 0;
                while (cursor < limit && *cursor >= '0' && *cursor <= '9') {
                    object = object * 10 + (*cursor - '0');
                    if (object > 1000000) { object = -1; break; }
                    ++cursor;
                }
            }
            if (object > 0 && reader) {
                globals = pdf_object_stream(reader, (size_t)object, &globals_size);
            }
        }
    }
    status = pdf_jbig2_decode_image(globals, globals_size, stream, stream_size,
                                    (uint32_t)image_width, (uint32_t)image_height,
                                    &gray);
    free(globals);
    if (status != 1 || !gray) return 0;
    if (pdf_decode_inverts(dict, dict_size)) {
        pdf_invert_gray(gray, (size_t)image_width * (size_t)image_height);
    }
    memset(&image, 0, sizeof(image));
    image.pixels = gray;
    image.size = (size_t)image_width * (size_t)image_height;
    image.stride = (size_t)image_width;
    image.width = (uint32_t)image_width;
    image.height = (uint32_t)image_height;
    image.format = QW_IMAGE_PIXEL_GRAY8;
    pdf_blit_image(&image, pixels, width, height, ctm);
    free(gray);
    return 1;
}

/* Bitonal pages from the PDF writer use /CCITTFaxDecode; the raw-sample path
 * rejects that filter, so decode it here and blit the samples. */
static int pdf_draw_ccitt_image(const char *dict, size_t dict_size,
                                const uint8_t *stream, size_t stream_size,
                                uint8_t *pixels, size_t width, size_t height,
                                const PdfMatrix *ctm)
{
    long columns;
    long rows;
    long k;
    int black_is1 = 0;
    uint8_t *gray = NULL;
    QwImage image;
    if (!pdf_memmem(dict, dict_size, "/CCITTFaxDecode", 15u)) return 0;
    k = pdf_parse_int_field(dict, dict_size, "/K", 0);
    if (k >= 0) return 0; /* Group 3 (modified Huffman/READ) is not handled */
    columns = pdf_parse_int_field(dict, dict_size, "/Columns", -1);
    rows = pdf_parse_int_field(dict, dict_size, "/Rows", -1);
    if (columns <= 0 || rows <= 0) {
        columns = pdf_parse_int_field(dict, dict_size, "/Width", -1);
        rows = pdf_parse_int_field(dict, dict_size, "/Height", -1);
    }
    if (columns <= 0 || rows <= 0 || columns > 20000 || rows > 20000) return 0;
    {
        const char *flag = pdf_memmem(dict, dict_size, "/BlackIs1", 9u);
        if (flag) {
            size_t after = (size_t)(flag - dict) + 9u;
            while (after < dict_size && isspace((unsigned char)dict[after])) ++after;
            if (after + 4u <= dict_size && memcmp(dict + after, "true", 4u) == 0) {
                black_is1 = 1;
            }
        }
    }
    if (!pdf_ccitt_decode_image(stream, stream_size, (uint32_t)columns,
                                (uint32_t)rows, black_is1, &gray)) {
        return 0;
    }
    if (pdf_decode_inverts(dict, dict_size)) {
        pdf_invert_gray(gray, (size_t)columns * (size_t)rows);
    }
    memset(&image, 0, sizeof(image));
    image.pixels = gray;
    image.size = (size_t)columns * (size_t)rows;
    image.stride = (size_t)columns;
    image.width = (uint32_t)columns;
    image.height = (uint32_t)rows;
    image.format = QW_IMAGE_PIXEL_GRAY8;
    pdf_blit_image(&image, pixels, width, height, ctm);
    free(gray);
    return 1;
}

static int pdf_draw_image_xobject(const QwPdfReader *reader, size_t object,
                                  uint8_t *pixels, size_t width, size_t height,
                                  const PdfMatrix *ctm)
{
    const char *body;
    size_t dict_size;
    size_t stream_size = 0u;
    uint8_t *stream;
    QwImage image;
    int ok = 0;
    if (!pdf_object_body(reader, object, &body, &dict_size)) return 0;
    stream = pdf_object_stream(reader, object, &stream_size);
    if (!stream) return 0;
    if (pdf_draw_raw_image(body, dict_size, stream, stream_size, pixels,
                           width, height, ctm)) {
        free(stream);
        return 1;
    }
    if (pdf_draw_ccitt_image(body, dict_size, stream, stream_size, pixels,
                             width, height, ctm)) {
        free(stream);
        return 1;
    }
    if (pdf_draw_jbig2_image(body, dict_size, stream, stream_size, pixels,
                             width, height, ctm, reader)) {
        free(stream);
        return 1;
    }
    memset(&image, 0, sizeof(image));
    if (qw_image_decode(stream, stream_size, &image) == QW_IMAGE_OK) {
        pdf_blit_image(&image, pixels, width, height, ctm);
        qw_image_free(&image);
        ok = 1;
    }
    free(stream);
    return ok;
}


typedef struct PdfPath {
    double x[64];
    double y[64];
    size_t count;
} PdfPath;

static void pdf_path_reset(PdfPath *path)
{
    path->count = 0u;
}

static void pdf_path_add(PdfPath *path, const PdfMatrix *ctm, double x, double y)
{
    if (path->count >= 64u) return;
    path->x[path->count] = ctm->a * x + ctm->c * y + ctm->e;
    path->y[path->count] = ctm->b * x + ctm->d * y + ctm->f;
    ++path->count;
}

static int pdf_point_in_polygon(const double *xs, const double *ys, size_t count,
                                double px, double py)
{
    size_t i;
    int inside = 0;
    for (i = 0u; i < count; ++i) {
        double xi = xs[i], yi = ys[i];
        double xj = xs[(i + 1u) % count], yj = ys[(i + 1u) % count];
        if ((yi > py) != (yj > py)) {
            double x = (xj - xi) * (py - yi) / (yj - yi) + xi;
            if (px < x) inside = !inside;
        }
    }
    return inside;
}

static void pdf_fill_path(uint8_t *pixels, size_t width, size_t height,
                          const PdfPath *path, const uint8_t rgb[3])
{
    size_t row, column;
    if (path->count < 3u) return;
    for (row = 0u; row < height; ++row) {
        size_t inverted = height - 1u - row;
        for (column = 0u; column < width; ++column) {
            if (pdf_point_in_polygon(path->x, path->y, path->count,
                                     (double)column + 0.5,
                                     (double)(inverted) + 0.5)) {
                size_t offset = (row * width + column) * 3u;
                pixels[offset + 0u] = rgb[0];
                pixels[offset + 1u] = rgb[1];
                pixels[offset + 2u] = rgb[2];
            }
        }
    }
}

static void pdf_stroke_path(uint8_t *pixels, size_t width, size_t height,
                            const PdfPath *path, const uint8_t rgb[3])
{
    size_t segment;
    if (path->count < 2u) return;
    for (segment = 0u; segment + 1u < path->count; ++segment) {
        double x0 = path->x[segment], y0 = path->y[segment];
        double x1 = path->x[segment + 1u], y1 = path->y[segment + 1u];
        double dx = x1 - x0, dy = y1 - y0;
        int steps = (int)((dx < 0.0 ? -dx : dx) > (dy < 0.0 ? -dy : dy)
                              ? (dx < 0.0 ? -dx : dx)
                              : (dy < 0.0 ? -dy : dy));
        int step;
        if (steps <= 0) steps = 1;
        for (step = 0; step <= steps; ++step) {
            double t = (double)step / (double)steps;
            double px = x0 + dx * t;
            double py = y0 + dy * t;
            if (px < 0.0 || py < 0.0 || px >= (double)width ||
                py >= (double)height)
                continue;
            {
                size_t column = (size_t)px;
                size_t row = height - 1u - (size_t)py;
                size_t offset;
                if (column >= width || row >= height) continue;
                offset = (row * width + column) * 3u;
                pixels[offset + 0u] = rgb[0];
                pixels[offset + 1u] = rgb[1];
                pixels[offset + 2u] = rgb[2];
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Built-in approximation font                                         */
/*                                                                     */
/* Text is drawn with a 3x5 bitmap font so a page is not blank. It is   */
/* deliberately not a font engine: no glyph outlines, no embedding, no  */
/* CID fonts. Lowercase maps to uppercase; anything outside printable   */
/* ASCII is skipped and reported through the route-loss note.           */
/* ------------------------------------------------------------------ */

static const uint8_t qw_pdf_font3x5[95][5] = {
    [' ' - 32] = {0, 0, 0, 0, 0},
    ['!' - 32] = {2, 2, 2, 0, 2},
    ['"' - 32] = {5, 5, 0, 0, 0},
    ['#' - 32] = {5, 7, 5, 7, 5},
    ['$' - 32] = {3, 6, 2, 3, 6},
    ['%' - 32] = {5, 1, 2, 4, 5},
    ['&' - 32] = {2, 5, 2, 5, 3},
    ['\'' - 32] = {2, 2, 0, 0, 0},
    ['(' - 32] = {1, 2, 2, 2, 1},
    [')' - 32] = {4, 2, 2, 2, 4},
    ['*' - 32] = {5, 2, 5, 0, 0},
    ['+' - 32] = {0, 2, 7, 2, 0},
    [',' - 32] = {0, 0, 0, 2, 4},
    ['-' - 32] = {0, 0, 7, 0, 0},
    ['.' - 32] = {0, 0, 0, 0, 2},
    ['/' - 32] = {1, 1, 2, 4, 4},
    ['0' - 32] = {7, 5, 5, 5, 7},
    ['1' - 32] = {2, 6, 2, 2, 7},
    ['2' - 32] = {6, 1, 2, 4, 7},
    ['3' - 32] = {6, 1, 2, 1, 6},
    ['4' - 32] = {5, 5, 7, 1, 1},
    ['5' - 32] = {7, 4, 6, 1, 6},
    ['6' - 32] = {3, 4, 7, 5, 2},
    ['7' - 32] = {7, 1, 2, 2, 2},
    ['8' - 32] = {2, 5, 2, 5, 2},
    ['9' - 32] = {2, 5, 3, 1, 6},
    [':' - 32] = {0, 2, 0, 2, 0},
    [';' - 32] = {0, 2, 0, 2, 4},
    ['<' - 32] = {1, 2, 4, 2, 1},
    ['=' - 32] = {0, 7, 0, 7, 0},
    ['>' - 32] = {4, 2, 1, 2, 4},
    ['?' - 32] = {6, 1, 2, 0, 2},
    ['@' - 32] = {2, 5, 7, 5, 4},
    ['A' - 32] = {2, 5, 7, 5, 5},
    ['B' - 32] = {6, 5, 6, 5, 6},
    ['C' - 32] = {3, 4, 4, 4, 3},
    ['D' - 32] = {6, 5, 5, 5, 6},
    ['E' - 32] = {7, 4, 6, 4, 7},
    ['F' - 32] = {7, 4, 6, 4, 4},
    ['G' - 32] = {3, 4, 5, 5, 3},
    ['H' - 32] = {5, 5, 7, 5, 5},
    ['I' - 32] = {7, 2, 2, 2, 7},
    ['J' - 32] = {1, 1, 1, 5, 2},
    ['K' - 32] = {5, 5, 6, 5, 5},
    ['L' - 32] = {4, 4, 4, 4, 7},
    ['M' - 32] = {5, 7, 7, 5, 5},
    ['N' - 32] = {6, 5, 5, 5, 5},
    ['O' - 32] = {2, 5, 5, 5, 2},
    ['P' - 32] = {6, 5, 6, 4, 4},
    ['Q' - 32] = {2, 5, 5, 6, 3},
    ['R' - 32] = {6, 5, 6, 5, 5},
    ['S' - 32] = {3, 4, 2, 1, 6},
    ['T' - 32] = {7, 2, 2, 2, 2},
    ['U' - 32] = {5, 5, 5, 5, 3},
    ['V' - 32] = {5, 5, 5, 5, 2},
    ['W' - 32] = {5, 5, 7, 7, 5},
    ['X' - 32] = {5, 5, 2, 5, 5},
    ['Y' - 32] = {5, 5, 2, 2, 2},
    ['Z' - 32] = {7, 1, 2, 4, 7},
    ['[' - 32] = {3, 2, 2, 2, 3},
    ['\\' - 32] = {4, 4, 2, 1, 1},
    [']' - 32] = {6, 2, 2, 2, 6},
    ['_' - 32] = {0, 0, 0, 0, 7}
};

static const uint8_t *pdf_glyph_for(unsigned char value)
{
    if (value >= 'a' && value <= 'z') value = (unsigned char)(value - 'a' + 'A');
    if (value < 32u || value > 126u) return NULL;
    return qw_pdf_font3x5[value - 32u];
}

/* Decode a literal string starting at '('; returns the position after ')'. */
static size_t pdf_scan_literal_string(const char *stream, size_t size,
                                      size_t position, uint8_t *out,
                                      size_t capacity, size_t *out_length)
{
    size_t index = position + 1u;
    size_t length = 0u;
    int level = 1;
    while (index < size && level > 0) {
        unsigned char ch = (unsigned char)stream[index++];
        if (ch == '\\' && index < size) {
            unsigned char escaped = (unsigned char)stream[index++];
            unsigned char decoded;
            switch (escaped) {
            case 'n': decoded = (unsigned char)'\n'; break;
            case 'r': decoded = (unsigned char)'\r'; break;
            case 't': decoded = (unsigned char)'\t'; break;
            case 'b': decoded = (unsigned char)'\b'; break;
            case 'f': decoded = (unsigned char)'\f'; break;
            case '(': decoded = (unsigned char)'('; break;
            case ')': decoded = (unsigned char)')'; break;
            case '\\': decoded = (unsigned char)'\\'; break;
            case '\r':
                if (index < size && stream[index] == '\n') ++index;
                continue;
            case '\n':
                continue;
            default:
                if (escaped >= '0' && escaped <= '7') {
                    unsigned value = (unsigned)(escaped - '0');
                    int digits = 1;
                    while (digits < 3 && index < size &&
                           stream[index] >= '0' && stream[index] <= '7') {
                        value = value * 8u + (unsigned)(stream[index] - '0');
                        ++index;
                        ++digits;
                    }
                    decoded = (unsigned char)value;
                } else {
                    decoded = escaped;
                }
                break;
            }
            if (length < capacity) out[length++] = decoded;
            continue;
        }
        if (ch == '(') {
            ++level;
        } else if (ch == ')') {
            --level;
        }
        if (level > 0 && length < capacity) out[length++] = ch;
    }
    *out_length = length;
    return index;
}

/* Decode a hex string starting at '<'; returns the position after '>'. */
static size_t pdf_scan_hex_string(const char *stream, size_t size,
                                  size_t position, uint8_t *out,
                                  size_t capacity, size_t *out_length)
{
    size_t index = position + 1u;
    size_t length = 0u;
    int high = -1;
    while (index < size && stream[index] != '>') {
        char ch = stream[index++];
        int value;
        if (ch >= '0' && ch <= '9') value = ch - '0';
        else if (ch >= 'a' && ch <= 'f') value = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') value = ch - 'A' + 10;
        else continue;
        if (high < 0) {
            high = value;
        } else {
            if (length < capacity) out[length++] = (uint8_t)((high << 4) | value);
            high = -1;
        }
    }
    if (high >= 0 && length < capacity) out[length++] = (uint8_t)(high << 4);
    if (index < size && stream[index] == '>') ++index;
    *out_length = length;
    return index;
}

/* Compose text-space -> user-space -> device-space for the current run. */
static void pdf_text_matrix_for(const PdfMatrix *ctm, const PdfMatrix *text,
                                double size, double hscale, PdfMatrix *out)
{
    double scale[6] = {size * hscale, 0.0, 0.0, size, 0.0, 0.0};
    double tm[6] = {text->a, text->b, text->c, text->d, text->e, text->f};
    double cm[6] = {ctm->a, ctm->b, ctm->c, ctm->d, ctm->e, ctm->f};
    pdf_matrix_reset(out);
    pdf_matrix_concat(out, scale);
    pdf_matrix_concat(out, tm);
    pdf_matrix_concat(out, cm);
}

/* Draw one decoded string and advance the text matrix. */
static void pdf_paint_string(const uint8_t *bytes, size_t count,
                             uint8_t *pixels, size_t width, size_t height,
                             const PdfMatrix *ctm, PdfMatrix *text,
                             double size, double hscale, double char_spacing,
                             double word_spacing, const uint8_t rgb[3],
                             size_t *drawn_glyphs)
{
    PdfMatrix trm;
    double x = 0.0;
    size_t index;
    pdf_text_matrix_for(ctm, text, size, hscale, &trm);
    for (index = 0u; index < count; ++index) {
        const uint8_t *glyph = pdf_glyph_for(bytes[index]);
        double advance = size * 0.6 * hscale + char_spacing;
        if (bytes[index] == ' ') advance += word_spacing;
        if (advance <= 0.0) advance = size * 0.6 * hscale;
        if (glyph) {
            int row;
            int column;
            for (row = 0; row < 5; ++row) {
                for (column = 0; column < 3; ++column) {
                    if (glyph[row] & (uint8_t)(1u << (2 - column))) {
                        double gx = x + (double)column * (0.6 / 3.0);
                        double gy = (double)(4 - row) * (0.7 / 5.0);
                        pdf_paint_rect(pixels, width, height, &trm, gx, gy,
                                       0.6 / 3.0, 0.7 / 5.0, rgb);
                    }
                }
            }
            ++*drawn_glyphs;
        }
        x += advance;
    }
    text->e += x * text->a;
    text->f += x * text->b;
}

/* TJ takes an array of strings interleaved with kerning adjustments. */
static void pdf_paint_tj_array(const char *stream, size_t start, size_t end,
                               uint8_t *pixels, size_t width, size_t height,
                               const PdfMatrix *ctm, PdfMatrix *text,
                               double size, double hscale, double char_spacing,
                               double word_spacing, const uint8_t rgb[3],
                               size_t *drawn_glyphs)
{
    size_t index = start;
    uint8_t buffer[256];
    while (index < end) {
        char ch = stream[index];
        if (ch == '(') {
            size_t length = 0u;
            index = pdf_scan_literal_string(stream, end, index, buffer,
                                            sizeof(buffer), &length);
            pdf_paint_string(buffer, length, pixels, width, height, ctm, text,
                             size, hscale, char_spacing, word_spacing, rgb,
                             drawn_glyphs);
        } else if (ch == '<' && index + 1u < end && stream[index + 1u] != '<') {
            size_t length = 0u;
            index = pdf_scan_hex_string(stream, end, index, buffer,
                                        sizeof(buffer), &length);
            pdf_paint_string(buffer, length, pixels, width, height, ctm, text,
                             size, hscale, char_spacing, word_spacing, rgb,
                             drawn_glyphs);
        } else if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' ||
                   ch == '.') {
            char *number_end = NULL;
            double value = strtod(stream + index, &number_end);
            if (number_end && number_end != stream + index) {
                double shift = -value / 1000.0 * size * hscale;
                text->e += shift * text->a;
                text->f += shift * text->b;
                index = (size_t)(number_end - stream);
                continue;
            }
            ++index;
        } else {
            ++index;
        }
    }
}

static int pdf_paint_content(const QwPdfReader *reader,
                             const PdfImageRef *images, size_t image_count,
                             const uint8_t *stream, size_t size,
                             uint8_t *pixels, size_t width, size_t height,
                             size_t *painted, int *skipped_text_or_image)
{
    size_t position = 0u;
    PdfMatrix ctm;
    PdfMatrix stack[32];
    size_t depth = 0u;
    uint8_t fill[3] = {0u, 0u, 0u};
    double numbers[8];
    size_t number_count = 0u;
    char last_name[64];
    PdfPath path;
    PdfMatrix text_matrix;
    PdfMatrix line_matrix;
    double font_size = 12.0;
    double leading = 0.0;
    double char_spacing = 0.0;
    double word_spacing = 0.0;
    double hscale = 1.0;
    uint8_t last_string[256];
    size_t last_string_size = 0u;
    size_t array_start = 0u;
    size_t array_end = 0u;
    int text_skipped = 0;
    last_name[0] = '\0';
    pdf_path_reset(&path);
    pdf_matrix_reset(&ctm);
    pdf_matrix_reset(&text_matrix);
    pdf_matrix_reset(&line_matrix);
    while (position < size) {
        char ch = (char)stream[position];
        if (ch == '%') {
            while (position < size && stream[position] != '\n' &&
                   stream[position] != '\r') ++position;
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
            ch == '\f' || ch == '\0') {
            ++position;
            continue;
        }
        if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' ||
            ch == '(' || ch == '[' || ch == ']' || ch == '<' || ch == '>' ||
            ch == '/') {
            if (ch == '(') {
                position = pdf_scan_literal_string(
                    (const char *)stream, size, position, last_string,
                    sizeof(last_string), &last_string_size);
                continue;
            }
            if (ch == '[') {
                array_start = position;
                array_end = position;
                ++position;
                continue;
            }
            if (ch == ']') {
                array_end = position;
                ++position;
                continue;
            }
            if (ch == '<') {
                if (position + 1u < size && stream[position + 1u] == '<') {
                    position += 2u; /* dictionary, not a string */
                } else {
                    position = pdf_scan_hex_string(
                        (const char *)stream, size, position, last_string,
                        sizeof(last_string), &last_string_size);
                }
                continue;
            }
            if (ch == '>') {
                ++position;
                continue;
            }
            if (ch == '/') {
                size_t name_start = ++position;
                while (position < size && (unsigned char)stream[position] > 32u &&
                       stream[position] != '/')
                    ++position;
                {
                    size_t name_length = position - name_start;
                    if (name_length >= sizeof(last_name)) name_length = sizeof(last_name) - 1u;
                    memcpy(last_name, stream + name_start, name_length);
                    last_name[name_length] = '\0';
                }
                continue;
            }
            {
                char *end = NULL;
                double value = strtod((const char *)stream + position, &end);
                if (end && end != (char *)stream + position) {
                    if (number_count < 8u) numbers[number_count++] = value;
                    position = (size_t)(end - (const char *)stream);
                    continue;
                }
            }
            ++position;
            continue;
        }
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            ch == '*' || ch == '\'' || ch == '"') {
            char operator_name[4];
            size_t operator_length = 0u;
            while (position < size && operator_length < 3u &&
                   (((unsigned char)stream[position] >= 'a' &&
                     (unsigned char)stream[position] <= 'z') ||
                    ((unsigned char)stream[position] >= 'A' &&
                     (unsigned char)stream[position] <= 'Z') ||
                    stream[position] == '*' || stream[position] == '\'' ||
                    stream[position] == '"')) {
                operator_name[operator_length++] =
                    (char)stream[position++];
            }
            operator_name[operator_length] = '\0';
            if (strcmp(operator_name, "q") == 0 && depth < 32u) {
                stack[depth++] = ctm;
            } else if (strcmp(operator_name, "Q") == 0 && depth > 0u) {
                ctm = stack[--depth];
            } else if (strcmp(operator_name, "cm") == 0 && number_count >= 6u) {
                double matrix[6];
                size_t index;
                for (index = 0u; index < 6u; ++index)
                    matrix[index] = numbers[number_count - 6u + index];
                pdf_matrix_concat(&ctm, matrix);
            } else if ((strcmp(operator_name, "rg") == 0 ||
                        strcmp(operator_name, "RG") == 0) &&
                       number_count >= 3u) {
                size_t index;
                for (index = 0u; index < 3u; ++index) {
                    double value = numbers[number_count - 3u + index];
                    fill[index] = (uint8_t)(value <= 0.0
                                                ? 0
                                                : value >= 1.0
                                                      ? 255
                                                      : (int)(value * 255.0 + 0.5));
                }
            } else if ((strcmp(operator_name, "g") == 0 ||
                        strcmp(operator_name, "G") == 0) &&
                       number_count >= 1u) {
                double value = numbers[number_count - 1u];
                uint8_t gray = (uint8_t)(value <= 0.0
                                             ? 0
                                             : value >= 1.0
                                                   ? 255
                                                   : (int)(value * 255.0 + 0.5));
                fill[0] = gray; fill[1] = gray; fill[2] = gray;
            } else if (strcmp(operator_name, "re") == 0 && number_count >= 4u) {
                pdf_paint_rect(pixels, width, height, &ctm,
                               numbers[number_count - 4u],
                               numbers[number_count - 3u],
                               numbers[number_count - 2u],
                               numbers[number_count - 1u], fill);
                ++*painted;
            } else if ((strcmp(operator_name, "m") == 0 ||
                        strcmp(operator_name, "l") == 0) &&
                       number_count >= 2u) {
                pdf_path_add(&path, &ctm, numbers[number_count - 2u],
                             numbers[number_count - 1u]);
            } else if ((strcmp(operator_name, "c") == 0 && number_count >= 6u) ||
                       (strcmp(operator_name, "v") == 0 && number_count >= 4u) ||
                       (strcmp(operator_name, "y") == 0 && number_count >= 4u)) {
                pdf_path_add(&path, &ctm, numbers[number_count - 2u],
                             numbers[number_count - 1u]);
            } else if (strcmp(operator_name, "h") == 0) {
                /* closed by the polygon fill; nothing else to record */
            } else if (strcmp(operator_name, "f") == 0 ||
                       strcmp(operator_name, "F") == 0 ||
                       strcmp(operator_name, "f*") == 0) {
                pdf_fill_path(pixels, width, height, &path, fill);
                if (path.count >= 3u) ++*painted;
                pdf_path_reset(&path);
            } else if (strcmp(operator_name, "S") == 0 ||
                       strcmp(operator_name, "s") == 0) {
                pdf_stroke_path(pixels, width, height, &path, fill);
                if (path.count >= 2u) ++*painted;
                pdf_path_reset(&path);
            } else if (strcmp(operator_name, "n") == 0) {
                pdf_path_reset(&path);
            } else if (strcmp(operator_name, "Do") == 0 && last_name[0]) {
                size_t image_index;
                int drawn = 0;
                for (image_index = 0u; image_index < image_count; ++image_index) {
                    if (strcmp(images[image_index].name, last_name) == 0) {
                        drawn = pdf_draw_image_xobject(reader,
                                                       images[image_index].object,
                                                       pixels, width, height,
                                                       &ctm);
                        break;
                    }
                }
                if (drawn) ++*painted;
                else *skipped_text_or_image = 1;
            } else if (strcmp(operator_name, "BT") == 0) {
                pdf_matrix_reset(&text_matrix);
                pdf_matrix_reset(&line_matrix);
            } else if (strcmp(operator_name, "Tf") == 0 && number_count >= 1u) {
                double size_value = numbers[number_count - 1u];
                font_size = size_value > 0.0 ? size_value : 12.0;
                if (font_size > 1000.0) font_size = 1000.0;
            } else if (strcmp(operator_name, "TL") == 0 && number_count >= 1u) {
                leading = numbers[number_count - 1u];
            } else if (strcmp(operator_name, "Tc") == 0 && number_count >= 1u) {
                char_spacing = numbers[number_count - 1u];
            } else if (strcmp(operator_name, "Tw") == 0 && number_count >= 1u) {
                word_spacing = numbers[number_count - 1u];
            } else if (strcmp(operator_name, "Tz") == 0 && number_count >= 1u) {
                hscale = numbers[number_count - 1u] / 100.0;
                if (hscale <= 0.0) hscale = 1.0;
            } else if (strcmp(operator_name, "Tm") == 0 && number_count >= 6u) {
                size_t base = number_count - 6u;
                line_matrix.a = numbers[base + 0u];
                line_matrix.b = numbers[base + 1u];
                line_matrix.c = numbers[base + 2u];
                line_matrix.d = numbers[base + 3u];
                line_matrix.e = numbers[base + 4u];
                line_matrix.f = numbers[base + 5u];
                text_matrix = line_matrix;
            } else if ((strcmp(operator_name, "Td") == 0 ||
                        strcmp(operator_name, "TD") == 0) &&
                       number_count >= 2u) {
                double tx = numbers[number_count - 2u];
                double ty = numbers[number_count - 1u];
                if (strcmp(operator_name, "TD") == 0) leading = -ty;
                line_matrix.e += tx * line_matrix.a + ty * line_matrix.c;
                line_matrix.f += tx * line_matrix.b + ty * line_matrix.d;
                text_matrix = line_matrix;
            } else if (strcmp(operator_name, "T*") == 0) {
                line_matrix.e += -leading * line_matrix.c;
                line_matrix.f += -leading * line_matrix.d;
                text_matrix = line_matrix;
            } else if (operator_length == 1u && operator_name[0] == '\'') {
                /* ' = next line, then show the pending string. */
                line_matrix.e += -leading * line_matrix.c;
                line_matrix.f += -leading * line_matrix.d;
                text_matrix = line_matrix;
                if (last_string_size) {
                    size_t drawn = 0u;
                    pdf_paint_string(last_string, last_string_size, pixels,
                                     width, height, &ctm, &text_matrix,
                                     font_size, hscale, char_spacing,
                                     word_spacing, fill, &drawn);
                    if (drawn) ++*painted;
                    else text_skipped = 1;
                }
                last_string_size = 0u;
            } else if (strcmp(operator_name, "Tj") == 0) {
                if (last_string_size) {
                    size_t drawn = 0u;
                    pdf_paint_string(last_string, last_string_size, pixels,
                                     width, height, &ctm, &text_matrix,
                                     font_size, hscale, char_spacing,
                                     word_spacing, fill, &drawn);
                    if (drawn) ++*painted;
                    else text_skipped = 1;
                }
                last_string_size = 0u;
            } else if (strcmp(operator_name, "TJ") == 0 &&
                       array_end > array_start) {
                size_t drawn = 0u;
                pdf_paint_tj_array((const char *)stream, array_start,
                                   array_end + 1u, pixels, width, height,
                                   &ctm, &text_matrix, font_size, hscale,
                                   char_spacing, word_spacing, fill, &drawn);
                if (drawn) ++*painted;
                else text_skipped = 1;
                last_string_size = 0u;
            } else if (operator_name[0] == '"' && number_count >= 2u) {
                word_spacing = numbers[number_count - 2u];
                char_spacing = numbers[number_count - 1u];
                line_matrix.e += -leading * line_matrix.c;
                line_matrix.f += -leading * line_matrix.d;
                text_matrix = line_matrix;
                if (last_string_size) {
                    size_t drawn = 0u;
                    pdf_paint_string(last_string, last_string_size, pixels,
                                     width, height, &ctm, &text_matrix,
                                     font_size, hscale, char_spacing,
                                     word_spacing, fill, &drawn);
                    if (drawn) ++*painted;
                    else text_skipped = 1;
                }
                last_string_size = 0u;
            } else if (strcmp(operator_name, "ET") == 0) {
                /* nothing to restore */
            }
            number_count = 0u;
            continue;
        }
        ++position;
    }
    if (text_skipped) *skipped_text_or_image = 1;
    return 1;
}

static int pdf_parse_box_field(const char *dict, size_t size, const char *key,
                               double box[4])
{
    const char *found = pdf_memmem(dict, size, key, strlen(key));
    const char *cursor;
    size_t index;
    if (!found) return 0;
    cursor = memchr(found, '[', size - (size_t)(found - dict));
    if (!cursor) return 0;
    ++cursor;
    for (index = 0u; index < 4u; ++index) {
        char *end = NULL;
        double value = strtod(cursor, &end);
        if (!end || end == cursor) return 0;
        box[index] = value;
        cursor = end;
    }
    return box[2] > box[0] && box[3] > box[1];
}

/* Media/crop boxes and rotation, defaulting to US Letter. A malformed box must
 * not hide the page: it falls back to the default instead of failing. */
static void pdf_page_boxes(const QwPdfReader *reader, size_t page_object,
                           double media[4], double crop[4], int *rotation)
{
    const char *body = NULL;
    size_t dict_size = 0u;
    double parsed[4];
    media[0] = 0.0;
    media[1] = 0.0;
    media[2] = 612.0;
    media[3] = 792.0;
    *rotation = 0;
    if (!pdf_object_body(reader, page_object, &body, &dict_size)) {
        memcpy(crop, media, sizeof(parsed));
        return;
    }
    if (pdf_parse_box_field(body, dict_size, "/MediaBox", parsed)) {
        memcpy(media, parsed, sizeof(parsed));
    } else if (pdf_parse_box_field(body, dict_size, "/CropBox", parsed)) {
        memcpy(media, parsed, sizeof(parsed));
    }
    if (pdf_parse_box_field(body, dict_size, "/CropBox", parsed)) {
        memcpy(crop, parsed, sizeof(parsed));
    } else {
        memcpy(crop, media, sizeof(parsed));
    }
    {
        long value = pdf_parse_int_field(body, dict_size, "/Rotate", 0);
        value %= 360;
        if (value < 0) value += 360;
        if (value % 90 != 0) value = 0;
        *rotation = (int)value;
    }
}

static int pdf_page_media_box(const QwPdfReader *reader, size_t page_object,
                              double box[4])
{
    double crop[4];
    int rotation = 0;
    pdf_page_boxes(reader, page_object, box, crop, &rotation);
    return 1;
}

int qw_pdf_reader_page_geometry(const QwPdfReader *reader, size_t page_index,
                                double media[4], double crop[4],
                                int *rotation)
{
    if (!reader || !media || !crop || !rotation ||
        page_index >= reader->page_count) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    pdf_page_boxes(reader, reader->page_objects[page_index], media, crop,
                   rotation);
    return QW_STATUS_OK;
}

int qw_pdf_reader_render_page(QwPdfReader *reader, size_t page_index,
                                    unsigned int dpi, QwPageImage *out_page,
                                    QwConvertReport *report)
{
    double box[4];
    double width_points;
    double height_points;
    size_t width;
    size_t height;
    size_t stride;
    size_t size;
    uint8_t *pixels;
    if (!reader || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    if (reader->encryption_unsupported) {
        if (report) {
            snprintf(report->note, sizeof(report->note),
                     "Encrypted PDF: content requires the user password (structure-only open)");
        }
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (page_index >= reader->page_count) return QW_STATUS_ERR_FORMAT;
    if (dpi == 0u) dpi = 72u;
    if (dpi > 1200u) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (!pdf_page_media_box(reader, reader->page_objects[page_index], box))
        return QW_STATUS_ERR_FORMAT;
    width_points = box[2] - box[0];
    height_points = box[3] - box[1];
    width = (size_t)((width_points * (double)dpi) / 72.0 + 0.999999);
    height = (size_t)((height_points * (double)dpi) / 72.0 + 0.999999);
    if (!width || !height) return QW_STATUS_ERR_FORMAT;
    if (width > 100000u || height > 100000u ||
        width > (size_t)-1 / 3u ||
        height > (size_t)-1 / (width * 3u))
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    stride = width * 3u;
    size = stride * height;
    pixels = (uint8_t *)malloc(size);
    if (!pixels) return QW_STATUS_ERR_OUT_OF_MEMORY;
    memset(pixels, 0xff, size);
    out_page->data = pixels;
    out_page->size = size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = (uint32_t)width;
    out_page->height = (uint32_t)height;
    {
        const char *body = NULL;
        size_t dict_size = 0u;
        size_t contents = 0u;
        size_t painted = 0u;
        int skipped_text_or_image = 0;
        PdfImageRef images[16];
        size_t image_count = pdf_collect_image_xobjects(
            reader, reader->page_objects[page_index], images, 16u);
        if (pdf_object_body(reader, reader->page_objects[page_index], &body,
                            &dict_size)) {
            char *dict = (char *)malloc(dict_size + 1u);
            if (dict) {
                memcpy(dict, body, dict_size);
                dict[dict_size] = '\0';
                if (pdf_find_reference(dict, dict_size, "/Contents", &contents)) {
                    size_t stream_size = 0u;
                    uint8_t *stream = pdf_object_stream(reader, contents,
                                                        &stream_size);
                    if (stream) {
                        (void)pdf_paint_content(reader, images, image_count,
                                                stream, stream_size, pixels,
                                                width, height, &painted,
                                                &skipped_text_or_image);
                        free(stream);
                    }
                }
                free(dict);
            }
        }
        if (report) {
            report->pages_written = 1u;
            snprintf(report->note, sizeof(report->note),
                     "PDF rasterisation: %ux%u at %u dpi; %zu painted object(s); "
                     "text uses a built-in approximation font%s "
                     "(QW_ROUTE_LOSS_RASTERIZATION|"
                     "QW_ROUTE_LOSS_SEMANTICS_UNVERIFIED)",
                     (unsigned int)width, (unsigned int)height, dpi, painted,
                     skipped_text_or_image ? "; text/images not painted" : "");
        }
    }
    return QW_STATUS_OK;
}

void qw_pdf_reader_close(QwPdfReader *reader)
{
    pdf_reader_free(reader);
}

size_t qw_pdf_reader_page_object(const QwPdfReader *reader, size_t index)
{
    if (!reader || index >= reader->page_count) return 0u;
    return reader->page_objects[index];
}

int qw_pdf_render_page_memory(const uint8_t *data, size_t size, size_t page_index,
                             unsigned int dpi, QwPageImage *out_page,
                             QwConvertReport *report)
{
    QwPdfReader *reader = NULL;
    size_t page_count = 0u;
    int rc;
    if (out_page) memset(out_page, 0u, sizeof(*out_page));
    rc = qw_pdf_reader_open(data, size, &reader, &page_count, NULL);
    if (rc != QW_STATUS_OK) return rc;
    rc = qw_pdf_reader_render_page(reader, page_index, dpi, out_page, report);
    qw_pdf_reader_close(reader);
    return rc;
}

int qw_doc_convert_memory_with_outline(const uint8_t *data, size_t size,
                                         const char *input_extension,
                                         const QwPdfOutlineItem *outline,
                                         size_t outline_count,
                                         uint8_t **out_pdf, size_t *out_size,
                                         QwConvertReport *report)
{
    (void)outline;
    (void)outline_count;
    return qw_doc_convert_memory(data, size, input_extension, 0, out_pdf,
                                   out_size, report);
}

int qw_doc_decode_caj_jp2(const uint8_t *data, size_t size, QwPageImage *out_page,
                            QwConvertReport *report)
{
    QwImage image;
    uint8_t *copy;
    if (!data || !size || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0u, sizeof(*out_page));
    memset(&image, 0, sizeof(image));
    if (qw_image_decode(data, size, &image) != QW_IMAGE_OK) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "CAJ JP2 payload could not be decoded by the internal image decoder");
        return QW_STATUS_ERR_FORMAT;
    }
    copy = (uint8_t *)malloc(image.size);
    if (!copy) {
        qw_image_free(&image);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, image.pixels, image.size);
    out_page->data = copy;
    out_page->size = image.size;
    out_page->width = image.width;
    out_page->height = image.height;
    out_page->kind = image.format == QW_IMAGE_PIXEL_RGB24 ? QW_PAGE_IMAGE_RGB24
                                                          : QW_PAGE_IMAGE_GRAY8;
    qw_image_free(&image);
    if (report) snprintf(report->note, sizeof(report->note),
                         "CAJ JP2 payload decoded by the internal image decoder");
    return QW_STATUS_OK;
}
