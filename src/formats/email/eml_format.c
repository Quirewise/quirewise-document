#include "eml_format.h"

#include "layout_document.h"
#include "text_buffer.h"
#ifndef QUIREWISE_STANDALONE_EMAIL_PARSER
#include "writer_registry.h"
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    EML_MAX_INPUT_BYTES = 64 * 1024 * 1024,
    EML_MAX_HEADER_BYTES = 256 * 1024,
    EML_MAX_HEADER_VALUE = 4096,
    EML_MAX_BOUNDARY = 256,
    EML_MAX_PARTS = 512,
    EML_MAX_DEPTH = 16,
    EML_MAX_ATTACHMENTS = 128,
    EML_MAX_ATTACHMENT_BYTES = 64 * 1024 * 1024,
    EML_MAX_TEXT_BYTES = 16 * 1024 * 1024,
    EML_MAX_HTML_BYTES = 20 * 1024 * 1024,
    EML_MAX_OUTPUT_BYTES = 128 * 1024 * 1024
};

typedef struct EmlHeaders {
    char from[EML_MAX_HEADER_VALUE];
    char to[EML_MAX_HEADER_VALUE];
    char cc[EML_MAX_HEADER_VALUE];
    char reply_to[EML_MAX_HEADER_VALUE];
    char date[EML_MAX_HEADER_VALUE];
    char subject[EML_MAX_HEADER_VALUE];
    char message_id[EML_MAX_HEADER_VALUE];
    char content_type[EML_MAX_HEADER_VALUE];
    char transfer_encoding[128];
    char disposition[EML_MAX_HEADER_VALUE];
    char content_id[1024];
} EmlHeaders;

typedef struct EmlOwner {
    char *html;
    QwTextBuffer body;
    QwAttachment attachments[EML_MAX_ATTACHMENTS];
    size_t attachment_count;
    size_t attachment_bytes;
    size_t part_count;
    int body_priority;
    int has_body;
    int had_unsupported_part;
    int had_charset_fallback;
    int had_encoded_header;
    int had_resource_skip;
    int had_malformed_header;
    int fatal_error;
    QwAttachmentSet attachment_set;
} EmlOwner;

static int eml_ascii_equal(const char *left, const char *right)
{
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
            return 0;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static int eml_ascii_contains(const char *text, const char *needle)
{
    size_t needle_size = strlen(needle);
    const char *cursor;
    if (!needle_size) return 1;
    for (cursor = text; *cursor; ++cursor) {
        size_t i;
        for (i = 0u; i < needle_size && cursor[i]; ++i) {
            if (tolower((unsigned char)cursor[i]) !=
                tolower((unsigned char)needle[i])) break;
        }
        if (i == needle_size) return 1;
    }
    return 0;
}

static int eml_span_equal(const uint8_t *data, size_t size, const char *text)
{
    size_t i;
    if (strlen(text) != size) return 0;
    for (i = 0u; i < size; ++i) {
        if (tolower((unsigned char)data[i]) !=
            tolower((unsigned char)text[i])) return 0;
    }
    return 1;
}

static size_t eml_trim_left(const uint8_t *data, size_t size)
{
    size_t offset = 0u;
    while (offset < size && (data[offset] == ' ' || data[offset] == '\t'))
        ++offset;
    return offset;
}

static size_t eml_trim_right(const uint8_t *data, size_t size)
{
    while (size && (data[size - 1u] == ' ' || data[size - 1u] == '\t'))
        --size;
    return size;
}

static int eml_append_literal(QwTextBuffer *buffer, const char *text)
{
    return qw_text_buffer_append(buffer, text, strlen(text));
}

static int eml_append_codepoint(QwTextBuffer *buffer, unsigned long cp)
{
    char encoded[4];
    size_t size;
    if (cp <= 0x7fu) {
        encoded[0] = (char)cp;
        size = 1u;
    } else if (cp <= 0x7ffu) {
        encoded[0] = (char)(0xc0u | (cp >> 6));
        encoded[1] = (char)(0x80u | (cp & 0x3fu));
        size = 2u;
    } else if (cp <= 0xffffu && !(cp >= 0xd800u && cp <= 0xdfffu)) {
        encoded[0] = (char)(0xe0u | (cp >> 12));
        encoded[1] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        encoded[2] = (char)(0x80u | (cp & 0x3fu));
        size = 3u;
    } else if (cp <= 0x10ffffu) {
        encoded[0] = (char)(0xf0u | (cp >> 18));
        encoded[1] = (char)(0x80u | ((cp >> 12) & 0x3fu));
        encoded[2] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        encoded[3] = (char)(0x80u | (cp & 0x3fu));
        size = 4u;
    } else {
        return eml_append_literal(buffer, "\xef\xbf\xbd");
    }
    return qw_text_buffer_append(buffer, encoded, size);
}

static int eml_append_html_escaped(QwTextBuffer *buffer,
                                   const char *text,
                                   size_t size)
{
    size_t i;
    for (i = 0u; i < size; ++i) {
        const char *replacement = NULL;
        switch ((unsigned char)text[i]) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        default: break;
        }
        if (replacement) {
            if (!eml_append_literal(buffer, replacement)) return 0;
        } else if (!qw_text_buffer_append(buffer, text + i, 1u)) {
            return 0;
        }
    }
    return 1;
}

static int eml_header_separator(const uint8_t *data,
                                size_t size,
                                size_t *out_body_offset,
                                size_t max_header_bytes)
{
    size_t line_start = 0u;
    if (!data || !out_body_offset) return 0;
    while (line_start < size && line_start <= max_header_bytes) {
        size_t line_end = line_start;
        size_t next;
        while (line_end < size && data[line_end] != '\n') ++line_end;
        if (line_end > max_header_bytes) return 0;
        next = line_end < size ? line_end + 1u : line_end;
        if (line_end > line_start && data[line_end - 1u] == '\r')
            --line_end;
        if (line_end == line_start) {
            *out_body_offset = next;
            return 1;
        }
        if (next == line_end) return 0;
        line_start = next;
    }
    return 0;
}

static char *eml_header_target(EmlHeaders *headers,
                              const uint8_t *name,
                              size_t name_size,
                              size_t *out_capacity)
{
#define EML_HEADER_MATCH(label, member) \
    if (eml_span_equal(name, name_size, label)) { \
        *out_capacity = sizeof(headers->member); return headers->member; \
    }
    EML_HEADER_MATCH("from", from)
    EML_HEADER_MATCH("to", to)
    EML_HEADER_MATCH("cc", cc)
    EML_HEADER_MATCH("reply-to", reply_to)
    EML_HEADER_MATCH("date", date)
    EML_HEADER_MATCH("subject", subject)
    EML_HEADER_MATCH("message-id", message_id)
    EML_HEADER_MATCH("content-type", content_type)
    EML_HEADER_MATCH("content-transfer-encoding", transfer_encoding)
    EML_HEADER_MATCH("content-disposition", disposition)
    EML_HEADER_MATCH("content-id", content_id)
#undef EML_HEADER_MATCH
    return NULL;
}

static int eml_append_header_value(EmlOwner *owner,
                                  char *target,
                                  size_t capacity,
                                  const uint8_t *value,
                                  size_t value_size,
                                  int continuation)
{
    size_t current = strlen(target);
    size_t offset = eml_trim_left(value, value_size);
    size_t trimmed = eml_trim_right(value + offset, value_size - offset);
    size_t needed = trimmed + (continuation && current ? 1u : 0u);
    if (needed >= capacity - current) {
        owner->had_malformed_header = 1;
        if (capacity > current + 1u) {
            size_t copy = capacity - current - 1u;
            if (continuation && current && copy) {
                target[current++] = ' ';
                --copy;
            }
            if (copy) memcpy(target + current, value + offset,
                             copy < trimmed ? copy : trimmed);
            target[capacity - 1u] = '\0';
        }
        return 1;
    }
    if (continuation && current) target[current++] = ' ';
    if (trimmed) memcpy(target + current, value + offset, trimmed);
    target[current + trimmed] = '\0';
    return 1;
}

static int eml_parse_headers(EmlOwner *owner,
                             const uint8_t *data,
                             size_t size,
                             EmlHeaders *headers,
                             size_t *out_body_offset)
{
    size_t line_start = 0u;
    char *current_target = NULL;
    size_t current_capacity = 0u;
    if (!eml_header_separator(data, size, out_body_offset,
                              EML_MAX_HEADER_BYTES)) return 0;
    memset(headers, 0, sizeof(*headers));
    while (line_start < *out_body_offset) {
        size_t line_end = line_start;
        size_t line_size;
        size_t next;
        while (line_end < *out_body_offset && data[line_end] != '\n') ++line_end;
        next = line_end < *out_body_offset ? line_end + 1u : line_end;
        if (line_end > line_start && data[line_end - 1u] == '\r') --line_end;
        line_size = line_end - line_start;
        if (!line_size) break;
        if (data[line_start] == ' ' || data[line_start] == '\t') {
            if (current_target &&
                !eml_append_header_value(owner, current_target,
                                         current_capacity, data + line_start,
                                         line_size, 1)) return 0;
        } else {
            size_t colon = 0u;
            while (colon < line_size && data[line_start + colon] != ':') ++colon;
            current_target = NULL;
            current_capacity = 0u;
            if (colon && colon < line_size) {
                current_target = eml_header_target(
                    headers, data + line_start, colon, &current_capacity);
                if (current_target) {
                    if (current_target[0]) owner->had_malformed_header = 1;
                    else if (!eml_append_header_value(
                                 owner, current_target, current_capacity,
                                 data + line_start + colon + 1u,
                                 line_size - colon - 1u, 0)) return 0;
                }
            } else {
                owner->had_malformed_header = 1;
            }
        }
        line_start = next;
    }
    return 1;
}

static int eml_parameter(const char *value,
                         const char *wanted,
                         char *out,
                         size_t out_capacity,
                         int *out_extended)
{
    const char *cursor = strchr(value, ';');
    if (out_capacity) out[0] = '\0';
    if (out_extended) *out_extended = 0;
    while (cursor && *cursor) {
        const char *name;
        const char *name_end;
        const char *value_start;
        const char *value_end;
        size_t name_size;
        size_t value_size;
        size_t copied;
        ++cursor;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ';') ++cursor;
        name = cursor;
        while (*cursor && *cursor != '=' && *cursor != ';') ++cursor;
        name_end = cursor;
        while (name_end > name &&
               (name_end[-1] == ' ' || name_end[-1] == '\t')) --name_end;
        if (*cursor != '=') {
            cursor = strchr(cursor, ';');
            continue;
        }
        ++cursor;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        value_start = cursor;
        if (*cursor == '"') {
            value_start = ++cursor;
            while (*cursor && (*cursor != '"' || cursor[-1] == '\\')) ++cursor;
            value_end = cursor;
            if (*cursor == '"') ++cursor;
            while (*cursor && *cursor != ';') ++cursor;
        } else {
            while (*cursor && *cursor != ';') ++cursor;
            value_end = cursor;
            while (value_end > value_start &&
                   (value_end[-1] == ' ' || value_end[-1] == '\t')) --value_end;
        }
        name_size = (size_t)(name_end - name);
        value_size = (size_t)(value_end - value_start);
        if (name_size == strlen(wanted) &&
            eml_span_equal((const uint8_t *)name, name_size, wanted)) {
            copied = value_size < out_capacity - 1u
                ? value_size : out_capacity - 1u;
            if (copied) memcpy(out, value_start, copied);
            out[copied] = '\0';
            if (out_extended && value_size >= copied &&
                (name_end > name && name_end[-1] == '*')) *out_extended = 1;
            return value_size < out_capacity;
        }
        if (out_extended && name_size > strlen(wanted) &&
            name_size >= strlen(wanted) + 1u && name_end[-1] == '*' &&
            eml_span_equal((const uint8_t *)name, name_size - 1u, wanted))
            *out_extended = 1;
    }
    return 0;
}

static int eml_hex(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return (int)(ch - '0');
    ch = (unsigned char)tolower(ch);
    if (ch >= 'a' && ch <= 'f') return (int)(ch - 'a') + 10;
    return -1;
}

static int eml_base64_value(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z') return (int)(ch - 'A');
    if (ch >= 'a' && ch <= 'z') return (int)(ch - 'a') + 26;
    if (ch >= '0' && ch <= '9') return (int)(ch - '0') + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static int eml_decode_transfer(const uint8_t *data,
                               size_t size,
                               const char *encoding,
                               uint8_t **out_data,
                               size_t *out_size)
{
    uint8_t *decoded;
    size_t output_size = 0u;
    size_t i;
    if (!out_data || !out_size || (!data && size)) return 0;
    *out_data = NULL;
    *out_size = 0u;
    decoded = (uint8_t *)malloc(size ? size : 1u);
    if (!decoded) return -1;
    if (eml_ascii_equal(encoding, "base64")) {
        int quartet[4];
        size_t quartet_size = 0u;
        int saw_padding = 0;
        for (i = 0u; i < size; ++i) {
            unsigned char ch = data[i];
            int value;
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
            if (saw_padding) { free(decoded); return 0; }
            if (ch == '=') value = -2;
            else value = eml_base64_value(ch);
            if (value < 0 && value != -2) { free(decoded); return 0; }
            quartet[quartet_size++] = value;
            if (quartet_size == 4u) {
                if (quartet[0] < 0 || quartet[1] < 0 ||
                    (quartet[2] == -2 && quartet[3] != -2) ||
                    quartet[2] < -2 || quartet[3] < -2) {
                    free(decoded); return 0;
                }
                decoded[output_size++] = (uint8_t)((quartet[0] << 2) |
                                                    (quartet[1] >> 4));
                if (quartet[2] != -2) {
                    decoded[output_size++] = (uint8_t)((quartet[1] << 4) |
                                                        (quartet[2] >> 2));
                    if (quartet[3] != -2) {
                        decoded[output_size++] = (uint8_t)((quartet[2] << 6) |
                                                            quartet[3]);
                    } else {
                        saw_padding = 1;
                    }
                } else {
                    saw_padding = 1;
                }
                quartet_size = 0u;
            }
        }
        if (quartet_size) {
            free(decoded);
            return 0;
        }
    } else if (eml_ascii_equal(encoding, "quoted-printable")) {
        for (i = 0u; i < size; ++i) {
            if (data[i] != '=') {
                decoded[output_size++] = data[i];
            } else if (i + 1u < size && data[i + 1u] == '\n') {
                ++i;
            } else if (i + 2u < size && data[i + 1u] == '\r' &&
                       data[i + 2u] == '\n') {
                i += 2u;
            } else if (i + 2u < size && eml_hex(data[i + 1u]) >= 0 &&
                       eml_hex(data[i + 2u]) >= 0) {
                decoded[output_size++] = (uint8_t)(
                    (eml_hex(data[i + 1u]) << 4) | eml_hex(data[i + 2u]));
                i += 2u;
            } else {
                decoded[output_size++] = '=';
            }
        }
    } else if (!encoding[0] || eml_ascii_equal(encoding, "7bit") ||
               eml_ascii_equal(encoding, "8bit") ||
               eml_ascii_equal(encoding, "binary")) {
        if (size) memcpy(decoded, data, size);
        output_size = size;
    } else {
        free(decoded);
        return 0;
    }
    *out_data = decoded;
    *out_size = output_size;
    return 1;
}

static unsigned long eml_read_u16(const uint8_t *data, int big_endian)
{
    return big_endian ? ((unsigned long)data[0] << 8) | data[1]
                      : ((unsigned long)data[1] << 8) | data[0];
}

static unsigned long eml_cp1252(unsigned char value)
{
    static const unsigned short map[32] = {
        0x20acu, 0x0081u, 0x201au, 0x0192u, 0x201eu, 0x2026u, 0x2020u, 0x2021u,
        0x02c6u, 0x2030u, 0x0160u, 0x2039u, 0x0152u, 0x008du, 0x017du, 0x008fu,
        0x0090u, 0x2018u, 0x2019u, 0x201cu, 0x201du, 0x2022u, 0x2013u, 0x2014u,
        0x02dcu, 0x2122u, 0x0161u, 0x203au, 0x0153u, 0x009du, 0x017eu, 0x0178u
    };
    return value >= 0x80u && value <= 0x9fu ? map[value - 0x80u] : value;
}

static int eml_append_decoded_text(EmlOwner *owner,
                                   QwTextBuffer *buffer,
                                   const uint8_t *data,
                                   size_t size,
                                   const char *charset)
{
    size_t i = 0u;
    int latin1 = eml_ascii_equal(charset, "iso-8859-1") ||
                 eml_ascii_equal(charset, "latin1") ||
                 eml_ascii_equal(charset, "latin-1");
    int cp1252 = eml_ascii_equal(charset, "windows-1252") ||
                 eml_ascii_equal(charset, "cp1252");
    int utf16 = eml_ascii_equal(charset, "utf-16") ||
                eml_ascii_equal(charset, "utf-16le") ||
                eml_ascii_equal(charset, "utf-16be") ||
                (size >= 2u && ((data[0] == 0xffu && data[1] == 0xfeu) ||
                                (data[0] == 0xfeu && data[1] == 0xffu)));
    int big_endian = eml_ascii_equal(charset, "utf-16be");
    if (!charset[0])
        owner->had_charset_fallback = 1;
    if (charset[0] && !latin1 && !cp1252 && !utf16 &&
        !eml_ascii_equal(charset, "utf-8") &&
        !eml_ascii_equal(charset, "utf8") &&
        !eml_ascii_equal(charset, "us-ascii") &&
        !eml_ascii_equal(charset, "ascii"))
        owner->had_charset_fallback = 1;
    if (utf16) {
        if (size >= 2u && data[0] == 0xffu && data[1] == 0xfeu) {
            big_endian = 0;
            i = 2u;
        } else if (size >= 2u && data[0] == 0xfeu && data[1] == 0xffu) {
            big_endian = 1;
            i = 2u;
        }
        while (i + 1u < size) {
            unsigned long cp = eml_read_u16(data + i, big_endian);
            i += 2u;
            if (cp >= 0xd800u && cp <= 0xdbffu) {
                if (i + 1u < size) {
                    unsigned long low = eml_read_u16(data + i, big_endian);
                    if (low >= 0xdc00u && low <= 0xdfffu) {
                        cp = 0x10000u + ((cp - 0xd800u) << 10) + low - 0xdc00u;
                        i += 2u;
                    } else {
                        cp = 0xfffdu;
                        owner->had_charset_fallback = 1;
                    }
                } else {
                    cp = 0xfffdu;
                    owner->had_charset_fallback = 1;
                }
            } else if (cp >= 0xdc00u && cp <= 0xdfffu) {
                cp = 0xfffdu;
                owner->had_charset_fallback = 1;
            }
            if ((cp < 0x20u && cp != '\n' && cp != '\r' && cp != '\t') ||
                cp == 0x7fu) cp = ' ';
            if (!eml_append_codepoint(buffer, cp)) goto out_of_memory;
        }
        if (i < size) {
            owner->had_charset_fallback = 1;
            if (!eml_append_literal(buffer, "\xef\xbf\xbd")) goto out_of_memory;
        }
        return 1;
    }
    while (i < size) {
        unsigned long cp;
        unsigned char first = data[i];
        if (latin1 || cp1252) {
            cp = cp1252 ? eml_cp1252(first) : first;
            ++i;
        } else if (first < 0x80u) {
            cp = first;
            ++i;
        } else {
            size_t count;
            if (first >= 0xc2u && first <= 0xdfu) count = 2u;
            else if (first >= 0xe0u && first <= 0xefu) count = 3u;
            else if (first >= 0xf0u && first <= 0xf4u) count = 4u;
            else count = 0u;
            if (!count || count > size - i) {
                cp = 0xfffdu;
                ++i;
                owner->had_charset_fallback = 1;
            } else {
                size_t j;
                cp = first & (count == 2u ? 0x1fu :
                              count == 3u ? 0x0fu : 0x07u);
                for (j = 1u; j < count; ++j) {
                    if ((data[i + j] & 0xc0u) != 0x80u) break;
                    cp = (cp << 6) | (data[i + j] & 0x3fu);
                }
                if (j != count || (count == 2u && cp < 0x80u) ||
                    (count == 3u && cp < 0x800u) ||
                    (count == 4u && cp < 0x10000u) ||
                    cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) {
                    cp = 0xfffdu;
                    ++i;
                    owner->had_charset_fallback = 1;
                } else {
                    i += count;
                }
            }
        }
        if ((cp < 0x20u && cp != '\n' && cp != '\r' && cp != '\t') ||
            cp == 0x7fu) cp = ' ';
        if (!eml_append_codepoint(buffer, cp)) goto out_of_memory;
    }
    return 1;

out_of_memory:
    owner->fatal_error = QW_STATUS_ERR_OUT_OF_MEMORY;
    return 0;
}

static int eml_append_decoded_header(EmlOwner *owner,
                                     QwTextBuffer *buffer,
                                     const char *header)
{
    size_t size = strlen(header);
    size_t cursor = 0u;
    while (cursor < size) {
        if (cursor + 2u < size && header[cursor] == '=' &&
            header[cursor + 1u] == '?') {
            const char *charset_start = header + cursor + 2u;
            const char *charset_end = strchr(charset_start, '?');
            const char *encoding_start;
            const char *encoding_end;
            const char *payload_start;
            const char *end_word;
            char charset[128];
            char encoding[16];
            uint8_t *decoded = NULL;
            size_t decoded_size = 0u;
            int decode_rc;
            size_t charset_size;
            size_t encoding_size;
            if (!charset_end || !(encoding_start = charset_end + 1) ||
                !(encoding_end = strchr(encoding_start, '?')) ||
                !(end_word = strstr(encoding_end + 1, "?="))) goto plain_char;
            payload_start = encoding_end + 1;
            charset_size = (size_t)(charset_end - charset_start);
            encoding_size = (size_t)(encoding_end - encoding_start);
            if (!charset_size || charset_size >= sizeof(charset) ||
                !encoding_size || encoding_size >= sizeof(encoding)) goto plain_char;
            memcpy(charset, charset_start, charset_size);
            charset[charset_size] = '\0';
            memcpy(encoding, encoding_start, encoding_size);
            encoding[encoding_size] = '\0';
            if (eml_ascii_equal(encoding, "B")) {
                decode_rc = eml_decode_transfer((const uint8_t *)payload_start,
                    (size_t)(end_word - payload_start), "base64",
                    &decoded, &decoded_size);
            } else if (eml_ascii_equal(encoding, "Q")) {
                decode_rc = eml_decode_transfer((const uint8_t *)payload_start,
                    (size_t)(end_word - payload_start), "quoted-printable",
                    &decoded, &decoded_size);
                if (decode_rc == 1) {
                    size_t i;
                    for (i = 0u; i < decoded_size; ++i) {
                        if (decoded[i] == '_') decoded[i] = ' ';
                    }
                }
            } else {
                decode_rc = 0;
            }
            if (decode_rc == -1) {
                owner->fatal_error = QW_STATUS_ERR_OUT_OF_MEMORY;
                return 0;
            }
            if (decode_rc == 1) {
                if (!eml_append_decoded_text(owner, buffer, decoded,
                                             decoded_size, charset)) {
                    free(decoded);
                    return 0;
                }
                free(decoded);
                owner->had_encoded_header = 1;
                cursor = (size_t)(end_word + 2 - header);
                {
                    size_t next_word = cursor;
                    while (next_word < size &&
                           (header[next_word] == ' ' ||
                            header[next_word] == '\t' ||
                            header[next_word] == '\r' ||
                            header[next_word] == '\n')) ++next_word;
                    if (next_word + 1u < size && header[next_word] == '=' &&
                        header[next_word + 1u] == '?') cursor = next_word;
                }
                continue;
            }
            owner->had_encoded_header = 1;
            owner->had_unsupported_part = 1;
        }
plain_char:
        {
            size_t next = cursor + 1u;
            while (next < size &&
                   !(header[next] == '=' && next + 1u < size &&
                     header[next + 1u] == '?')) ++next;
            if (!eml_append_decoded_text(owner, buffer,
                    (const uint8_t *)header + cursor, next - cursor, "utf-8"))
                return 0;
            cursor = next;
        }
    }
    return 1;
}

static char *eml_copy_header_utf8(EmlOwner *owner, const char *value)
{
    QwTextBuffer decoded = {0};
    char *copy;
    decoded.limit = EML_MAX_HEADER_VALUE * 4u;
    if (!eml_append_decoded_header(owner, &decoded, value)) {
        free(decoded.data);
        return NULL;
    }
    copy = (char *)malloc(decoded.size + 1u);
    if (!copy) {
        free(decoded.data);
        owner->fatal_error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    if (decoded.size) memcpy(copy, decoded.data, decoded.size);
    copy[decoded.size] = '\0';
    free(decoded.data);
    return copy;
}

static int eml_add_attachment(EmlOwner *owner,
                              const uint8_t *data,
                              size_t size,
                              const char *media_type,
                              const char *raw_name)
{
    char generated_name[40];
    char *name;
    size_t start;
    size_t read_offset;
    size_t write_offset = 0u;
    if (!size) {
        owner->had_unsupported_part = 1;
        return 1;
    }
    if (owner->attachment_count >= EML_MAX_ATTACHMENTS ||
        size > EML_MAX_ATTACHMENT_BYTES - owner->attachment_bytes) {
        owner->had_resource_skip = 1;
        return 1;
    }
    name = eml_copy_header_utf8(owner, raw_name);
    if (!name) {
        if (owner->fatal_error) return 0;
        (void)snprintf(generated_name, sizeof(generated_name),
                       "attachment-%zu", owner->attachment_count + 1u);
        name = (char *)malloc(strlen(generated_name) + 1u);
        if (!name) {
            owner->fatal_error = QW_STATUS_ERR_OUT_OF_MEMORY;
            return 0;
        }
        strcpy(name, generated_name);
    }
    start = 0u;
    for (read_offset = 0u; name[read_offset]; ++read_offset) {
        if (name[read_offset] == '/' || name[read_offset] == '\\')
            start = read_offset + 1u;
    }
    if (start) memmove(name, name + start, strlen(name + start) + 1u);
    for (read_offset = 0u; name[read_offset]; ++read_offset) {
        unsigned char ch = (unsigned char)name[read_offset];
        if (ch < 0x20u || ch == 0x7fu || ch == ':' || ch == '"') continue;
        name[write_offset++] = (char)ch;
    }
    name[write_offset] = '\0';
    if (!write_offset) {
        free(name);
        (void)snprintf(generated_name, sizeof(generated_name),
                       "attachment-%zu", owner->attachment_count + 1u);
        name = (char *)malloc(strlen(generated_name) + 1u);
        if (!name) {
            owner->fatal_error = QW_STATUS_ERR_OUT_OF_MEMORY;
            return 0;
        }
        strcpy(name, generated_name);
    }
    owner->attachments[owner->attachment_count].name = name;
    owner->attachments[owner->attachment_count].media_type =
        (char *)malloc(strlen(media_type) + 1u);
    owner->attachments[owner->attachment_count].data =
        (uint8_t *)malloc(size);
    if (!owner->attachments[owner->attachment_count].media_type ||
        !owner->attachments[owner->attachment_count].data) {
        free((void *)owner->attachments[owner->attachment_count].name);
        free((void *)owner->attachments[owner->attachment_count].media_type);
        free((void *)owner->attachments[owner->attachment_count].data);
        memset(&owner->attachments[owner->attachment_count], 0,
               sizeof(owner->attachments[owner->attachment_count]));
        owner->fatal_error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    strcpy((char *)owner->attachments[owner->attachment_count].media_type,
           media_type);
    memcpy((void *)owner->attachments[owner->attachment_count].data, data, size);
    owner->attachments[owner->attachment_count].size = size;
    owner->attachment_bytes += size;
    ++owner->attachment_count;
    return 1;
}

static void eml_normalize_media_type(const char *source,
                                    char *out,
                                    size_t capacity)
{
    size_t i = 0u;
    while (source[i] && source[i] != ';' && i + 1u < capacity) {
        if (source[i] != ' ' && source[i] != '\t')
            out[i] = (char)tolower((unsigned char)source[i]);
        else
            out[i] = source[i];
        ++i;
    }
    while (i && (out[i - 1u] == ' ' || out[i - 1u] == '\t')) --i;
    out[i] = '\0';
}

static int eml_boundary_line(const uint8_t *line,
                             size_t line_size,
                             const char *boundary,
                             int *out_closing)
{
    size_t boundary_size = strlen(boundary);
    size_t cursor;
    if (line_size < boundary_size + 2u || line[0] != '-' || line[1] != '-' ||
        memcmp(line + 2u, boundary, boundary_size) != 0) return 0;
    cursor = boundary_size + 2u;
    *out_closing = 0;
    if (cursor + 2u <= line_size && line[cursor] == '-' &&
        line[cursor + 1u] == '-') {
        *out_closing = 1;
        cursor += 2u;
    }
    while (cursor < line_size && (line[cursor] == ' ' || line[cursor] == '\t'))
        ++cursor;
    return cursor == line_size;
}

static int eml_parse_entity(EmlOwner *owner,
                            const uint8_t *data,
                            size_t size,
                            size_t depth,
                            EmlHeaders *out_headers);

static int eml_parse_multipart(EmlOwner *owner,
                               const uint8_t *data,
                               size_t size,
                               const char *boundary,
                               size_t depth)
{
    size_t cursor = 0u;
    size_t part_start = 0u;
    int have_part = 0;
    int saw_boundary = 0;
    while (cursor < size) {
        size_t line_start = cursor;
        size_t line_end = line_start;
        size_t next;
        int closing = 0;
        while (line_end < size && data[line_end] != '\n') ++line_end;
        next = line_end < size ? line_end + 1u : line_end;
        if (line_end > line_start && data[line_end - 1u] == '\r') --line_end;
        if (eml_boundary_line(data + line_start, line_end - line_start,
                              boundary, &closing)) {
            saw_boundary = 1;
            if (have_part) {
                size_t part_end = line_start;
                if (part_end > part_start && data[part_end - 1u] == '\n') {
                    --part_end;
                    if (part_end > part_start && data[part_end - 1u] == '\r')
                        --part_end;
                }
                if (!eml_parse_entity(owner, data + part_start,
                                      part_end - part_start,
                                      depth + 1u, NULL)) return 0;
            }
            if (closing) return 1;
            have_part = 1;
            part_start = next;
        }
        if (next == cursor) break;
        cursor = next;
    }
    if (have_part && part_start < size) {
        size_t part_end = size;
        while (part_end > part_start &&
               (data[part_end - 1u] == '\r' || data[part_end - 1u] == '\n'))
            --part_end;
        if (!eml_parse_entity(owner, data + part_start, part_end - part_start,
                              depth + 1u, NULL)) return 0;
    }
    if (!saw_boundary) owner->had_unsupported_part = 1;
    return 1;
}

static int eml_record_body(EmlOwner *owner,
                           const uint8_t *data,
                           size_t size,
                           const char *encoding,
                           const char *charset,
                           int priority)
{
    uint8_t *decoded = NULL;
    size_t decoded_size = 0u;
    QwTextBuffer text = {0};
    int transfer_rc = eml_decode_transfer(data, size, encoding,
                                          &decoded, &decoded_size);
    if (transfer_rc == -1) {
        owner->fatal_error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    if (transfer_rc != 1) {
        owner->had_unsupported_part = 1;
        return 1;
    }
    text.limit = EML_MAX_TEXT_BYTES;
    if (!eml_append_decoded_text(owner, &text, decoded, decoded_size,
                                 charset[0] ? charset : "utf-8")) {
        free(decoded);
        free(text.data);
        if (!owner->fatal_error) owner->fatal_error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    free(decoded);
    if (priority > owner->body_priority) {
        free(owner->body.data);
        owner->body = text;
        owner->body_priority = priority;
        owner->has_body = 1;
    } else {
        free(text.data);
    }
    return 1;
}

static int eml_parse_entity(EmlOwner *owner,
                            const uint8_t *data,
                            size_t size,
                            size_t depth,
                            EmlHeaders *out_headers)
{
    EmlHeaders headers;
    size_t body_offset = 0u;
    const uint8_t *body;
    size_t body_size;
    char media_type[128] = "text/plain";
    char boundary[EML_MAX_BOUNDARY] = "";
    char charset[128] = "";
    char filename[1024] = "";
    int filename_extended = 0;
    int name_extended = 0;
    if (owner->fatal_error) return 0;
    if (depth > EML_MAX_DEPTH || owner->part_count >= EML_MAX_PARTS) {
        owner->fatal_error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    ++owner->part_count;
    if (!eml_parse_headers(owner, data, size, &headers, &body_offset)) {
        owner->had_unsupported_part = 1;
        return 1;
    }
    if (out_headers) *out_headers = headers;
    body = data + body_offset;
    body_size = size - body_offset;
    if (headers.content_type[0])
        eml_normalize_media_type(headers.content_type, media_type,
                                 sizeof(media_type));
    (void)eml_parameter(headers.content_type, "boundary", boundary,
                        sizeof(boundary), NULL);
    (void)eml_parameter(headers.content_type, "charset", charset,
                        sizeof(charset), NULL);
    (void)eml_parameter(headers.content_type, "name", filename,
                        sizeof(filename), &name_extended);
    if (!eml_parameter(headers.disposition, "filename", filename,
                       sizeof(filename), &filename_extended) &&
        !filename[0]) filename[0] = '\0';
    if (filename_extended || name_extended) owner->had_unsupported_part = 1;
    if (strncmp(media_type, "multipart/", 10u) == 0) {
        if (!boundary[0] || strlen(boundary) >= EML_MAX_BOUNDARY) {
            owner->had_unsupported_part = 1;
            return 1;
        }
        return eml_parse_multipart(owner, body, body_size, boundary, depth);
    }
    if (eml_ascii_equal(media_type, "message/rfc822") && !filename[0]) {
        return eml_parse_entity(owner, body, body_size, depth + 1u, NULL);
    }
    if (eml_ascii_equal(media_type, "text/plain") && !filename[0] &&
        !eml_ascii_contains(headers.disposition, "attachment")) {
        return eml_record_body(owner, body, body_size,
                               headers.transfer_encoding, charset, 2);
    }
    if (eml_ascii_equal(media_type, "text/html") && !filename[0] &&
        !eml_ascii_contains(headers.disposition, "attachment")) {
        if (owner->body_priority < 1) {
            QwTextBuffer html_source = {0};
            QwTextBuffer encoded = {0};
            uint8_t *decoded = NULL;
            size_t decoded_size = 0u;
            int transfer_rc = eml_decode_transfer(
                body, body_size, headers.transfer_encoding,
                &decoded, &decoded_size);
            if (transfer_rc == -1) {
                owner->fatal_error = QW_STATUS_ERR_OUT_OF_MEMORY;
                return 0;
            }
            if (transfer_rc == 1) {
                encoded.limit = EML_MAX_TEXT_BYTES;
                if (!eml_append_decoded_text(owner, &encoded, decoded,
                                             decoded_size,
                                             charset[0] ? charset : "utf-8")) {
                    free(decoded);
                    free(encoded.data);
                    if (!owner->fatal_error)
                        owner->fatal_error = QW_STATUS_ERR_RESOURCE_LIMIT;
                    return 0;
                }
                html_source.limit = EML_MAX_TEXT_BYTES;
                if (!eml_append_literal(&html_source,
                        "HTML body is shown as escaped source text.\n\n") ||
                    !qw_text_buffer_append(&html_source, encoded.data,
                                           encoded.size)) {
                    free(decoded);
                    free(encoded.data);
                    free(html_source.data);
                    owner->fatal_error = QW_STATUS_ERR_RESOURCE_LIMIT;
                    return 0;
                }
                free(decoded);
                free(encoded.data);
                free(owner->body.data);
                owner->body = html_source;
                owner->body_priority = 1;
                owner->has_body = 1;
            } else {
                owner->had_unsupported_part = 1;
            }
        }
        return 1;
    }
    {
        uint8_t *decoded = NULL;
        size_t decoded_size = 0u;
        char content_id_name[1024];
        const char *attachment_name = filename;
        int transfer_rc = eml_decode_transfer(body, body_size,
            headers.transfer_encoding, &decoded, &decoded_size);
        if (transfer_rc == -1) {
            owner->fatal_error = QW_STATUS_ERR_OUT_OF_MEMORY;
            return 0;
        }
        if (transfer_rc != 1) {
            owner->had_unsupported_part = 1;
            return 1;
        }
        if (!attachment_name[0] && headers.content_id[0]) {
            size_t i;
            size_t n = strlen(headers.content_id);
            if (n >= sizeof(content_id_name)) n = sizeof(content_id_name) - 1u;
            memcpy(content_id_name, headers.content_id, n);
            content_id_name[n] = '\0';
            for (i = 0u; i < n; ++i) {
                if (content_id_name[i] == '<' || content_id_name[i] == '>')
                    content_id_name[i] = '_';
            }
            attachment_name = content_id_name;
        }
        if (!attachment_name[0]) attachment_name = "attachment";
        if (!eml_add_attachment(owner, decoded, decoded_size,
                                media_type, attachment_name)) {
            free(decoded);
            return 0;
        }
        free(decoded);
    }
    return 1;
}

static int eml_probe(const void *input, size_t size)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t header_limit = size < EML_MAX_HEADER_BYTES ? size : EML_MAX_HEADER_BYTES;
    size_t body_offset;
    size_t line_start = 0u;
    unsigned common_headers = 0u;
    int has_mime_version = 0;
    int has_content_type = 0;
    if (!data || size < 12u || size > EML_MAX_INPUT_BYTES ||
        !eml_header_separator(data, header_limit, &body_offset,
                              EML_MAX_HEADER_BYTES)) return 0;
    while (line_start < body_offset) {
        size_t line_end = line_start;
        size_t colon;
        while (line_end < body_offset && data[line_end] != '\n') ++line_end;
        if (line_end > line_start && data[line_end - 1u] == '\r') --line_end;
        if (line_end == line_start) break;
        if (data[line_start] != ' ' && data[line_start] != '\t') {
            colon = line_start;
            while (colon < line_end && data[colon] != ':') ++colon;
            if (colon < line_end) {
                const uint8_t *name = data + line_start;
                size_t name_size = colon - line_start;
                if (eml_span_equal(name, name_size, "from") ||
                    eml_span_equal(name, name_size, "to") ||
                    eml_span_equal(name, name_size, "cc") ||
                    eml_span_equal(name, name_size, "subject") ||
                    eml_span_equal(name, name_size, "date") ||
                    eml_span_equal(name, name_size, "message-id"))
                    ++common_headers;
                else if (eml_span_equal(name, name_size, "mime-version"))
                    has_mime_version = 1;
                else if (eml_span_equal(name, name_size, "content-type"))
                    has_content_type = 1;
            }
        }
        line_start = line_end;
        while (line_start < body_offset && data[line_start] != '\n') ++line_start;
        if (line_start < body_offset) ++line_start;
    }
    return (has_mime_version && has_content_type) || common_headers >= 3u;
}

static void eml_owner_dispose(void *context)
{
    EmlOwner *owner = (EmlOwner *)context;
    size_t i;
    if (!owner) return;
    free(owner->html);
    free(owner->body.data);
    for (i = 0u; i < owner->attachment_count; ++i) {
        free((void *)owner->attachments[i].name);
        free((void *)owner->attachments[i].media_type);
        free((void *)owner->attachments[i].data);
    }
    free(owner);
}

static int eml_append_metadata_row(QwTextBuffer *html,
                                   const char *label,
                                   const char *value,
                                   EmlOwner *owner)
{
    QwTextBuffer decoded = {0};
    int ok;
    if (!value[0]) return 1;
    decoded.limit = EML_MAX_HEADER_VALUE * 4u;
    if (!eml_append_decoded_header(owner, &decoded, value)) {
        free(decoded.data);
        return 0;
    }
    ok = eml_append_literal(html, "<tr><th>") &&
         eml_append_literal(html, label) &&
         eml_append_literal(html, "</th><td>") &&
         eml_append_html_escaped(html, decoded.data, decoded.size) &&
         eml_append_literal(html, "</td></tr>");
    free(decoded.data);
    return ok;
}

static int eml_build_html(EmlOwner *owner, const EmlHeaders *headers)
{
    QwTextBuffer html = {0};
    size_t i;
    html.limit = EML_MAX_HTML_BYTES;
    if (!eml_append_literal(&html,
            "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
            "@page{size:A4;margin:18mm}body{font-family:sans-serif;color:#111;"
            "font-size:10.5pt;line-height:1.35}h1{font-size:18pt}"
            "table{border-collapse:collapse;width:100%;margin:10pt 0}"
            "th{text-align:left;width:16%;vertical-align:top;padding:3pt 6pt 3pt 0}"
            "td{padding:3pt 0;overflow-wrap:anywhere}pre{font-family:sans-serif;"
            "white-space:pre-wrap;overflow-wrap:anywhere}li{margin:3pt 0}"
            ".note{color:#555;font-size:9pt;margin-top:10pt}"
            "</style></head><body><h1>")) return 0;
    if (headers->subject[0]) {
        QwTextBuffer subject = {0};
        subject.limit = EML_MAX_HEADER_VALUE * 4u;
        if (!eml_append_decoded_header(owner, &subject, headers->subject) ||
            !eml_append_html_escaped(&html, subject.data, subject.size)) {
            free(subject.data);
            free(html.data);
            return 0;
        }
        free(subject.data);
    } else if (!eml_append_literal(&html, "Email message")) {
        free(html.data);
        return 0;
    }
    if (!eml_append_literal(&html, "</h1><table>")) goto fail;
    if (!eml_append_metadata_row(&html, "From", headers->from, owner) ||
        !eml_append_metadata_row(&html, "To", headers->to, owner) ||
        !eml_append_metadata_row(&html, "Cc", headers->cc, owner) ||
        !eml_append_metadata_row(&html, "Reply-To", headers->reply_to, owner) ||
        !eml_append_metadata_row(&html, "Date", headers->date, owner) ||
        !eml_append_metadata_row(&html, "Message-ID", headers->message_id, owner) ||
        !eml_append_literal(&html, "</table><hr><pre>")) goto fail;
    if (owner->has_body) {
        if (!eml_append_html_escaped(&html, owner->body.data,
                                    owner->body.size)) goto fail;
    } else if (!eml_append_literal(&html,
                   "No supported text/plain or text/html MIME body was found.")) {
        goto fail;
    }
    if (!eml_append_literal(&html, "</pre>")) goto fail;
    if (owner->attachment_count) {
        if (!eml_append_literal(&html, "<h2>Attachments</h2><ul>")) goto fail;
        for (i = 0u; i < owner->attachment_count; ++i) {
            const QwAttachment *attachment = &owner->attachments[i];
            if (!eml_append_literal(&html, "<li><strong>") ||
                !eml_append_html_escaped(&html, attachment->name,
                                         strlen(attachment->name)) ||
                !eml_append_literal(&html, "</strong> <span>(") ||
                !eml_append_html_escaped(&html, attachment->media_type,
                                         strlen(attachment->media_type)) ||
                !eml_append_literal(&html, ", ")) goto fail;
            {
                char size_text[48];
                int size = snprintf(size_text, sizeof(size_text),
                                    "%zu bytes", attachment->size);
                if (size < 0 || (size_t)size >= sizeof(size_text) ||
                    !qw_text_buffer_append(&html, size_text, (size_t)size))
                    goto fail;
            }
            if (!eml_append_literal(&html, ")</span></li>")) goto fail;
        }
        if (!eml_append_literal(&html, "</ul><p class=\"note\">Attachment data "
                "is retained in the layout document but is not embedded in PDF output.</p>"))
            goto fail;
    }
    if (owner->had_unsupported_part || owner->had_charset_fallback ||
        owner->had_resource_skip || owner->had_malformed_header) {
        if (!eml_append_literal(&html,
                "<p class=\"note\">Some MIME parts, header encodings, or "
                "character encodings could not be represented completely.</p>"))
            goto fail;
    }
    if (owner->had_encoded_header &&
        !eml_append_literal(&html,
            "<p class=\"note\">RFC 2047 encoded header words were decoded where supported.</p>"))
        goto fail;
    if (!eml_append_literal(&html, "</body></html>")) goto fail;
    owner->html = html.data;
    return 1;

fail:
    free(html.data);
    return 0;
}

int qw_eml_probe(const void *data, size_t size)
{
    return eml_probe(data, size);
}

int qw_eml_native_open_layout(const void *data,
                              size_t size,
                              QwInputFormat input_format,
                              const QwConvertOptions *options,
                              QwLayoutDocument *out_document,
                              QwConvertReport *report)
{
    EmlOwner *owner;
    EmlHeaders root_headers;
    int rc;
    (void)options;
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (input_format != QW_FORMAT_EML || !data || !size)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (size > EML_MAX_INPUT_BYTES) return QW_STATUS_ERR_RESOURCE_LIMIT;
    if (!eml_probe(data, size)) return QW_STATUS_ERR_FORMAT;
    owner = (EmlOwner *)calloc(1u, sizeof(*owner));
    if (!owner) return QW_STATUS_ERR_OUT_OF_MEMORY;
    owner->fatal_error = QW_STATUS_OK;
    memset(&root_headers, 0, sizeof(root_headers));
    if (!eml_parse_entity(owner, (const uint8_t *)data, size, 0u,
                          &root_headers)) {
        rc = owner->fatal_error ? owner->fatal_error : QW_STATUS_ERR_FORMAT;
        eml_owner_dispose(owner);
        if (report && rc == QW_STATUS_ERR_RESOURCE_LIMIT)
            (void)snprintf(report->note, sizeof(report->note),
                           "EML MIME structure exceeded the depth or part limit");
        return rc;
    }
    if (owner->fatal_error) {
        rc = owner->fatal_error;
        eml_owner_dispose(owner);
        return rc;
    }
    if (!eml_build_html(owner, &root_headers)) {
        rc = owner->fatal_error ? owner->fatal_error : QW_STATUS_ERR_RESOURCE_LIMIT;
        eml_owner_dispose(owner);
        return rc;
    }
    owner->attachment_set.items = owner->attachments;
    owner->attachment_set.count = owner->attachment_count;
    owner->attachment_set.context = owner;
    owner->attachment_set.dispose = eml_owner_dispose;
    out_document->html_utf8 = (const uint8_t *)owner->html;
    out_document->html_size = strlen(owner->html);
    out_document->attachments = &owner->attachment_set;
    out_document->context = owner;
    out_document->dispose = eml_owner_dispose;
    if (report) {
        (void)snprintf(report->note, sizeof(report->note),
            "EML headers and %s text body extracted; %zu attachment(s) retained as metadata but not embedded in PDF%s%s%s",
            owner->body_priority == 2 ? "plain-text" :
            owner->body_priority == 1 ? "escaped HTML-source" : "no supported",
            owner->attachment_count,
            owner->had_unsupported_part ? "; unsupported MIME data skipped" : "",
            owner->had_charset_fallback ? "; some charset conversion used replacement" : "",
            owner->had_resource_skip ? "; attachment resource limit reached" : "");
    }
    return QW_STATUS_OK;
}

#ifndef QUIREWISE_STANDALONE_EMAIL_PARSER
static int eml_append_layout_sections(QwTextBuffer *html,
                                     const QwLayoutDocument *document)
{
    size_t section_index;
    if (document->html_utf8 && document->html_size)
        return qw_text_buffer_append(html,
            (const char *)document->html_utf8, document->html_size);
    if (!document->sections || !document->section_count) return 0;
    for (section_index = 0u; section_index < document->section_count;
         ++section_index) {
        const QwLayoutSection *section = &document->sections[section_index];
        size_t paragraph_index;
        if (section->title && section->title[0]) {
            if (!eml_append_literal(html, "<h2>") ||
                !eml_append_html_escaped(html, section->title,
                                         strlen(section->title)) ||
                !eml_append_literal(html, "</h2>")) return 0;
        }
        for (paragraph_index = 0u;
             paragraph_index < section->paragraph_count; ++paragraph_index) {
            const QwLayoutParagraph *paragraph =
                &section->paragraphs[paragraph_index];
            size_t run_index;
            unsigned level = paragraph->heading_level;
            char tag[5] = "p";
            if (paragraph->run_count && !paragraph->runs) return 0;
            if (level > 6u) level = 6u;
            if (level) {
                tag[0] = 'h';
                tag[1] = (char)('0' + level);
                tag[2] = '\0';
            }
            if (paragraph->page_break_before &&
                !eml_append_literal(html,
                    "<div style=\"break-before:page\"></div>")) return 0;
            if (!eml_append_literal(html, "<") ||
                !eml_append_literal(html, tag) ||
                !eml_append_literal(html, ">")) return 0;
            for (run_index = 0u; run_index < paragraph->run_count; ++run_index) {
                const QwLayoutRun *run = &paragraph->runs[run_index];
                if (run->text_size && !run->utf8_text) return 0;
                if (run->style_flags & QW_LAYOUT_STYLE_BOLD) {
                    if (!eml_append_literal(html, "<strong>")) return 0;
                }
                if (run->style_flags & QW_LAYOUT_STYLE_ITALIC) {
                    if (!eml_append_literal(html, "<em>")) return 0;
                }
                if (run->style_flags & QW_LAYOUT_STYLE_CODE) {
                    if (!eml_append_literal(html, "<code>")) return 0;
                }
                if (!eml_append_html_escaped(html,
                        (const char *)run->utf8_text, run->text_size)) return 0;
                if (run->style_flags & QW_LAYOUT_STYLE_CODE) {
                    if (!eml_append_literal(html, "</code>")) return 0;
                }
                if (run->style_flags & QW_LAYOUT_STYLE_ITALIC) {
                    if (!eml_append_literal(html, "</em>")) return 0;
                }
                if (run->style_flags & QW_LAYOUT_STYLE_BOLD) {
                    if (!eml_append_literal(html, "</strong>")) return 0;
                }
            }
            if (!eml_append_literal(html, "</") ||
                !eml_append_literal(html, tag) ||
                !eml_append_literal(html, ">")) return 0;
        }
    }
    return 1;
}

static uint64_t eml_hash_bytes(uint64_t hash,
                              const uint8_t *data,
                              size_t size)
{
    size_t i;
    for (i = 0u; i < size; ++i) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int eml_bytes_contain(const uint8_t *data,
                             size_t size,
                             const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t i;
    if (!needle_size || needle_size > size) return 0;
    for (i = 0u; i <= size - needle_size; ++i) {
        if (memcmp(data + i, needle, needle_size) == 0) return 1;
    }
    return 0;
}

static int eml_boundary_collision(const QwLayoutDocument *document,
                                 const uint8_t *body,
                                 size_t body_size,
                                 const char *boundary)
{
    size_t i;
    if (eml_bytes_contain(body, body_size, boundary)) return 1;
    if (!document->attachments) return 0;
    for (i = 0u; i < document->attachments->count; ++i) {
        const QwAttachment *attachment = &document->attachments->items[i];
        if (eml_bytes_contain(attachment->data, attachment->size, boundary))
            return 1;
    }
    return 0;
}

static int eml_choose_boundary(const QwLayoutDocument *document,
                               const uint8_t *body,
                               size_t body_size,
                               char *boundary,
                               size_t boundary_capacity)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    unsigned attempt;
    size_t i;
    hash = eml_hash_bytes(hash, body, body_size);
    if (document->attachments) {
        for (i = 0u; i < document->attachments->count; ++i) {
            const QwAttachment *attachment = &document->attachments->items[i];
            hash = eml_hash_bytes(hash, attachment->data, attachment->size);
        }
    }
    for (attempt = 0u; attempt < 64u; ++attempt) {
        int written = snprintf(boundary, boundary_capacity,
            "----Quirewise-%016llx-%u",
            (unsigned long long)hash, attempt);
        if (written < 0 || (size_t)written >= boundary_capacity) return 0;
        if (!eml_boundary_collision(document, body, body_size, boundary)) return 1;
    }
    return 0;
}

static int eml_append_base64(QwTextBuffer *output,
                             const uint8_t *data,
                             size_t size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t cursor = 0u;
    size_t column = 0u;
    while (cursor < size) {
        size_t remaining = size - cursor;
        size_t count = remaining >= 3u ? 3u : remaining;
        uint32_t value = (uint32_t)data[cursor] << 16;
        char encoded[4];
        if (count > 1u) value |= (uint32_t)data[cursor + 1u] << 8;
        if (count > 2u) value |= data[cursor + 2u];
        encoded[0] = alphabet[(value >> 18) & 0x3fu];
        encoded[1] = alphabet[(value >> 12) & 0x3fu];
        encoded[2] = count > 1u ? alphabet[(value >> 6) & 0x3fu] : '=';
        encoded[3] = count > 2u ? alphabet[value & 0x3fu] : '=';
        if (!qw_text_buffer_append(output, encoded, sizeof(encoded))) return 0;
        cursor += count;
        column += sizeof(encoded);
        if (column == 76u) {
            if (!eml_append_literal(output, "\r\n")) return 0;
            column = 0u;
        }
    }
    if (column && !eml_append_literal(output, "\r\n")) return 0;
    if (!size && !eml_append_literal(output, "\r\n")) return 0;
    return 1;
}

static void eml_safe_media_type(const char *source,
                                char *out,
                                size_t out_capacity)
{
    size_t i;
    int slash_count = 0;
    int valid = out_capacity > 1u;
    size_t size = strlen(source);
    if (size >= out_capacity) valid = 0;
    for (i = 0u; valid && i < size; ++i) {
        unsigned char ch = (unsigned char)source[i];
        if (ch == '/') {
            ++slash_count;
        } else if (!((ch >= 'A' && ch <= 'Z') ||
                     (ch >= 'a' && ch <= 'z') ||
                     (ch >= '0' && ch <= '9')) &&
                   ch != '!' && ch != '#' && ch != '$' &&
                   ch != '&' && ch != '^' && ch != '_' && ch != '.' &&
                   ch != '+' && ch != '-') {
            valid = 0;
        }
    }
    if (slash_count != 1) valid = 0;
    if (valid) {
        memcpy(out, source, size + 1u);
    } else {
        (void)snprintf(out, out_capacity, "application/octet-stream");
    }
}

static int eml_append_attachment_part(QwTextBuffer *output,
                                      const QwAttachment *attachment,
                                      const char *boundary)
{
    char media_type[128];
    char filename[256];
    size_t input_offset = 0u;
    size_t output_offset = 0u;
    eml_safe_media_type(attachment->media_type, media_type,
                        sizeof(media_type));
    while (attachment->name[input_offset] && output_offset + 1u < sizeof(filename)) {
        unsigned char ch = (unsigned char)attachment->name[input_offset++];
        if (ch >= 0x21u && ch <= 0x7eu && ch != '"' && ch != '\\' &&
            ch != '/' && ch != ';') {
            filename[output_offset++] = (char)ch;
        } else if (ch == ' ') {
            filename[output_offset++] = '_';
        } else if (ch >= 0x80u) {
            filename[output_offset++] = '_';
        }
    }
    filename[output_offset] = '\0';
    if (!filename[0]) strcpy(filename, "attachment.bin");
    if (!eml_append_literal(output, "--") ||
        !eml_append_literal(output, boundary) ||
        !eml_append_literal(output, "\r\nContent-Type: ") ||
        !eml_append_literal(output, media_type) ||
        !eml_append_literal(output, "\r\nContent-Transfer-Encoding: base64\r\n") ||
        !eml_append_literal(output, "Content-Disposition: attachment; filename=\"") ||
        !eml_append_literal(output, filename) ||
        !eml_append_literal(output, "\"\r\n\r\n") ||
        !eml_append_base64(output, attachment->data, attachment->size)) return 0;
    return 1;
}

static int eml_encode_layout_document(const QwLayoutDocument *document,
                                     const QwConvertOptions *options,
                                     size_t output_limit,
                                     uint8_t **out_data,
                                     size_t *out_size,
                                     size_t *out_pages,
                                     QwConvertReport *report)
{
    QwTextBuffer body = {0};
    QwTextBuffer output = {0};
    char boundary[64];
    size_t i;
    (void)options;
    if (!document || !out_data || !out_size || !out_pages)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    *out_pages = 0u;
    body.limit = EML_MAX_HTML_BYTES;
    if (document->html_utf8 && document->html_size) {
        if (!eml_append_layout_sections(&body, document)) {
            free(body.data);
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
    } else if (!eml_append_literal(&body,
                   "<!doctype html><html><head><meta charset=\"utf-8\"></head><body>") ||
               !eml_append_layout_sections(&body, document) ||
               !eml_append_literal(&body, "</body></html>")) {
        free(body.data);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (!eml_choose_boundary(document, (const uint8_t *)body.data,
                             body.size, boundary, sizeof(boundary))) {
        free(body.data);
        return QW_STATUS_ERR_FORMAT;
    }
    output.limit = output_limit && output_limit < EML_MAX_OUTPUT_BYTES
        ? output_limit : EML_MAX_OUTPUT_BYTES;
    if (!eml_append_literal(&output,
            "Subject: Quirewise document\r\nMIME-Version: 1.0\r\n") ||
        !eml_append_literal(&output, "Content-Type: multipart/mixed; boundary=\"") ||
        !eml_append_literal(&output, boundary) ||
        !eml_append_literal(&output, "\"\r\n\r\n--") ||
        !eml_append_literal(&output, boundary) ||
        !eml_append_literal(&output,
            "\r\nContent-Type: text/html; charset=utf-8\r\n"
            "Content-Transfer-Encoding: base64\r\n\r\n") ||
        !eml_append_base64(&output, (const uint8_t *)body.data, body.size)) {
        free(body.data);
        free(output.data);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    free(body.data);
    if (document->attachments) {
        for (i = 0u; i < document->attachments->count; ++i) {
            if (!eml_append_attachment_part(&output,
                    &document->attachments->items[i], boundary)) {
                free(output.data);
                return QW_STATUS_ERR_RESOURCE_LIMIT;
            }
        }
    }
    if (!eml_append_literal(&output, "--") ||
        !eml_append_literal(&output, boundary) ||
        !eml_append_literal(&output, "--\r\n")) {
        free(output.data);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    *out_data = (uint8_t *)output.data;
    *out_size = output.size;
    *out_pages = 1u;
    if (report) {
        (void)snprintf(report->note, sizeof(report->note),
            "EML MIME output written with UTF-8 HTML body and %zu attachment(s); non-ASCII filenames are replaced in MIME headers",
            document->attachments ? document->attachments->count : 0u);
    }
    return QW_STATUS_OK;
}

static int qw_eml_write_layout_document(const QwLayoutDocument *document,
                                        const QwConvertOptions *options,
                                        uint8_t **out_data,
                                        size_t *out_size,
                                        size_t *out_pages,
                                        QwConvertReport *report)
{
    return eml_encode_layout_document(document, options,
        EML_MAX_OUTPUT_BYTES, out_data, out_size, out_pages, report);
}

static int qw_eml_write_layout_document_to_stream(
    const QwLayoutDocument *document,
    const QwConvertOptions *options,
    FILE *stream,
    size_t max_output_bytes,
    size_t *out_size,
    size_t *out_pages,
    QwConvertReport *report)
{
    uint8_t *output = NULL;
    size_t output_size = 0u;
    size_t pages = 0u;
    int rc;
    if (!stream || !out_size || !out_pages) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_size = 0u;
    *out_pages = 0u;
    rc = eml_encode_layout_document(document, options, max_output_bytes,
                                    &output, &output_size, &pages, report);
    if (rc != QW_STATUS_OK) return rc;
    if (max_output_bytes && output_size > max_output_bytes) {
        free(output);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (fwrite(output, 1u, output_size, stream) != output_size) {
        free(output);
        return QW_STATUS_ERR_IO;
    }
    free(output);
    *out_size = output_size;
    *out_pages = pages;
    return QW_STATUS_OK;
}

const QwLayoutStreamWriterDescriptor qw_layout_stream_writer_eml = {
    QW_FORMAT_EML,
    "native RFC 5322/MIME EML stream writer",
    qw_eml_write_layout_document_to_stream
};

const QwWriterDescriptor qw_writer_eml = {
    QW_FORMAT_EML,
    0u,
    12u,
    "native RFC 5322/MIME EML writer",
    NULL,
    0u,
    0u,
    QW_MEDIA_KIND_BIT(QW_MEDIA_KIND_LAYOUT_DOCUMENT),
    qw_eml_write_layout_document,
    NULL,
    EML_MAX_OUTPUT_BYTES,
    0u
};

const QwFormatDescriptor qw_format_eml = {
    .id = QW_FORMAT_EML,
    .name = "EML email message",
    .capabilities = QW_FORMAT_CAP_MULTI_PAGE,
    .probe = qw_eml_probe,
    .open_memory = NULL,
    .open_layout = qw_eml_native_open_layout,
    .open_audio = NULL,
    .max_input_bytes = EML_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .convert_to_pdf = NULL
};
#endif
