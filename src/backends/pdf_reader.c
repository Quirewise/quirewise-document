/* Internal PDF reading support for the former MuPDF backend interface.
 *
 * Implemented behaviour:
 *  - `qw_mupdf_pdf_reader_open` walks a classic PDF object table, counts pages
 *    and rejects encrypted or object-stream-only documents with a stable
 *    status;
 *  - `qw_mupdf_pdf_reader_extract_text` decodes page content streams
 *    (FlateDecode or raw), interprets the text operators (Tj, TJ, ', ") and
 *    returns UTF-8 text; Type0 fonts are decoded as UCS-2 big endian, simple
 *    fonts as single-byte WinAnsi/ASCII.
 *
 * Explicitly unimplemented (stable `QW_STATUS_ERR_UNSUPPORTED`): rasterisation,
 * PDF merging, conversion from arbitrary input formats and CAJ JP2 decoding.
 * There is no MuPDF code, header or library involved. */

#include "mupdf_backend.h"

#include <quirewise/compression.h>
#include <quirewise/image.h>
#include <quirewise/crypto.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QW_PDF_READER_MAX_PAGES 100000u
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

struct QwMuPdfReader {
    const uint8_t *data;
    size_t size;
    int encrypted;
    uint8_t encryption_key[16];
    size_t encryption_key_size;
    size_t *page_objects;
    size_t page_count;
    QwPdfObjectSlice *slices;
    size_t slice_count;
    char **buffers;
    size_t buffer_count;
    char *text;
    size_t text_size;
    int text_ready;
};

static void pdf_reader_free(QwMuPdfReader *reader)
{
    size_t index;
    if (!reader) return;
    for (index = 0u; index < reader->buffer_count; ++index) {
        free(reader->buffers[index]);
    }
    free(reader->buffers);
    free(reader->slices);
    free(reader->page_objects);
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
    } else {
        encoded[0] = (char)(0xE0u | (code >> 12));
        encoded[1] = (char)(0x80u | ((code >> 6) & 0x3Fu));
        encoded[2] = (char)(0x80u | (code & 0x3Fu));
        length = 3u;
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

/* Find "<number> 0 obj" with digit-boundary checks and return the offset of the
 * dictionary body. */
static const char *pdf_find_object(const QwMuPdfReader *reader, size_t object)
{
    char pattern[32];
    int length = snprintf(pattern, sizeof(pattern), "%zu 0 obj", object);
    const char *cursor = (const char *)reader->data;
    size_t remaining = reader->size;
    if (length <= 0) return NULL;
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
static int pdf_object_number_before(const QwMuPdfReader *reader, const char *position,
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
static int pdf_raw_object_body(const QwMuPdfReader *reader, size_t object,
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
static int pdf_object_body(const QwMuPdfReader *reader, size_t object,
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

/* Register every object stored inside a decompressed /Type /ObjStm stream. */
static int pdf_register_object_stream(QwMuPdfReader *reader, size_t owner)
{
    const char *body;
    size_t body_size;
    const char *data;
    size_t data_size;
    size_t stream_offset;
    size_t payload_size;
    const char *endstream;
    uint8_t *inflated = NULL;
    size_t inflated_size = 0u;
    size_t count = 0u;
    size_t first = 0u;
    const char *cursor;
    const char *end;
    size_t index;
    size_t capacity = 0u;

    if (!pdf_raw_object_body(reader, owner, &body, &body_size)) return 1;
    {
        const char *stream = pdf_memmem(body, body_size, "stream", 6u);
        if (!stream) return 1;
        data = (const char *)reader->data;
        data_size = reader->size;
        stream_offset = (size_t)(stream - data) + 6u;
        while (stream_offset < data_size &&
               (reader->data[stream_offset] == '\r' || reader->data[stream_offset] == '\n')) {
            ++stream_offset;
        }
        endstream = pdf_memmem(data + stream_offset, data_size - stream_offset,
                               "endstream", 9u);
        if (!endstream) return 1;
        payload_size = (size_t)(endstream - (data + stream_offset));
        while (payload_size &&
               (reader->data[stream_offset + payload_size - 1u] == '\n' ||
                reader->data[stream_offset + payload_size - 1u] == '\r')) {
            --payload_size;
        }
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
        size_t capacity_guess = payload_size * 6u + 4096u;
        size_t attempt;
        for (attempt = 0u; attempt < 4u; ++attempt) {
            size_t produced = 0u;
            QwCompressionInflateStatus status;
            if (capacity_guess > QW_PDF_READER_MAX_STREAM) return 1;
            inflated = (uint8_t *)malloc(capacity_guess);
            if (!inflated) return 0;
            status = qw_compression_inflate_zlib_bounded(
                reader->data + stream_offset, payload_size, inflated,
                capacity_guess, &produced);
            if (status == QW_COMPRESSION_INFLATE_OK) {
                inflated_size = produced;
                break;
            }
            free(inflated);
            inflated = NULL;
            if (status != QW_COMPRESSION_INFLATE_OUTPUT_FULL) return 1;
            capacity_guess *= 4u;
        }
    }
    if (!inflated || inflated_size <= first) {
        free(inflated);
        return 1;
    }
    if (reader->buffer_count >= 64u) { free(inflated); return 1; }
    cursor = (const char *)inflated;
    end = cursor + inflated_size;
    for (index = 0u; index < count; ++index) {
        unsigned long number = strtoul(cursor, (char **)&cursor, 10);
        unsigned long offset = strtoul(cursor, (char **)&cursor, 10);
        size_t start;
        size_t stop;
        if (cursor >= end) break;
        (void)number;
        (void)offset;
    }
    /* Second pass with a fresh cursor: header pairs are "num offset" repeated. */
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
            if (reader->slice_count >= capacity) {
                size_t grown = capacity ? capacity * 2u : 64u;
                QwPdfObjectSlice *resized = (QwPdfObjectSlice *)realloc(
                    reader->slices, grown * sizeof(*resized));
                if (!resized) return 0;
                reader->slices = resized;
                capacity = grown;
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
    const char *found = pdf_memmem(dict, size, tag, strlen(tag));
    const char *cursor;
    size_t count = 0u;
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
    const char *found = pdf_memmem(dict, size, tag, strlen(tag));
    char *end = NULL;
    long value;
    if (!found) return fallback;
    value = strtol(found + strlen(tag), &end, 10);
    if (end == found + strlen(tag)) return fallback;
    return value;
}

static int pdf_setup_encryption(QwMuPdfReader *reader)
{
    const char *found;
    size_t encrypt_object = 0u;
    const char *dict = NULL;
    size_t dict_size = 0u;
    char *copy;
    uint8_t o_value[48], u_value[48], id_value[64];
    size_t o_size = 0u, u_size = 0u, id_size = 0u;
    long revision, version, length_bits, permissions;
    size_t key_size;
    uint8_t md5_input[128];
    uint8_t digest[16];
    size_t md5_size;
    size_t i;
    found = pdf_memmem((const char *)reader->data, reader->size, "/Encrypt",
                       8u);
    if (!found) return 1;
    {
        char *end = NULL;
        encrypt_object = (size_t)strtoul(found + 8, &end, 10);
        if (!encrypt_object) return -1;
    }
    if (!pdf_object_body(reader, encrypt_object, &dict, &dict_size)) return -1;
    copy = (char *)malloc(dict_size + 1u);
    if (!copy) return -1;
    memcpy(copy, dict, dict_size);
    copy[dict_size] = '\0';
    version = pdf_parse_int_field(copy, dict_size, "/V", -1);
    revision = pdf_parse_int_field(copy, dict_size, "/R", -1);
    length_bits = pdf_parse_int_field(copy, dict_size, "/Length", 40);
    permissions = pdf_parse_int_field(copy, dict_size, "/P", 0);
    if (!pdf_parse_hex_field(copy, dict_size, "/O", o_value, sizeof(o_value),
                             &o_size) ||
        !pdf_parse_hex_field(copy, dict_size, "/U", u_value, sizeof(u_value),
                             &u_size) ||
        (version != 1 && version != 2) || (revision != 2 && revision != 3) ||
        o_size < 32u || u_size < 32u) {
        free(copy);
        return -1;
    }
    free(copy);
    if (length_bits < 40 || length_bits > 128 || (length_bits % 8) != 0)
        return -1;
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
            for (i = 1u; i < 20u; ++i) {
                pdf_rc4(reader->encryption_key, key_size, x, 16u);
            }
            if (memcmp(x, u_value, 16u) != 0) return -1;
        }
    }
    reader->encrypted = 1;
    return 0;
}

static void pdf_decrypt_object_data(const QwMuPdfReader *reader, size_t object,
                                    uint8_t *data, size_t size)
{
    uint8_t extra[5];
    uint8_t digest[16];
    size_t digest_size;
    if (!reader->encrypted || !size) return;
    extra[0] = (uint8_t)(object & 0xffu);
    extra[1] = (uint8_t)((object >> 8) & 0xffu);
    extra[2] = (uint8_t)((object >> 16) & 0xffu);
    extra[3] = 0u;
    extra[4] = 0u;
    digest_size = reader->encryption_key_size + 5u;
    if (digest_size > sizeof(digest)) digest_size = sizeof(digest);
    memcpy(digest, reader->encryption_key, reader->encryption_key_size);
    memcpy(digest + reader->encryption_key_size, extra, 5u);
    {
        uint8_t final_digest[16];
        if (qw_crypto_md5_digest(digest, reader->encryption_key_size + 5u,
                                 final_digest) != 0)
            return;
        memcpy(digest, final_digest, sizeof(digest));
    }
    pdf_rc4(digest, digest_size, data, size);
}

static uint8_t *pdf_object_stream(const QwMuPdfReader *reader, size_t object,
                                  size_t *out_size)
{
    const char *body;
    size_t dict_size;
    const char *stream;
    const char *endstream;
    const char *dict_end;
    size_t stream_offset;
    size_t payload_size;
    int flate = 0;
    uint8_t *output;

    *out_size = 0u;
    if (!pdf_object_body(reader, object, &body, &dict_size)) return NULL;
    stream = pdf_memmem(body, dict_size, "stream", 6u);
    if (!stream) return NULL;
    dict_end = stream;
    if (pdf_memmem(body, (size_t)(dict_end - body), "/FlateDecode", 12u)) flate = 1;
    stream_offset = (size_t)(stream - (const char *)reader->data) + 6u;
    while (stream_offset < reader->size &&
           (reader->data[stream_offset] == '\r' || reader->data[stream_offset] == '\n')) {
        ++stream_offset;
    }
    endstream = pdf_memmem((const char *)reader->data + stream_offset,
                           reader->size - stream_offset, "endstream", 9u);
    if (!endstream) return NULL;
    payload_size = (size_t)(endstream - ((const char *)reader->data + stream_offset));
    while (payload_size && (reader->data[stream_offset + payload_size - 1u] == '\n' ||
                            reader->data[stream_offset + payload_size - 1u] == '\r')) {
        --payload_size;
    }
    if (!payload_size || payload_size > QW_PDF_READER_MAX_STREAM) return NULL;
    if (reader->encrypted) {
        uint8_t *decrypted = (uint8_t *)malloc(payload_size);
        if (!decrypted) return NULL;
        memcpy(decrypted, reader->data + stream_offset, payload_size);
        pdf_decrypt_object_data(reader, object, decrypted, payload_size);
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
        memcpy(output, reader->data + stream_offset, payload_size);
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
                reader->data + stream_offset, payload_size, output, capacity, &produced);
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

static int pdf_object_is_type0(const QwMuPdfReader *reader, size_t object)
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

/* Map a font resource name (e.g. "/F2") to a Type0 flag. */
typedef struct QwPdfFontMap {
    char names[32][24];
    int type0[32];
    size_t count;
} QwPdfFontMap;

static void pdf_collect_fonts(const QwMuPdfReader *reader, const char *dict,
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
            fonts->count += 1u;
        }
        cursor = slash + 1u;
    }
}

static int pdf_font_is_type0(const QwPdfFontMap *fonts, const char *name)
{
    size_t index;
    if (!fonts || !name) return 0;
    for (index = 0u; index < fonts->count; ++index) {
        if (strcmp(fonts->names[index], name) == 0) return fonts->type0[index];
    }
    return 0;
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

static int pdf_emit_string(QwPdfBuffer *text, const QwPdfBuffer *raw, int type0)
{
    size_t index = 0u;
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

static int pdf_extract_content_text(const QwMuPdfReader *reader, const char *content,
                                    size_t content_size, const QwPdfFontMap *fonts,
                                    QwPdfBuffer *text)
{
    const char *cursor = content;
    const char *end = content + content_size;
    char font_name[24] = "/F1";
    int type0 = 0;
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
            if (!pdf_emit_string(text, &raw, type0)) { free(raw.data); return 0; }
            free(raw.data);
            continue;
        }
        if (*cursor == '<' && cursor + 1 < end && cursor[1] != '<') {
            QwPdfBuffer raw = {NULL, 0u, 0u};
            cursor = pdf_read_hex_string(cursor + 1, end, &raw);
            if (!pdf_emit_string(text, &raw, type0)) { free(raw.data); return 0; }
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
            type0 = pdf_font_is_type0(fonts, font_name);
        } else if ((token_length == 2u && memcmp(token, "Tj", 2u) == 0) ||
                   (token_length == 2u && memcmp(token, "'", 1u) == 0)) {
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

static int pdf_extract_page_text(const QwMuPdfReader *reader, size_t page_object,
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
        free(dict_copy);
        return 1; /* page without content: valid, contributes no text */
    }
    free(dict_copy);
    stream = pdf_object_stream(reader, contents, &stream_size);
    if (!stream) return 0;
    ok = pdf_extract_content_text(reader, (const char *)stream, stream_size,
                                  &fonts, text);
    free(stream);
    return ok;
}

int qw_mupdf_pdf_reader_open(const uint8_t *data, size_t size,
                             QwMuPdfReader **out_reader,
                             size_t *out_page_count,
                             QwConvertReport *report)
{
    QwMuPdfReader *reader;
    size_t capacity = 0u;
    size_t count = 0u;
    if (!data || !size || !out_reader || !out_page_count) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_reader = NULL;
    *out_page_count = 0u;
    if (size < 8u || memcmp(data, "%PDF-", 5u) != 0) return QW_STATUS_ERR_FORMAT;
    reader = (QwMuPdfReader *)calloc(1u, sizeof(*reader));
    if (!reader) return QW_STATUS_ERR_OUT_OF_MEMORY;
    reader->data = data;
    reader->size = size;
    {
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
        const char *cursor = (const char *)data;
        size_t remaining = size;
        while (remaining > 6u) {
            const char *found = pdf_find_typed(cursor, remaining, "Page");
            size_t object = 0u;
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
    /* Pages stored inside object streams. */
    if (count < QW_PDF_READER_MAX_PAGES) {
        size_t slice_index;
        for (slice_index = 0u; slice_index < reader->slice_count; ++slice_index) {
            const char *found = pdf_find_typed(reader->slices[slice_index].data,
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
                             "No /Type /Page objects were found; compressed object streams are not supported");
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (pdf_memmem((const char *)data, size, "/Encrypt", 8u)) {
        int encryption_status;
        reader->page_count = count;
        encryption_status = pdf_setup_encryption(reader);
        if (encryption_status == -1) {
            pdf_reader_free(reader);
            if (report) snprintf(report->note, sizeof(report->note),
                                 "Encrypted PDF: unsupported security handler or wrong user password");
            return QW_STATUS_ERR_UNSUPPORTED;
        }
    }
    reader->page_count = count;
    *out_reader = reader;
    *out_page_count = count;
    if (report) snprintf(report->note, sizeof(report->note),
                         "Opened %zu PDF page(s) with the internal Quirewise reader", count);
    return QW_STATUS_OK;
}

int qw_mupdf_pdf_reader_extract_text(QwMuPdfReader *reader, uint8_t **out_text,
                                     size_t *out_size, QwConvertReport *report)
{
    QwPdfBuffer text;
    size_t index;
    if (!reader || !out_text || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_text = NULL;
    *out_size = 0u;
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

int qw_mupdf_pdf_reader_extract_text_to_stream(QwMuPdfReader *reader, FILE *stream,
                                               size_t max_output_bytes,
                                               size_t *out_size,
                                               QwConvertReport *report)
{
    uint8_t *text = NULL;
    size_t text_size = 0u;
    int rc;
    if (!stream) return QW_STATUS_ERR_INVALID_ARGUMENT;
    rc = qw_mupdf_pdf_reader_extract_text(reader, &text, &text_size, report);
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
    if (px0 > px1) { double swap = px0; px0 = px1; px1 = swap; }
    if (py0 > py1) { double swap = py0; py0 = py1; py1 = swap; }
    left = px0 < 0.0 ? 0u : (size_t)px0;
    right = px1 > (double)width ? width : (size_t)(px1 + 0.999);
    bottom = py0 < 0.0 ? 0u : (size_t)py0;
    top = py1 > (double)height ? height : (size_t)(py1 + 0.999);
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

static size_t pdf_collect_image_xobjects(const QwMuPdfReader *reader,
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
            size_t source_y = (size_t)((row - bottom) * (size_t)image->height /
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

static int pdf_draw_image_xobject(const QwMuPdfReader *reader, size_t object,
                                  uint8_t *pixels, size_t width, size_t height,
                                  const PdfMatrix *ctm)
{
    size_t stream_size = 0u;
    uint8_t *stream = pdf_object_stream(reader, object, &stream_size);
    QwImage image;
    int ok = 0;
    if (!stream) return 0;
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

static int pdf_paint_content(const QwMuPdfReader *reader,
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
    last_name[0] = '\0';
    pdf_path_reset(&path);
    pdf_matrix_reset(&ctm);
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
                int level = 1;
                ++position;
                while (position < size && level > 0) {
                    if (stream[position] == '\\' && position + 1u < size) {
                        position += 2u;
                        continue;
                    }
                    if (stream[position] == '(') ++level;
                    else if (stream[position] == ')') --level;
                    ++position;
                }
                continue;
            }
            if (ch == '[' || ch == ']' || ch == '<' || ch == '>') {
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
            } else if (strcmp(operator_name, "Tj") == 0 ||
                       strcmp(operator_name, "TJ") == 0 ||
                       strcmp(operator_name, "BT") == 0) {
                *skipped_text_or_image = 1;
            }
            number_count = 0u;
            continue;
        }
        ++position;
    }
    return 1;
}

static int pdf_page_media_box(const QwMuPdfReader *reader, size_t page_object,
                              double box[4])
{
    const char *body = NULL;
    size_t dict_size = 0u;
    char *copy;
    const char *found;
    const char *cursor;
    size_t index;
    box[0] = 0.0; box[1] = 0.0; box[2] = 612.0; box[3] = 792.0;
    if (!pdf_object_body(reader, page_object, &body, &dict_size)) return 0;
    copy = (char *)malloc(dict_size + 1u);
    if (!copy) return 0;
    memcpy(copy, body, dict_size);
    copy[dict_size] = '\0';
    found = strstr(copy, "/MediaBox");
    if (!found) found = strstr(copy, "/CropBox");
    if (found && (cursor = strchr(found, '[')) != NULL) {
        for (index = 0u; index < 4u; ++index) {
            char *end = NULL;
            double value = strtod(cursor + 1, &end);
            if (end == cursor + 1) break;
            box[index] = value;
            cursor = end;
        }
    }
    free(copy);
    if (box[2] <= box[0] || box[3] <= box[1]) return 0;
    return 1;
}

int qw_mupdf_pdf_reader_render_page(QwMuPdfReader *reader, size_t page_index,
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
                     "PDF rasterisation: %ux%u at %u dpi; %zu painted object(s) "
                     "painted%s (QW_ROUTE_LOSS_RASTERIZATION|"
                     "QW_ROUTE_LOSS_SEMANTICS_UNVERIFIED)",
                     (unsigned int)width, (unsigned int)height, dpi, painted,
                     skipped_text_or_image ? "; text/images not painted" : "");
        }
    }
    return QW_STATUS_OK;
}

void qw_mupdf_pdf_reader_close(QwMuPdfReader *reader)
{
    pdf_reader_free(reader);
}

int qw_mupdf_render_pdf_page(const uint8_t *data, size_t size, size_t page_index,
                             unsigned int dpi, QwPageImage *out_page,
                             QwConvertReport *report)
{
    QwMuPdfReader *reader = NULL;
    size_t page_count = 0u;
    int rc;
    if (out_page) memset(out_page, 0u, sizeof(*out_page));
    rc = qw_mupdf_pdf_reader_open(data, size, &reader, &page_count, NULL);
    if (rc != QW_STATUS_OK) return rc;
    rc = qw_mupdf_pdf_reader_render_page(reader, page_index, dpi, out_page, report);
    qw_mupdf_pdf_reader_close(reader);
    return rc;
}

int qw_mupdf_convert_memory_with_outline(const uint8_t *data, size_t size,
                                         const char *input_extension,
                                         const QwMuPdfOutlineItem *outline,
                                         size_t outline_count,
                                         uint8_t **out_pdf, size_t *out_size,
                                         QwConvertReport *report)
{
    (void)outline;
    (void)outline_count;
    return qw_mupdf_convert_memory(data, size, input_extension, 0, out_pdf,
                                   out_size, report);
}

int qw_mupdf_decode_caj_jp2(const uint8_t *data, size_t size, QwPageImage *out_page,
                            QwConvertReport *report)
{
    (void)data;
    (void)size;
    if (out_page) memset(out_page, 0u, sizeof(*out_page));
    if (report) snprintf(report->note, sizeof(report->note),
                         "CAJ JP2 decoding belongs to the page package; the document backend only renders HTML");
    return QW_STATUS_ERR_UNSUPPORTED;
}
