/* Internal HTML-to-PDF renderer for Quirewise document formats.
 *
 * This module replaces the former external MuPDF backend for the HTML/layout
 * to PDF path. It intentionally implements a small, well-defined subset:
 *
 * - block structure: p, div, headings h1-h6, li, br, table rows/cells;
 * - inline markup is flattened to text;
 * - UTF-8 text is emitted with Helvetica for ASCII and a predefined Adobe-GB1
 *   CID font (STSong-Light) for non-ASCII runs, so no font file is embedded;
 * - deterministic output: no timestamps, no object stream compression.
 *
 * The interface names still carry the historical `qw_mupdf_*` prefix because
 * they are the boundary declared in `mupdf_backend.h`; there is no MuPDF code,
 * header or library involved. */

#include "mupdf_backend.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QW_PDF_MAX_LINES 2000000u
#define QW_PDF_MAX_TEXT_BYTES ((size_t)256u * 1024u * 1024u)

typedef struct QwPdfText {
    char *data;
    size_t size;
    size_t capacity;
} QwPdfText;

typedef struct QwPdfLines {
    QwPdfText *items;
    unsigned char *heading;
    size_t count;
    size_t capacity;
    size_t total_bytes;
} QwPdfLines;

typedef struct QwPdfCursor {
    const unsigned char *data;
    size_t size;
    size_t position;
} QwPdfCursor;

static void pdf_text_free(QwPdfText *text)
{
    if (!text) return;
    free(text->data);
    text->data = NULL;
    text->size = 0u;
    text->capacity = 0u;
}

static int pdf_text_reserve(QwPdfText *text, size_t needed)
{
    size_t capacity;
    char *grown;
    if (needed <= text->capacity) return 1;
    capacity = text->capacity ? text->capacity : 256u;
    while (capacity < needed) {
        if (capacity > QW_PDF_MAX_TEXT_BYTES / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    if (capacity > QW_PDF_MAX_TEXT_BYTES) return 0;
    grown = (char *)realloc(text->data, capacity);
    if (!grown) return 0;
    text->data = grown;
    text->capacity = capacity;
    return 1;
}

static int pdf_text_append(QwPdfText *text, const char *bytes, size_t size)
{
    if (!size) return 1;
    if (!pdf_text_reserve(text, text->size + size + 1u)) return 0;
    memcpy(text->data + text->size, bytes, size);
    text->size += size;
    text->data[text->size] = '\0';
    return 1;
}

static int pdf_text_append_byte(QwPdfText *text, char value)
{
    return pdf_text_append(text, &value, 1u);
}

static void pdf_lines_free(QwPdfLines *lines)
{
    size_t index;
    if (!lines) return;
    for (index = 0; index < lines->count; ++index) {
        pdf_text_free(&lines->items[index]);
    }
    free(lines->items);
    free(lines->heading);
    lines->items = NULL;
    lines->heading = NULL;
    lines->count = 0u;
    lines->capacity = 0u;
    lines->total_bytes = 0u;
}

static int pdf_lines_reserve(QwPdfLines *lines)
{
    size_t capacity;
    QwPdfText *items;
    unsigned char *heading;
    if (lines->count < lines->capacity) return 1;
    if (lines->count >= QW_PDF_MAX_LINES) return 0;
    capacity = lines->capacity ? lines->capacity * 2u : 64u;
    items = (QwPdfText *)realloc(lines->items, capacity * sizeof(*items));
    if (!items) return 0;
    lines->items = items;
    heading = (unsigned char *)realloc(lines->heading, capacity);
    if (!heading) return 0;
    lines->heading = heading;
    lines->capacity = capacity;
    return 1;
}

static int pdf_lines_flush(QwPdfLines *lines, QwPdfText *current,
                           unsigned char level)
{
    if (!current->size) return 1;
    if (lines->total_bytes + current->size > QW_PDF_MAX_TEXT_BYTES) return 0;
    if (!pdf_lines_reserve(lines)) return 0;
    lines->items[lines->count] = *current;
    lines->heading[lines->count] = level;
    lines->total_bytes += current->size;
    lines->count += 1u;
    current->data = NULL;
    current->size = 0u;
    current->capacity = 0u;
    return 1;
}

static int pdf_is_space(unsigned char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
           value == '\f' || value == 0x0bu;
}

static int pdf_tag_is_block(const char *name, size_t length)
{
    static const char *const block_tags[] = {
        "p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "li", "br", "tr",
        "table", "blockquote", "pre", "dt", "dd", "hr", "section", "article",
        "header", "footer", "figcaption", "caption", "ul", "ol"
    };
    size_t index;
    for (index = 0; index < sizeof(block_tags) / sizeof(block_tags[0]); ++index) {
        size_t candidate = strlen(block_tags[index]);
        size_t offset = 0u;
        if (candidate != length) continue;
        while (offset < length) {
            char left = name[offset];
            char right = block_tags[index][offset];
            if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
            if (left != right) break;
            ++offset;
        }
        if (offset == length) return 1;
    }
    return 0;
}

static unsigned char pdf_heading_level(const char *name, size_t length)
{
    if (length == 2u && (name[0] == 'h' || name[0] == 'H') &&
        name[1] >= '1' && name[1] <= '6') {
        return (unsigned char)(name[1] - '0');
    }
    return 0u;
}

static int pdf_append_utf8(QwPdfText *text, unsigned int code)
{
    char buffer[4];
    size_t length;
    if (code < 0x80u) {
        buffer[0] = (char)code;
        length = 1u;
    } else if (code < 0x800u) {
        buffer[0] = (char)(0xC0u | (code >> 6));
        buffer[1] = (char)(0x80u | (code & 0x3Fu));
        length = 2u;
    } else if (code < 0x10000u) {
        buffer[0] = (char)(0xE0u | (code >> 12));
        buffer[1] = (char)(0x80u | ((code >> 6) & 0x3Fu));
        buffer[2] = (char)(0x80u | (code & 0x3Fu));
        length = 3u;
    } else if (code <= 0x10FFFFu) {
        buffer[0] = (char)(0xF0u | (code >> 18));
        buffer[1] = (char)(0x80u | ((code >> 12) & 0x3Fu));
        buffer[2] = (char)(0x80u | ((code >> 6) & 0x3Fu));
        buffer[3] = (char)(0x80u | (code & 0x3Fu));
        length = 4u;
    } else {
        return 0;
    }
    return pdf_text_append(text, buffer, length);
}

static int pdf_append_entity(QwPdfText *text, const char *name, size_t length)
{
    if (length == 3u && memcmp(name, "amp", 3u) == 0) return pdf_text_append_byte(text, '&');
    if (length == 2u && memcmp(name, "lt", 2u) == 0) return pdf_text_append_byte(text, '<');
    if (length == 2u && memcmp(name, "gt", 2u) == 0) return pdf_text_append_byte(text, '>');
    if (length == 4u && memcmp(name, "quot", 4u) == 0) return pdf_text_append_byte(text, '"');
    if (length == 4u && memcmp(name, "apos", 4u) == 0) return pdf_text_append_byte(text, '\'');
    if (length == 4u && memcmp(name, "nbsp", 4u) == 0) return pdf_text_append_byte(text, ' ');
    return pdf_text_append(text, "&", 1u) &&
           pdf_text_append(text, name, length) &&
           pdf_text_append(text, ";", 1u);
}

static int pdf_parse_entity(QwPdfText *text, QwPdfCursor *cursor)
{
    size_t start = cursor->position;
    size_t scan = start;
    while (scan < cursor->size && scan - start < 12u &&
           cursor->data[scan] != ';' && !pdf_is_space(cursor->data[scan])) {
        ++scan;
    }
    if (scan >= cursor->size || cursor->data[scan] != ';') {
        cursor->position = start;
        return pdf_text_append_byte(text, '&');
    }
    {
        const char *name = (const char *)(cursor->data + start);
        size_t length = scan - start;
        cursor->position = scan + 1u;
        if (length >= 2u && name[0] == '#') {
            unsigned int code = 0u;
            size_t index = 1u;
            int hex = 0;
            if (name[1] == 'x' || name[1] == 'X') {
                hex = 1;
                index = 2u;
            }
            if (index >= length) return pdf_text_append_byte(text, '?');
            for (; index < length; ++index) {
                char value = name[index];
                unsigned int digit;
                if (hex) {
                    if (value >= '0' && value <= '9') digit = (unsigned int)(value - '0');
                    else if (value >= 'a' && value <= 'f') digit = (unsigned int)(value - 'a' + 10);
                    else if (value >= 'A' && value <= 'F') digit = (unsigned int)(value - 'A' + 10);
                    else return pdf_text_append_byte(text, '?');
                    code = code * 16u + digit;
                } else {
                    if (value < '0' || value > '9') return pdf_text_append_byte(text, '?');
                    code = code * 10u + (unsigned int)(value - '0');
                }
                if (code > 0x10FFFFu) return pdf_text_append_byte(text, '?');
            }
            return pdf_append_utf8(text, code);
        }
        return pdf_append_entity(text, name, length);
    }
}

static int pdf_skip_until(QwPdfCursor *cursor, const char *needle)
{
    size_t needle_length = strlen(needle);
    while (cursor->position + needle_length <= cursor->size) {
        if (memcmp(cursor->data + cursor->position, needle, needle_length) == 0) {
            cursor->position += needle_length;
            return 1;
        }
        cursor->position += 1u;
    }
    cursor->position = cursor->size;
    return 0;
}

static int pdf_parse_html(const char *html, size_t size, QwPdfLines *lines)
{
    QwPdfCursor cursor;
    QwPdfText current = {NULL, 0u, 0u};
    unsigned char level = 0u;
    unsigned char pending_heading = 0u;
    int space_pending = 0;
    int ok = 1;

    cursor.data = (const unsigned char *)html;
    cursor.size = size;
    cursor.position = 0u;
    while (cursor.position < cursor.size && ok) {
        unsigned char value = cursor.data[cursor.position];
        if (value == '<') {
            if (cursor.position + 4u <= cursor.size &&
                memcmp(cursor.data + cursor.position, "<!--", 4u) == 0) {
                cursor.position += 4u;
                (void)pdf_skip_until(&cursor, "-->");
                continue;
            }
            {
                size_t start = cursor.position + 1u;
                size_t scan = start;
                int closing = 0;
                size_t name_start;
                size_t name_length;
                if (scan < cursor.size && cursor.data[scan] == '/') {
                    closing = 1;
                    ++scan;
                }
                name_start = scan;
                while (scan < cursor.size && cursor.data[scan] != '>' &&
                       !pdf_is_space(cursor.data[scan])) {
                    ++scan;
                }
                name_length = scan - name_start;
                if (scan >= cursor.size) {
                    cursor.position = cursor.size;
                    break;
                }
                while (scan < cursor.size && cursor.data[scan] != '>') ++scan;
                cursor.position = scan < cursor.size ? scan + 1u : cursor.size;
                if (name_length == 5u &&
                    memcmp(cursor.data + name_start, "style", 5u) == 0 && !closing) {
                    (void)pdf_skip_until(&cursor, "</style");
                    while (cursor.position < cursor.size &&
                           cursor.data[cursor.position] != '>') {
                        ++cursor.position;
                    }
                    if (cursor.position < cursor.size) ++cursor.position;
                    continue;
                }
                if (name_length == 6u &&
                    memcmp(cursor.data + name_start, "script", 6u) == 0 && !closing) {
                    (void)pdf_skip_until(&cursor, "</script");
                    while (cursor.position < cursor.size &&
                           cursor.data[cursor.position] != '>') {
                        ++cursor.position;
                    }
                    if (cursor.position < cursor.size) ++cursor.position;
                    continue;
                }
                if (name_length && pdf_tag_is_block((const char *)cursor.data + name_start,
                                                    name_length)) {
                    unsigned char heading = pdf_heading_level(
                        (const char *)cursor.data + name_start, name_length);
                    if (!closing && heading) {
                        pending_heading = heading;
                    }
                    if (!closing && name_length == 2u &&
                        memcmp(cursor.data + name_start, "br", 2u) == 0) {
                        ok = pdf_lines_flush(lines, &current, level);
                        continue;
                    }
                    if (current.size) {
                        level = pending_heading;
                        ok = pdf_lines_flush(lines, &current, level);
                        if (closing || !heading) pending_heading = 0u;
                    } else if (closing && heading) {
                        pending_heading = 0u;
                    }
                }
                continue;
            }
        }
        if (value == '&') {
            cursor.position += 1u;
            ok = pdf_parse_entity(&current, &cursor);
            space_pending = 0;
            continue;
        }
        if (pdf_is_space(value)) {
            ++cursor.position;
            space_pending = 1;
            continue;
        }
        if (space_pending && current.size) {
            ok = ok && pdf_text_append_byte(&current, ' ');
        }
        space_pending = 0;
        {
            size_t start = cursor.position;
            while (cursor.position < cursor.size && cursor.data[cursor.position] != '<' &&
                   cursor.data[cursor.position] != '&' &&
                   !pdf_is_space(cursor.data[cursor.position])) {
                ++cursor.position;
            }
            ok = ok && pdf_text_append(&current,
                                       (const char *)cursor.data + start,
                                       cursor.position - start);
        }
    }
    if (ok) ok = pdf_lines_flush(lines, &current, level);
    pdf_text_free(&current);
    return ok;
}

/* ---- layout ---- */

typedef struct QwPdfGlyphRun {
    int ascii;
    size_t offset;
    size_t length;
} QwPdfGlyphRun;

static size_t pdf_decode_utf8(const unsigned char *data, size_t size,
                              unsigned int *code)
{
    unsigned char first;
    if (!size) return 0u;
    first = data[0];
    if (first < 0x80u) { *code = first; return 1u; }
    if ((first & 0xE0u) == 0xC0u && size >= 2u) {
        *code = ((unsigned int)(first & 0x1Fu) << 6) | (data[1] & 0x3Fu);
        return 2u;
    }
    if ((first & 0xF0u) == 0xE0u && size >= 3u) {
        *code = ((unsigned int)(first & 0x0Fu) << 12) |
                ((unsigned int)(data[1] & 0x3Fu) << 6) | (data[2] & 0x3Fu);
        return 3u;
    }
    if ((first & 0xF8u) == 0xF0u && size >= 4u) {
        *code = ((unsigned int)(first & 0x07u) << 18) |
                ((unsigned int)(data[1] & 0x3Fu) << 12) |
                ((unsigned int)(data[2] & 0x3Fu) << 6) | (data[3] & 0x3Fu);
        return 4u;
    }
    *code = 0xFFFDu;
    return 1u;
}

static double pdf_glyph_width(unsigned int code, double font_size)
{
    if (code < 0x80u) {
        if (code == ' ') return 0.28 * font_size;
        if (code == '.' || code == ',' || code == ':' || code == ';' ||
            code == '!' || code == '\'' || code == '|') return 0.28 * font_size;
        return 0.5 * font_size;
    }
    return 1.0 * font_size;
}

static double pdf_font_size(unsigned char heading)
{
    if (heading == 1u) return 18.0;
    if (heading == 2u) return 15.0;
    if (heading >= 3u) return 13.0;
    return 12.0;
}

static int pdf_collect_runs(const char *text, size_t size,
                            QwPdfGlyphRun **out_runs, size_t *out_count)
{
    QwPdfGlyphRun *runs = NULL;
    size_t count = 0u;
    size_t capacity = 0u;
    size_t index = 0u;
    while (index < size) {
        unsigned int code = 0u;
        size_t length = pdf_decode_utf8((const unsigned char *)text + index,
                                        size - index, &code);
        int ascii = code < 0x80u;
        size_t next = index + length;
        if (!length) break;
        if (!count || runs[count - 1u].ascii != ascii) {
            if (count >= capacity) {
                size_t grown = capacity ? capacity * 2u : 8u;
                QwPdfGlyphRun *resized =
                    (QwPdfGlyphRun *)realloc(runs, grown * sizeof(*resized));
                if (!resized) { free(runs); return 0; }
                runs = resized;
                capacity = grown;
            }
            runs[count].ascii = ascii;
            runs[count].offset = index;
            runs[count].length = 0u;
            count += 1u;
        }
        runs[count - 1u].length += next - index;
        index = next;
    }
    *out_runs = runs;
    *out_count = count;
    return 1;
}

static int pdf_wrap_line(const char *text, size_t size, double max_width,
                         double font_size, size_t **out_breaks, size_t *out_count)
{
    size_t *breaks = NULL;
    size_t count = 0u;
    size_t capacity = 0u;
    size_t index = 0u;
    size_t start = 0u;
    size_t last_space = 0u;
    int have_space = 0;
    double width = 0.0;
    while (index < size) {
        unsigned int code = 0u;
        size_t length = pdf_decode_utf8((const unsigned char *)text + index,
                                        size - index, &code);
        double glyph = pdf_glyph_width(code, font_size);
        if (width + glyph > max_width && index > start) {
            size_t break_at = have_space ? last_space : index;
            if (count >= capacity) {
                size_t grown = capacity ? capacity * 2u : 16u;
                size_t *resized = (size_t *)realloc(breaks, grown * sizeof(*resized));
                if (!resized) { free(breaks); return 0; }
                breaks = resized;
                capacity = grown;
            }
            breaks[count++] = break_at;
            start = break_at + 1u < size ? break_at + 1u : break_at;
            index = start;
            last_space = 0u;
            have_space = 0;
            width = 0.0;
            while (start < size && text[start] == ' ') { ++start; ++index; }
            continue;
        }
        width += glyph;
        if (code == ' ' || code == '\t') {
            last_space = index;
            have_space = 1;
        }
        index += length;
    }
    *out_breaks = breaks;
    *out_count = count;
    return 1;
}

/* ---- PDF writing ---- */

typedef struct QwPdfOut {
    QwPdfText bytes;
    size_t *offsets;
    size_t offset_count;
    size_t offset_capacity;
} QwPdfOut;

static int pdf_out_append(QwPdfOut *out, const char *bytes, size_t size)
{
    return pdf_text_append(&out->bytes, bytes, size);
}

static int pdf_out_printf(QwPdfOut *out, const char *format, ...)
{
    char buffer[512];
    va_list args;
    int written;
    va_start(args, format);
    written = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (written < 0) return 0;
    if ((size_t)written >= sizeof(buffer)) return 0;
    return pdf_out_append(out, buffer, (size_t)written);
}

static int pdf_out_track(QwPdfOut *out, size_t offset)
{
    if (out->offset_count >= out->offset_capacity) {
        size_t grown = out->offset_capacity ? out->offset_capacity * 2u : 32u;
        size_t *resized = (size_t *)realloc(out->offsets, grown * sizeof(*resized));
        if (!resized) return 0;
        out->offsets = resized;
        out->offset_capacity = grown;
    }
    out->offsets[out->offset_count++] = offset;
    return 1;
}

static int pdf_append_hex_ascii(QwPdfOut *out, const char *data, size_t size)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t index;
    for (index = 0; index < size; ++index) {
        unsigned char value = (unsigned char)data[index];
        char pair[2];
        pair[0] = digits[value >> 4];
        pair[1] = digits[value & 0x0Fu];
        if (!pdf_out_append(out, pair, 2u)) return 0;
    }
    return 1;
}

static int pdf_append_hex_utf16(QwPdfOut *out, const char *data, size_t size)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t index = 0u;
    while (index < size) {
        unsigned int code = 0u;
        size_t length = pdf_decode_utf8((const unsigned char *)data + index,
                                        size - index, &code);
        unsigned int units[2];
        size_t unit_count;
        size_t unit_index;
        if (!length) break;
        if (code < 0x10000u) {
            units[0] = code;
            unit_count = 1u;
        } else {
            unsigned int adjusted = code - 0x10000u;
            units[0] = 0xD800u | (adjusted >> 10);
            units[1] = 0xDC00u | (adjusted & 0x3FFu);
            unit_count = 2u;
        }
        for (unit_index = 0; unit_index < unit_count; ++unit_index) {
            char pair[4];
            unsigned int unit = units[unit_index];
            pair[0] = digits[(unit >> 12) & 0x0Fu];
            pair[1] = digits[(unit >> 8) & 0x0Fu];
            pair[2] = digits[(unit >> 4) & 0x0Fu];
            pair[3] = digits[unit & 0x0Fu];
            if (!pdf_out_append(out, pair, 4u)) return 0;
        }
        index += length;
    }
    return 1;
}

static int pdf_write_text_line(QwPdfOut *out, const QwPdfGlyphRun *runs,
                               size_t run_count, const char *text, size_t size,
                               double font_size, double x, double y)
{
    size_t run_index;
    double cursor = x;
    if (!pdf_out_printf(out, "BT\n")) return 0;
    for (run_index = 0; run_index < run_count; ++run_index) {
        const QwPdfGlyphRun *run = &runs[run_index];
        const char *chunk = text + run->offset;
        size_t chunk_index;
        if (!pdf_out_printf(out, "%s %.2f Tf 1 0 0 1 %.2f %.2f Tm <",
                            run->ascii ? "/F1" : "/F2", font_size, cursor, y)) {
            return 0;
        }
        if (run->ascii) {
            if (!pdf_append_hex_ascii(out, chunk, run->length)) return 0;
        } else {
            if (!pdf_append_hex_utf16(out, chunk, run->length)) return 0;
        }
        if (!pdf_out_printf(out, "> Tj\n")) return 0;
        for (chunk_index = 0u; chunk_index < run->length;) {
            unsigned int code = 0u;
            size_t length = pdf_decode_utf8((const unsigned char *)chunk + chunk_index,
                                            run->length - chunk_index, &code);
            if (!length) break;
            cursor += pdf_glyph_width(code, font_size);
            chunk_index += length;
        }
    }
    return pdf_out_printf(out, "ET\n");
}

static int pdf_emit_document(const QwPdfLines *lines, double page_width,
                             double page_height, QwPdfText *output,
                             size_t *out_pages)
{
    const double margin = 56.0;
    const double line_gap = 1.22;
    typedef struct QwPdfSegment {
        size_t line_index;
        size_t start;
        size_t end;
        double font_size;
    } QwPdfSegment;
    QwPdfOut out;
    QwPdfSegment *segments = NULL;
    size_t segment_count = 0u;
    size_t segment_capacity = 0u;
    size_t *page_first = NULL;
    size_t *page_count_lines = NULL;
    size_t page_capacity_total = 0u;
    size_t page_count = 0u;
    size_t page_line_capacity;
    size_t line_index;
    int ok = 1;

    memset(&out, 0, sizeof(out));
    page_line_capacity = (size_t)((page_height - 2.0 * margin) / (12.0 * line_gap));
    if (page_line_capacity < 8u) page_line_capacity = 8u;

    /* Pass 1: wrap every logical line into layout segments. */
    for (line_index = 0u; ok && line_index < lines->count; ++line_index) {
        const QwPdfText *item = &lines->items[line_index];
        double font_size = pdf_font_size(lines->heading[line_index]);
        size_t *breaks = NULL;
        size_t break_count = 0u;
        size_t segment_start = 0u;
        size_t break_index;
        if (!pdf_wrap_line(item->data ? item->data : "", item->size,
                           page_width - 2.0 * margin, font_size,
                           &breaks, &break_count)) {
            ok = 0;
            break;
        }
        for (break_index = 0u; ok && break_index <= break_count; ++break_index) {
            size_t segment_end = break_index < break_count
                ? breaks[break_index] : item->size;
            if (segment_count >= segment_capacity) {
                size_t grown = segment_capacity ? segment_capacity * 2u : 64u;
                QwPdfSegment *resized =
                    (QwPdfSegment *)realloc(segments, grown * sizeof(*resized));
                if (!resized) { ok = 0; break; }
                segments = resized;
                segment_capacity = grown;
            }
            segments[segment_count].line_index = line_index;
            segments[segment_count].start = segment_start;
            segments[segment_count].end = segment_end;
            segments[segment_count].font_size = font_size;
            segment_count += 1u;
            segment_start = segment_end < item->size ? segment_end + 1u : item->size;
            if (segment_start > item->size) segment_start = item->size;
        }
        free(breaks);
    }

    /* Pass 2: pack segments into pages. */
    for (line_index = 0u; ok && line_index < segment_count;) {
        size_t remaining = segment_count - line_index;
        size_t take = remaining < page_line_capacity ? remaining : page_line_capacity;
        if (page_count >= page_capacity_total) {
            size_t grown = page_capacity_total ? page_capacity_total * 2u : 16u;
            size_t *first = (size_t *)realloc(page_first, grown * sizeof(*first));
            size_t *counts;
            if (!first) { ok = 0; break; }
            page_first = first;
            counts = (size_t *)realloc(page_count_lines, grown * sizeof(*counts));
            if (!counts) { ok = 0; break; }
            page_count_lines = counts;
            page_capacity_total = grown;
        }
        page_first[page_count] = line_index;
        page_count_lines[page_count] = take;
        page_count += 1u;
        line_index += take;
    }

    if (!ok) {
        free(segments);
        free(page_first);
        free(page_count_lines);
        return 0;
    }

    {
        size_t font_ascii = 3u;
        size_t font_cjk = 4u;
        size_t font_descendant = 5u;
        size_t font_descriptor = 6u;
        size_t pages_object = 2u;
        size_t first_page_object = 7u;
        size_t object;

        ok = pdf_out_printf(&out, "%%PDF-1.4\n%%\xE2\xE3\xCF\xD3\n");
        if (ok) ok = pdf_out_track(&out, out.bytes.size);
        if (ok) ok = pdf_out_printf(&out, "1 0 obj\n<< /Type /Catalog /Pages %zu 0 R >>\nendobj\n",
                                    pages_object);
        if (ok) ok = pdf_out_track(&out, out.bytes.size);
        if (ok) {
            size_t kid;
            ok = pdf_out_printf(&out, "2 0 obj\n<< /Type /Pages /Count %zu /Kids [",
                                page_count ? page_count : 1u);
            for (kid = 0u; ok && kid < page_count; ++kid) {
                ok = pdf_out_printf(&out, "%zu 0 R ", first_page_object + kid * 2u);
            }
            if (ok) ok = pdf_out_printf(&out, "] >>\nendobj\n");
        }
        if (ok) ok = pdf_out_track(&out, out.bytes.size);
        if (ok) ok = pdf_out_printf(&out,
            "3 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
            "/Encoding /WinAnsiEncoding >>\nendobj\n");
        if (ok) ok = pdf_out_track(&out, out.bytes.size);
        if (ok) ok = pdf_out_printf(&out,
            "4 0 obj\n<< /Type /Font /Subtype /Type0 /BaseFont /STSong-Light "
            "/Encoding /UniGB-UCS2-H /DescendantFonts [%zu 0 R] >>\nendobj\n",
            font_descendant);
        if (ok) ok = pdf_out_track(&out, out.bytes.size);
        if (ok) ok = pdf_out_printf(&out,
            "5 0 obj\n<< /Type /Font /Subtype /CIDFontType0 /BaseFont /STSong-Light "
            "/CIDSystemInfo << /Registry (Adobe) /Ordering (GB1) /Supplement 4 >> "
            "/FontDescriptor %zu 0 R /DW 1000 >>\nendobj\n", font_descriptor);
        if (ok) ok = pdf_out_track(&out, out.bytes.size);
        if (ok) ok = pdf_out_printf(&out,
            "6 0 obj\n<< /Type /FontDescriptor /FontName /STSong-Light /Flags 6 "
            "/FontBBox [-25 -254 1000 880] /ItalicAngle 0 /Ascent 880 /Descent -120 "
            "/CapHeight 880 /StemV 80 >>\nendobj\n");

        for (object = 0u; ok && object < page_count; ++object) {
            QwPdfOut stream;
            size_t first = page_first[object];
            size_t stop = first + page_count_lines[object];
            double y = page_height - margin - 12.0;
            size_t segment_index;
            memset(&stream, 0, sizeof(stream));
            for (segment_index = first; ok && segment_index < stop; ++segment_index) {
                const QwPdfSegment *segment = &segments[segment_index];
                const QwPdfText *item = &lines->items[segment->line_index];
                const char *text = (item->data ? item->data : "") + segment->start;
                size_t size = segment->end > segment->start
                    ? segment->end - segment->start : 0u;
                QwPdfGlyphRun *runs = NULL;
                size_t run_count = 0u;
                if (!pdf_collect_runs(text, size, &runs, &run_count)) { ok = 0; break; }
                if (!pdf_write_text_line(&stream, runs, run_count, text, size,
                                         segment->font_size, margin, y)) {
                    free(runs);
                    ok = 0;
                    break;
                }
                free(runs);
                y -= segment->font_size * line_gap;
            }
            if (!ok) { free(stream.bytes.data); free(stream.offsets); break; }
            {
                size_t page_object = first_page_object + object * 2u;
                size_t content_object = page_object + 1u;
                if (!pdf_out_track(&out, out.bytes.size) ||
                    !pdf_out_printf(&out,
                        "%zu 0 obj\n<< /Type /Page /Parent %zu 0 R /MediaBox [0 0 %.2f %.2f] "
                        "/Resources << /Font << /F1 %zu 0 R /F2 %zu 0 R >> >> "
                        "/Contents %zu 0 R >>\nendobj\n",
                        page_object, pages_object, page_width, page_height,
                        font_ascii, font_cjk, content_object) ||
                    !pdf_out_track(&out, out.bytes.size) ||
                    !pdf_out_printf(&out, "%zu 0 obj\n<< /Length %zu >>\nstream\n",
                                    content_object, stream.bytes.size) ||
                    (stream.bytes.size &&
                     !pdf_out_append(&out, stream.bytes.data, stream.bytes.size)) ||
                    !pdf_out_printf(&out, "\nendstream\nendobj\n")) {
                    ok = 0;
                }
            }
            free(stream.bytes.data);
            free(stream.offsets);
        }
        if (ok && page_count == 0u) {
            size_t page_object = first_page_object;
            size_t content_object = first_page_object + 1u;
            if (!pdf_out_track(&out, out.bytes.size) ||
                !pdf_out_printf(&out,
                    "%zu 0 obj\n<< /Type /Page /Parent %zu 0 R /MediaBox [0 0 %.2f %.2f] "
                    "/Resources << /Font << /F1 %zu 0 R /F2 %zu 0 R >> >> "
                    "/Contents %zu 0 R >>\nendobj\n",
                    page_object, pages_object, page_width, page_height,
                    font_ascii, font_cjk, content_object) ||
                !pdf_out_track(&out, out.bytes.size) ||
                !pdf_out_printf(&out, "%zu 0 obj\n<< /Length 0 >>\nstream\n\nendstream\nendobj\n",
                                content_object)) {
                ok = 0;
            }
        }
    }

    free(segments);
    free(page_first);
    free(page_count_lines);
    if (!ok) {
        free(out.bytes.data);
        free(out.offsets);
        return 0;
    }
    {
        size_t xref_offset = out.bytes.size;
        size_t entry;
        size_t object_total = 6u + (page_count ? page_count : 1u) * 2u;
        if (!pdf_out_printf(&out, "xref\n0 %zu\n", object_total + 1u) ||
            !pdf_out_printf(&out, "0000000000 65535 f \n")) {
            ok = 0;
        }
        for (entry = 0u; ok && entry < object_total; ++entry) {
            unsigned long long offset = entry < out.offset_count ? out.offsets[entry] : 0u;
            ok = pdf_out_printf(&out, "%010llu 00000 n \n", offset);
        }
        if (ok) {
            ok = pdf_out_printf(&out,
                "trailer\n<< /Size %zu /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n",
                object_total + 1u, xref_offset);
        }
    }
    if (!ok) {
        free(out.bytes.data);
        free(out.offsets);
        return 0;
    }
    free(out.offsets);
    *output = out.bytes;
    if (out_pages) *out_pages = page_count ? page_count : 1u;
    return 1;
}

static int pdf_render_html(const char *html, size_t html_size,
                           double page_width, double page_height,
                           QwPdfText *output, size_t *out_pages,
                           QwConvertReport *report)
{
    QwPdfLines lines;
    size_t pages = 0u;
    if (!html || !output) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (html_size > QW_PDF_MAX_TEXT_BYTES) return QW_STATUS_ERR_RESOURCE_LIMIT;
    if (page_width < 72.0 || page_width > 5000.0 ||
        page_height < 72.0 || page_height > 5000.0) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    memset(&lines, 0, sizeof(lines));
    if (!pdf_parse_html(html, html_size, &lines)) {
        pdf_lines_free(&lines);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    if (!pdf_emit_document(&lines, page_width, page_height, output, &pages)) {
        pdf_lines_free(&lines);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "Rendered %zu layout line(s) into %zu PDF page(s) with the internal Quirewise HTML/PDF writer",
                 lines.count, pages);
    }
    pdf_lines_free(&lines);
    if (out_pages) *out_pages = pages;
    return QW_STATUS_OK;
}

int qw_mupdf_convert_html_with_page_size(const char *html, size_t html_size,
                                         float page_width_points,
                                         float page_height_points,
                                         uint8_t **out_pdf, size_t *out_size,
                                         QwConvertReport *report)
{
    QwPdfText output;
    int rc;
    if (!out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    memset(&output, 0, sizeof(output));
    rc = pdf_render_html(html, html_size,
                         page_width_points > 0.0f ? page_width_points : 595.0,
                         page_height_points > 0.0f ? page_height_points : 842.0,
                         &output, NULL, report);
    if (rc != QW_STATUS_OK) return rc;
    *out_pdf = (uint8_t *)output.data;
    *out_size = output.size;
    return QW_STATUS_OK;
}

int qw_mupdf_convert_html(const char *html, size_t html_size,
                          uint8_t **out_pdf, size_t *out_size,
                          QwConvertReport *report)
{
    return qw_mupdf_convert_html_with_page_size(html, html_size, 595.0f, 842.0f,
                                                out_pdf, out_size, report);
}

int qw_mupdf_convert_html_to_stream(const char *html, size_t html_size,
                                    FILE *stream, size_t max_output_bytes,
                                    size_t *out_size, QwConvertReport *report)
{
    return qw_mupdf_convert_html_with_page_size_to_stream(
        html, html_size, 595.0f, 842.0f, stream, max_output_bytes, out_size, report);
}

int qw_mupdf_convert_html_with_page_size_to_stream(
    const char *html, size_t html_size, float page_width_points,
    float page_height_points, FILE *stream, size_t max_output_bytes,
    size_t *out_size, QwConvertReport *report)
{
    QwPdfText output;
    int rc;
    if (!stream) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (out_size) *out_size = 0u;
    memset(&output, 0, sizeof(output));
    rc = pdf_render_html(html, html_size,
                         page_width_points > 0.0f ? page_width_points : 595.0,
                         page_height_points > 0.0f ? page_height_points : 842.0,
                         &output, NULL, report);
    if (rc != QW_STATUS_OK) return rc;
    if (max_output_bytes && output.size > max_output_bytes) {
        pdf_text_free(&output);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (output.size &&
        fwrite(output.data, 1u, output.size, stream) != output.size) {
        pdf_text_free(&output);
        return QW_STATUS_ERR_IO;
    }
    if (out_size) *out_size = output.size;
    pdf_text_free(&output);
    return QW_STATUS_OK;
}

int qw_mupdf_convert_html_file(const char *path, uint8_t **out_pdf,
                               size_t *out_size, QwConvertReport *report)
{
    FILE *file;
    long length;
    char *buffer;
    size_t read_size;
    int rc;
    if (!path || !out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    file = fopen(path, "rb");
    if (!file) return QW_STATUS_ERR_IO;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return QW_STATUS_ERR_IO;
    }
    if ((unsigned long)length > QW_PDF_MAX_TEXT_BYTES) {
        fclose(file);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    buffer = (char *)malloc((size_t)length + 1u);
    if (!buffer) {
        fclose(file);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    read_size = fread(buffer, 1u, (size_t)length, file);
    fclose(file);
    if (read_size != (size_t)length) {
        free(buffer);
        return QW_STATUS_ERR_IO;
    }
    rc = qw_mupdf_convert_html(buffer, read_size, out_pdf, out_size, report);
    free(buffer);
    return rc;
}

int qw_mupdf_convert_html_file_to_stream(const char *path, FILE *stream,
                                         size_t max_output_bytes,
                                         size_t *out_size,
                                         QwConvertReport *report)
{
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    int rc;
    if (!path || !stream) return QW_STATUS_ERR_INVALID_ARGUMENT;
    rc = qw_mupdf_convert_html_file(path, &pdf, &pdf_size, report);
    if (rc != QW_STATUS_OK) return rc;
    if (max_output_bytes && pdf_size > max_output_bytes) {
        free(pdf);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (pdf_size && fwrite(pdf, 1u, pdf_size, stream) != pdf_size) {
        free(pdf);
        return QW_STATUS_ERR_IO;
    }
    if (out_size) *out_size = pdf_size;
    free(pdf);
    return QW_STATUS_OK;
}
