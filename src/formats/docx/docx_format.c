#include "format_registry.h"
#include "docx_format.h"

#ifndef QUIREWISE_STANDALONE_DOCX_PARSER
#include "../../backends/mupdf_backend.h"
#include "../../backends/svg_backend.h"
#endif
#include "../archive/zip_native.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct QwDocxHtml {
    char *data;
    size_t size;
    size_t capacity;
} QwDocxHtml;

static int html_append(QwDocxHtml *html, const char *data, size_t size)
{
    size_t capacity;
    char *grown;
    if (size > SIZE_MAX - html->size - 1u) return 0;
    if (html->size + size + 1u > html->capacity) {
        capacity = html->capacity ? html->capacity : 1024u;
        while (capacity < html->size + size + 1u) {
            if (capacity > SIZE_MAX / 2u) return 0;
            capacity *= 2u;
        }
        grown = (char *)realloc(html->data, capacity);
        if (!grown) return 0;
        html->data = grown;
        html->capacity = capacity;
    }
    memcpy(html->data + html->size, data, size);
    html->size += size;
    html->data[html->size] = '\0';
    return 1;
}

static int html_literal(QwDocxHtml *html, const char *text)
{
    return html_append(html, text, strlen(text));
}

static int html_codepoint(QwDocxHtml *html, unsigned long cp)
{
    char bytes[4];
    size_t size;
    if (cp == '&') return html_literal(html, "&amp;");
    if (cp == '<') return html_literal(html, "&lt;");
    if (cp == '>') return html_literal(html, "&gt;");
    if (cp >= 0xd800u && cp <= 0xdfffu) cp = 0xfffdu;
    if (cp <= 0x7fu) {
        bytes[0] = (char)cp; size = 1;
    } else if (cp <= 0x7ffu) {
        bytes[0] = (char)(0xc0u | (cp >> 6));
        bytes[1] = (char)(0x80u | (cp & 0x3fu)); size = 2;
    } else if (cp <= 0xffffu && !(cp >= 0xd800u && cp <= 0xdfffu)) {
        bytes[0] = (char)(0xe0u | (cp >> 12));
        bytes[1] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        bytes[2] = (char)(0x80u | (cp & 0x3fu)); size = 3;
    } else if (cp <= 0x10ffffu) {
        bytes[0] = (char)(0xf0u | (cp >> 18));
        bytes[1] = (char)(0x80u | ((cp >> 12) & 0x3fu));
        bytes[2] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        bytes[3] = (char)(0x80u | (cp & 0x3fu)); size = 4;
    } else {
        return html_literal(html, "\xef\xbf\xbd");
    }
    return html_append(html, bytes, size);
}

static int append_word_text(QwDocxHtml *html, const uint8_t *text, size_t size)
{
    size_t i = 0;
    while (i < size) {
        unsigned long cp;
        size_t count;
        unsigned char first = text[i];
        if (first == '&') {
            size_t end = i + 1u;
            unsigned long value = 0;
            unsigned int base = 10;
            int recognized = 0;
            while (end < size && end - i < 16u && text[end] != ';') ++end;
            if (end >= size || text[end] != ';') return 0;
            if (text[i + 1u] == '#') {
                size_t cursor = i + 2u;
                if (cursor < end && (text[cursor] == 'x' || text[cursor] == 'X')) {
                    base = 16;
                    ++cursor;
                }
                if (cursor == end) return 0;
                for (; cursor < end; ++cursor) {
                    unsigned char digit = text[cursor];
                    unsigned int n;
                    if (digit >= '0' && digit <= '9') n = digit - '0';
                    else if (base == 16 && digit >= 'a' && digit <= 'f') n = digit - 'a' + 10u;
                    else if (base == 16 && digit >= 'A' && digit <= 'F') n = digit - 'A' + 10u;
                    else return 0;
                    if (value > (0x10ffffu - n) / base) return 0;
                    value = value * base + n;
                }
                recognized = 1;
            } else if (end - i + 1u == 4u && memcmp(text + i, "&lt;", 4) == 0) {
                value = '<'; recognized = 1;
            } else if (end - i + 1u == 4u && memcmp(text + i, "&gt;", 4) == 0) {
                value = '>'; recognized = 1;
            } else if (end - i + 1u == 5u && memcmp(text + i, "&amp;", 5) == 0) {
                value = '&'; recognized = 1;
            } else if (end - i + 1u == 6u && memcmp(text + i, "&quot;", 6) == 0) {
                value = '"'; recognized = 1;
            } else if (end - i + 1u == 6u && memcmp(text + i, "&apos;", 6) == 0) {
                value = '\''; recognized = 1;
            }
            if (!recognized || !html_codepoint(html, value)) return 0;
            i = end + 1u;
            continue;
        }
        if (first < 0x80u) {
            cp = first; count = 1;
        } else if (first >= 0xc2u && first <= 0xdfu && i + 1u < size &&
                   (text[i + 1u] & 0xc0u) == 0x80u) {
            cp = ((unsigned long)(first & 0x1fu) << 6) | (text[i + 1u] & 0x3fu);
            count = 2;
        } else if (first >= 0xe0u && first <= 0xefu && i + 2u < size &&
                   (text[i + 1u] & 0xc0u) == 0x80u &&
                   (text[i + 2u] & 0xc0u) == 0x80u &&
                   !(first == 0xe0u && text[i + 1u] < 0xa0u) &&
                   !(first == 0xedu && text[i + 1u] >= 0xa0u)) {
            cp = ((unsigned long)(first & 0x0fu) << 12) |
                 ((unsigned long)(text[i + 1u] & 0x3fu) << 6) |
                 (text[i + 2u] & 0x3fu);
            count = 3;
        } else if (first >= 0xf0u && first <= 0xf4u && i + 3u < size &&
                   (text[i + 1u] & 0xc0u) == 0x80u &&
                   (text[i + 2u] & 0xc0u) == 0x80u &&
                   (text[i + 3u] & 0xc0u) == 0x80u &&
                   !(first == 0xf0u && text[i + 1u] < 0x90u) &&
                   !(first == 0xf4u && text[i + 1u] >= 0x90u)) {
            cp = ((unsigned long)(first & 0x07u) << 18) |
                 ((unsigned long)(text[i + 1u] & 0x3fu) << 12) |
                 ((unsigned long)(text[i + 2u] & 0x3fu) << 6) |
                 (text[i + 3u] & 0x3fu);
            count = 4;
        } else {
            return 0;
        }
        if (cp == '<') {
            if (!html_literal(html, "&lt;")) return 0;
        } else if (cp == '>') {
            if (!html_literal(html, "&gt;")) return 0;
        } else if (cp == '&') {
            if (!html_literal(html, "&amp;")) return 0;
        } else if (!html_append(html, (const char *)text + i, count)) {
            return 0;
        }
        i += count;
    }
    return 1;
}

static int xml_local_name_is(const uint8_t *name, size_t size, const char *expected)
{
    size_t start = 0;
    size_t i;
    size_t expected_size = strlen(expected);
    for (i = 0; i < size; ++i) if (name[i] == ':') start = i + 1u;
    return size - start == expected_size &&
           memcmp(name + start, expected, expected_size) == 0;
}

static int xml_tag_has_false_value(const uint8_t *xml, size_t start, size_t end)
{
    static const char *const false_values[] = {
        "val=\"0\"", "val='0'", "val=\"false\"", "val='false'",
        "val=\"none\"", "val='none'"
    };
    size_t item;
    for (item = 0; item < sizeof(false_values) / sizeof(false_values[0]); ++item) {
        size_t size = strlen(false_values[item]);
        size_t i;
        for (i = start; i + size <= end; ++i) {
            if (memcmp(xml + i, false_values[item], size) == 0) return 1;
        }
    }
    return 0;
}

static int xml_attribute_value(const uint8_t *xml, size_t start, size_t end,
                               const char *attribute,
                               const uint8_t **value, size_t *value_size)
{
    size_t i = start;
    if (!xml || !attribute || !value || !value_size) return 0;
    while (i < end) {
        size_t name_start;
        size_t name_end;
        size_t value_start;
        uint8_t quote;
        while (i < end && (xml[i] == ' ' || xml[i] == '\t' ||
                           xml[i] == '\r' || xml[i] == '\n' ||
                           xml[i] == '/')) ++i;
        name_start = i;
        while (i < end && xml[i] != '=' && xml[i] != ' ' &&
               xml[i] != '\t' && xml[i] != '\r' && xml[i] != '\n' &&
               xml[i] != '/' && xml[i] != '>') ++i;
        name_end = i;
        if (name_end == name_start) {
            if (i < end) ++i;
            continue;
        }
        while (i < end && (xml[i] == ' ' || xml[i] == '\t' ||
                           xml[i] == '\r' || xml[i] == '\n')) ++i;
        if (i >= end || xml[i] != '=') continue;
        ++i;
        while (i < end && (xml[i] == ' ' || xml[i] == '\t' ||
                           xml[i] == '\r' || xml[i] == '\n')) ++i;
        if (i >= end || (xml[i] != '\'' && xml[i] != '"')) continue;
        quote = xml[i++];
        value_start = i;
        while (i < end && xml[i] != quote) ++i;
        if (i >= end) return 0;
        if (xml_local_name_is(xml + name_start, name_end - name_start,
                              attribute)) {
            *value = xml + value_start;
            *value_size = i - value_start;
            return 1;
        }
        ++i;
    }
    return 0;
}

static const char *docx_heading_tag(const uint8_t *value, size_t size)
{
    static const char *const heading_names[] = {
        "Heading1", "Heading2", "Heading3", "Heading4", "Heading5", "Heading6"
    };
    static const char *const heading_tags[] = {
        "h1", "h2", "h3", "h4", "h5", "h6"
    };
    size_t i;
    if (size == 5u && memcmp(value, "Title", 5u) == 0) return "h1";
    if (size == 8u && memcmp(value, "Subtitle", 8u) == 0) return "h2";
    for (i = 0; i < sizeof(heading_names) / sizeof(heading_names[0]); ++i) {
        if (size == strlen(heading_names[i]) &&
            memcmp(value, heading_names[i], size) == 0) {
            return heading_tags[i];
        }
    }
    return NULL;
}

static const char *docx_alignment(const uint8_t *value, size_t size)
{
    if ((size == 6u && memcmp(value, "center", 6u) == 0) ||
        (size == 6u && memcmp(value, "centre", 6u) == 0)) return "center";
    if (size == 5u && memcmp(value, "right", 5u) == 0) return "right";
    if ((size == 4u && memcmp(value, "both", 4u) == 0) ||
        (size == 10u && memcmp(value, "distribute", 10u) == 0)) return "justify";
    if ((size == 4u && memcmp(value, "left", 4u) == 0) ||
        (size == 5u && memcmp(value, "start", 5u) == 0)) return "left";
    if (size == 3u && memcmp(value, "end", 3u) == 0) return "right";
    return NULL;
}

static int html_open_paragraph(QwDocxHtml *html, const char *tag,
                               const char *alignment)
{
    if (!html_literal(html, "<") || !html_literal(html, tag)) return 0;
    if (alignment && (!html_literal(html, " style=\"text-align:") ||
                      !html_literal(html, alignment) ||
                      !html_literal(html, "\""))) return 0;
    return html_literal(html, ">");
}

static int html_close_paragraph(QwDocxHtml *html, const char *tag)
{
    return html_literal(html, "</") && html_literal(html, tag) &&
           html_literal(html, ">");
}

static int docx_append_page_break(QwDocxHtml *html)
{
    return html_literal(html,
        "<br style=\"display:block;page-break-before:always\">");
}

enum {
    DOCX_MAX_IMAGE_SIZE = 32 * 1024 * 1024,
    DOCX_MAX_TOTAL_IMAGE_SIZE = 64 * 1024 * 1024,
    DOCX_MAX_HTML_IMAGE_WIDTH = 10000
};

static int docx_ascii_equal(const char *left, const char *right)
{
    while (*left && *right) {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return 0;
    }
    return *left == '\0' && *right == '\0';
}

static int docx_resolve_relationship_target(const uint8_t *target,
                                            size_t target_size,
                                            char *path,
                                            size_t path_capacity)
{
    char decoded[2048];
    char normalized[2048];
    size_t restore_points[256];
    size_t decoded_size = 0;
    size_t normalized_size = 0;
    size_t segment_count = 0;
    size_t i = 0;
    size_t position;
    if (!target || target_size == 0 || !path || path_capacity == 0) return 0;

    if (target[0] != '/') {
        memcpy(decoded, "word/", 5u);
        decoded_size = 5u;
    } else {
        while (i < target_size && target[i] == '/') ++i;
    }
    for (; i < target_size; ++i) {
        uint8_t value = target[i];
        if (value == '%') {
            unsigned int high;
            unsigned int low;
            if (i + 2u >= target_size) return 0;
            if (target[i + 1u] >= '0' && target[i + 1u] <= '9')
                high = target[i + 1u] - '0';
            else if (target[i + 1u] >= 'a' && target[i + 1u] <= 'f')
                high = target[i + 1u] - 'a' + 10u;
            else if (target[i + 1u] >= 'A' && target[i + 1u] <= 'F')
                high = target[i + 1u] - 'A' + 10u;
            else return 0;
            if (target[i + 2u] >= '0' && target[i + 2u] <= '9')
                low = target[i + 2u] - '0';
            else if (target[i + 2u] >= 'a' && target[i + 2u] <= 'f')
                low = target[i + 2u] - 'a' + 10u;
            else if (target[i + 2u] >= 'A' && target[i + 2u] <= 'F')
                low = target[i + 2u] - 'A' + 10u;
            else return 0;
            value = (uint8_t)((high << 4) | low);
            i += 2u;
        }
        if (value == 0 || value == '\\' || value == ':' ||
            value == '?' || value == '#') return 0;
        if (decoded_size >= sizeof(decoded)) return 0;
        decoded[decoded_size++] = (char)value;
    }

    position = 0;
    while (position <= decoded_size) {
        size_t start = position;
        size_t segment_size;
        while (position < decoded_size && decoded[position] != '/') ++position;
        segment_size = position - start;
        if (segment_size == 0 ||
            (segment_size == 1u && decoded[start] == '.')) {
            /* Empty and current-directory segments do not change the path. */
        } else if (segment_size == 2u && decoded[start] == '.' &&
                   decoded[start + 1u] == '.') {
            if (segment_count == 0) return 0;
            normalized_size = restore_points[--segment_count];
        } else {
            if (segment_count >= sizeof(restore_points) / sizeof(restore_points[0])) return 0;
            restore_points[segment_count++] = normalized_size;
            if (normalized_size != 0) {
                if (normalized_size >= sizeof(normalized)) return 0;
                normalized[normalized_size++] = '/';
            }
            if (segment_size > sizeof(normalized) - normalized_size) return 0;
            memcpy(normalized + normalized_size, decoded + start, segment_size);
            normalized_size += segment_size;
        }
        if (position == decoded_size) break;
        ++position;
    }
    if (normalized_size == 0 || normalized_size + 1u > path_capacity) return 0;
    memcpy(path, normalized, normalized_size);
    path[normalized_size] = '\0';
    return 1;
}

static int docx_find_image_target(const uint8_t *relationships,
                                  size_t relationships_size,
                                  const uint8_t *relationship_id,
                                  size_t relationship_id_size,
                                  char *path,
                                  size_t path_capacity)
{
    size_t i = 0;
    if (!relationships || !relationship_id || relationship_id_size == 0) return 0;
    while (i < relationships_size) {
        size_t name_start;
        size_t name_end;
        size_t tag_end;
        int closing = 0;
        if (relationships[i] != '<') {
            ++i;
            continue;
        }
        if (i + 3u < relationships_size &&
            memcmp(relationships + i, "<!--", 4u) == 0) {
            i += 4u;
            while (i + 2u < relationships_size &&
                   memcmp(relationships + i, "-->", 3u) != 0) ++i;
            i = i + 2u < relationships_size ? i + 3u : relationships_size;
            continue;
        }
        ++i;
        if (i < relationships_size && relationships[i] == '/') {
            closing = 1;
            ++i;
        }
        name_start = i;
        while (i < relationships_size && relationships[i] != '>' &&
               relationships[i] != '/' && relationships[i] != ' ' &&
               relationships[i] != '\t' && relationships[i] != '\r' &&
               relationships[i] != '\n') ++i;
        name_end = i;
        if (name_end == name_start) {
            ++i;
            continue;
        }
        tag_end = i;
        {
            uint8_t quote = 0;
            while (tag_end < relationships_size) {
                uint8_t ch = relationships[tag_end++];
                if (quote) {
                    if (ch == quote) quote = 0;
                } else if (ch == '\'' || ch == '"') {
                    quote = ch;
                } else if (ch == '>') {
                    break;
                }
            }
        }
        if (tag_end > relationships_size || relationships[tag_end - 1u] != '>') return 0;
        if (!closing && xml_local_name_is(relationships + name_start,
                                          name_end - name_start, "Relationship")) {
            const uint8_t *id_value;
            const uint8_t *target_value;
            const uint8_t *type_value;
            const uint8_t *mode_value;
            size_t id_size;
            size_t target_size;
            size_t type_size;
            size_t mode_size;
            if (xml_attribute_value(relationships, name_end, tag_end - 1u,
                                    "Id", &id_value, &id_size) &&
                id_size == relationship_id_size &&
                memcmp(id_value, relationship_id, id_size) == 0 &&
                xml_attribute_value(relationships, name_end, tag_end - 1u,
                                    "Target", &target_value, &target_size) &&
                xml_attribute_value(relationships, name_end, tag_end - 1u,
                                    "Type", &type_value, &type_size) &&
                type_size >= 6u &&
                memcmp(type_value + type_size - 6u, "/image", 6u) == 0) {
                if (xml_attribute_value(relationships, name_end, tag_end - 1u,
                                        "TargetMode", &mode_value, &mode_size) &&
                    mode_size == 8u &&
                    memcmp(mode_value, "External", 8u) == 0) return 0;
                return docx_resolve_relationship_target(target_value, target_size,
                                                        path, path_capacity);
            }
        }
        i = tag_end;
    }
    return 0;
}

static int docx_ascii_prefix_equal(const uint8_t *text, size_t text_size,
                                   const char *prefix)
{
    size_t i;
    size_t prefix_size = strlen(prefix);
    if (!text || text_size < prefix_size) return 0;
    for (i = 0; i < prefix_size; ++i) {
        uint8_t ch = text[i];
        if (ch >= 'A' && ch <= 'Z') ch = (uint8_t)(ch - 'A' + 'a');
        if (ch != (uint8_t)prefix[i]) return 0;
    }
    return 1;
}

static int docx_external_uri_is_safe(const uint8_t *target, size_t target_size)
{
    size_t scheme_size;
    size_t i;
    int mailto = 0;
    if (!target || target_size == 0) return 0;
    if (docx_ascii_prefix_equal(target, target_size, "https://")) {
        scheme_size = 8u;
    } else if (docx_ascii_prefix_equal(target, target_size, "http://")) {
        scheme_size = 7u;
    } else if (docx_ascii_prefix_equal(target, target_size, "mailto:")) {
        scheme_size = 7u;
        mailto = 1;
    } else {
        return 0;
    }
    if (target_size <= scheme_size) return 0;
    for (i = 0; i < target_size; ++i) {
        uint8_t ch = target[i];
        if (ch <= 0x20u || ch == 0x7fu || ch == '"' || ch == '<' ||
            ch == '>' || ch == '\\') return 0;
        if (ch == '&') {
            if (i + 5u > target_size ||
                memcmp(target + i, "&amp;", 5u) != 0) return 0;
            i += 4u;
        }
    }
    if (!mailto) {
        size_t authority_end = scheme_size;
        while (authority_end < target_size && target[authority_end] != '/' &&
               target[authority_end] != '?' && target[authority_end] != '#') {
            ++authority_end;
        }
        if (authority_end == scheme_size) return 0;
    }
    return 1;
}

static int docx_find_hyperlink_target(const uint8_t *relationships,
                                      size_t relationships_size,
                                      const uint8_t *relationship_id,
                                      size_t relationship_id_size,
                                      const uint8_t **target,
                                      size_t *target_size)
{
    size_t i = 0;
    if (!relationships || !relationship_id || relationship_id_size == 0 ||
        !target || !target_size) return 0;
    while (i < relationships_size) {
        size_t name_start;
        size_t name_end;
        size_t tag_end;
        int closing = 0;
        if (relationships[i] != '<') {
            ++i;
            continue;
        }
        if (i + 3u < relationships_size &&
            memcmp(relationships + i, "<!--", 4u) == 0) {
            i += 4u;
            while (i + 2u < relationships_size &&
                   memcmp(relationships + i, "-->", 3u) != 0) ++i;
            i = i + 2u < relationships_size ? i + 3u : relationships_size;
            continue;
        }
        ++i;
        if (i < relationships_size && relationships[i] == '/') {
            closing = 1;
            ++i;
        }
        name_start = i;
        while (i < relationships_size && relationships[i] != '>' &&
               relationships[i] != '/' && relationships[i] != ' ' &&
               relationships[i] != '\t' && relationships[i] != '\r' &&
               relationships[i] != '\n') ++i;
        name_end = i;
        if (name_end == name_start) {
            ++i;
            continue;
        }
        tag_end = i;
        {
            uint8_t quote = 0;
            while (tag_end < relationships_size) {
                uint8_t ch = relationships[tag_end++];
                if (quote) {
                    if (ch == quote) quote = 0;
                } else if (ch == '\'' || ch == '"') {
                    quote = ch;
                } else if (ch == '>') {
                    break;
                }
            }
        }
        if (tag_end > relationships_size || relationships[tag_end - 1u] != '>')
            return 0;
        if (!closing && xml_local_name_is(relationships + name_start,
                                          name_end - name_start,
                                          "Relationship")) {
            const uint8_t *id_value;
            const uint8_t *target_value;
            const uint8_t *type_value;
            const uint8_t *mode_value;
            size_t id_size;
            size_t target_value_size;
            size_t type_size;
            size_t mode_size;
            static const char hyperlink_type[] = "/hyperlink";
            if (xml_attribute_value(relationships, name_end, tag_end - 1u,
                                    "Id", &id_value, &id_size) &&
                id_size == relationship_id_size &&
                memcmp(id_value, relationship_id, id_size) == 0 &&
                xml_attribute_value(relationships, name_end, tag_end - 1u,
                                    "Target", &target_value,
                                    &target_value_size) &&
                xml_attribute_value(relationships, name_end, tag_end - 1u,
                                    "Type", &type_value, &type_size) &&
                type_size >= sizeof(hyperlink_type) - 1u &&
                memcmp(type_value + type_size - (sizeof(hyperlink_type) - 1u),
                       hyperlink_type, sizeof(hyperlink_type) - 1u) == 0 &&
                xml_attribute_value(relationships, name_end, tag_end - 1u,
                                    "TargetMode", &mode_value, &mode_size) &&
                mode_size == 8u && memcmp(mode_value, "External", 8u) == 0 &&
                docx_external_uri_is_safe(target_value, target_value_size)) {
                *target = target_value;
                *target_size = target_value_size;
                return 1;
            }
        }
        i = tag_end;
    }
    return 0;
}

enum {
    DOCX_BOOKMARK_NAME_MAX = 256,
    DOCX_BOOKMARK_ID_CAP = 6 + DOCX_BOOKMARK_NAME_MAX * 2 + 1
};

static int docx_make_bookmark_id(const uint8_t *name, size_t name_size,
                                 char *id, size_t id_capacity)
{
    static const char hex[] = "0123456789abcdef";
    static const char prefix[] = "qw-bm-";
    size_t prefix_size = sizeof(prefix) - 1u;
    size_t i;
    if (!name || name_size == 0 || name_size > DOCX_BOOKMARK_NAME_MAX ||
        !id || id_capacity <= prefix_size ||
        name_size > (id_capacity - prefix_size - 1u) / 2u) return 0;
    memcpy(id, prefix, prefix_size);
    for (i = 0; i < name_size; ++i) {
        id[prefix_size + i * 2u] = hex[name[i] >> 4];
        id[prefix_size + i * 2u + 1u] = hex[name[i] & 0x0fu];
    }
    id[prefix_size + name_size * 2u] = '\0';
    return 1;
}

static int docx_append_bookmark_target(QwDocxHtml *html,
                                       const uint8_t *name,
                                       size_t name_size)
{
    char id[DOCX_BOOKMARK_ID_CAP];
    size_t id_size;
    if (!docx_make_bookmark_id(name, name_size, id, sizeof(id))) return 0;
    id_size = sizeof("qw-bm-") - 1u + name_size * 2u;
    if (!html_literal(html, "<span id=\"") ||
        !html_append(html, id, id_size) ||
        !html_literal(html, "\"></span>")) return -1;
    return 1;
}

static int docx_append_hyperlink_start(QwDocxHtml *html,
                                       const uint8_t *relationships,
                                       size_t relationships_size,
                                       const uint8_t *relationship_id,
                                       size_t relationship_id_size,
                                       const uint8_t *bookmark_name,
                                       size_t bookmark_name_size)
{
    const uint8_t *target;
    size_t target_size;
    char bookmark_id[DOCX_BOOKMARK_ID_CAP];
    size_t bookmark_id_size;
    if (docx_make_bookmark_id(bookmark_name, bookmark_name_size,
                              bookmark_id, sizeof(bookmark_id))) {
        bookmark_id_size = sizeof("qw-bm-") - 1u + bookmark_name_size * 2u;
        if (!html_literal(html, "<a href=\"#") ||
            !html_append(html, bookmark_id, bookmark_id_size) ||
            !html_literal(html, "\">")) return -1;
        return 1;
    }
    if (!docx_find_hyperlink_target(relationships, relationships_size,
                                    relationship_id, relationship_id_size,
                                    &target, &target_size)) return 0;
    if (!html_literal(html, "<a href=\"") ||
        !html_append(html, (const char *)target, target_size) ||
        !html_literal(html, "\">")) return -1;
    return 1;
}

static const char *docx_image_mime_type(const char *path)
{
    const char *extension = strrchr(path, '.');
    if (!extension) return NULL;
    if (docx_ascii_equal(extension, ".jpg") ||
        docx_ascii_equal(extension, ".jpeg")) return "image/jpeg";
    if (docx_ascii_equal(extension, ".png")) return "image/png";
    if (docx_ascii_equal(extension, ".gif")) return "image/gif";
    if (docx_ascii_equal(extension, ".bmp") ||
        docx_ascii_equal(extension, ".dib")) return "image/bmp";
    if (docx_ascii_equal(extension, ".tif") ||
        docx_ascii_equal(extension, ".tiff")) return "image/tiff";
    if (docx_ascii_equal(extension, ".svg")) return "image/svg+xml";
    return NULL;
}

static int html_append_base64(QwDocxHtml *html, const uint8_t *data, size_t size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char output[4096];
    size_t input_offset = 0;
    size_t output_size = 0;
    while (input_offset < size) {
        size_t remaining = size - input_offset;
        size_t count = remaining >= 3u ? 3u : remaining;
        uint32_t value = (uint32_t)data[input_offset] << 16;
        if (count > 1u) value |= (uint32_t)data[input_offset + 1u] << 8;
        if (count > 2u) value |= data[input_offset + 2u];
        if (output_size + 4u > sizeof(output)) {
            if (!html_append(html, output, output_size)) return 0;
            output_size = 0;
        }
        output[output_size++] = alphabet[(value >> 18) & 0x3fu];
        output[output_size++] = alphabet[(value >> 12) & 0x3fu];
        output[output_size++] = count > 1u ? alphabet[(value >> 6) & 0x3fu] : '=';
        output[output_size++] = count > 2u ? alphabet[value & 0x3fu] : '=';
        input_offset += count;
    }
    return output_size == 0 || html_append(html, output, output_size);
}

static int docx_append_image(QwDocxHtml *html,
                             QwZipArchive *archive,
                             const uint8_t *relationships,
                             size_t relationships_size,
                             const uint8_t *relationship_id,
                             size_t relationship_id_size,
                             uint32_t width_px,
                             size_t *embedded_size)
{
    char path[2048];
    char width_attribute[32];
    int file_index;
    size_t image_size = 0;
    size_t embed_size = 0;
    size_t total_size;
    void *image_data;
    uint8_t *converted_data = NULL;
    const uint8_t *embed_data;
    QwZipEntryInfo file_stat;
    const char *mime_type;
    if (!archive ||
        !docx_find_image_target(relationships, relationships_size,
                                relationship_id, relationship_id_size,
                                path, sizeof(path))) return 0;
    mime_type = docx_image_mime_type(path);
    if (!mime_type) return 0;
    file_index = qw_zip_reader_locate_file(archive, path, NULL,
                                           QW_ZIP_CASE_SENSITIVE);
    if (file_index < 0 ||
        !qw_zip_reader_is_file_supported(archive, (size_t)file_index) ||
        !qw_zip_reader_file_stat(archive, (size_t)file_index, &file_stat) ||
        file_stat.uncompressed_size == 0 ||
        file_stat.uncompressed_size > DOCX_MAX_IMAGE_SIZE ||
        file_stat.uncompressed_size > SIZE_MAX) return 0;
    total_size = embedded_size ? *embedded_size : 0;
    if (total_size > DOCX_MAX_TOTAL_IMAGE_SIZE - (size_t)file_stat.uncompressed_size) return 0;
    image_data = qw_zip_reader_extract_to_heap(archive, (size_t)file_index,
                                               &image_size, 0);
    if (!image_data || image_size != file_stat.uncompressed_size) {
        free(image_data);
        return 0;
    }

    embed_data = (const uint8_t *)image_data;
    embed_size = image_size;
#ifndef QUIREWISE_STANDALONE_DOCX_PARSER
    if (strcmp(mime_type, "image/svg+xml") == 0) {
        if (qw_svg_to_png((const uint8_t *)image_data, image_size,
                          &converted_data, &embed_size, NULL) != 0 ||
            !converted_data || embed_size == 0) {
            free(image_data);
            free(converted_data);
            return 0;
        }
        embed_data = converted_data;
        mime_type = "image/png";
    }
#endif
    if (embed_size > DOCX_MAX_IMAGE_SIZE ||
        total_size > DOCX_MAX_TOTAL_IMAGE_SIZE - embed_size) {
        free(image_data);
        free(converted_data);
        return 0;
    }

    if (!html_literal(html, "<img src=\"data:") ||
        !html_literal(html, mime_type) ||
        !html_literal(html, ";base64,") ||
        !html_append_base64(html, embed_data, embed_size) ||
        !html_literal(html, "\"")) {
        free(image_data);
        free(converted_data);
        return -1;
    }
    if (width_px > 0 && width_px <= DOCX_MAX_HTML_IMAGE_WIDTH) {
        (void)snprintf(width_attribute, sizeof(width_attribute),
                       " width=\"%u\"", width_px);
        if (!html_literal(html, width_attribute)) {
            free(image_data);
            free(converted_data);
            return -1;
        }
    }
    if (!html_literal(html, ">")) {
        free(image_data);
        free(converted_data);
        return -1;
    }
    if (embedded_size) *embedded_size = total_size + embed_size;
    free(image_data);
    free(converted_data);
    return 1;
}

static uint32_t docx_emu_width_to_pixels(const uint8_t *value, size_t size)
{
    uint64_t emu = 0;
    size_t i;
    for (i = 0; i < size; ++i) {
        unsigned int digit;
        if (value[i] < '0' || value[i] > '9') return 0;
        digit = value[i] - '0';
        if (emu > (UINT64_MAX - digit) / 10u) return 0;
        emu = emu * 10u + digit;
    }
    if (size == 0) return 0;
    emu = (emu + 4762u) / 9525u;
    if (emu == 0) return 0;
    if (emu > DOCX_MAX_HTML_IMAGE_WIDTH) return DOCX_MAX_HTML_IMAGE_WIDTH;
    return (uint32_t)emu;
}

enum {
    DOCX_NUMBERING_RECORD_LIMIT = 256,
    DOCX_NUMBERING_LEVEL_COUNT = 9,
    DOCX_STYLE_RECORD_LIMIT = 256,
    DOCX_STYLE_ID_CAPACITY = 128,
    DOCX_MAX_STYLES_XML_SIZE = 16 * 1024 * 1024,
    DOCX_LIST_FORMAT_NONE = 0,
    DOCX_LIST_FORMAT_BULLET = 1,
    DOCX_LIST_FORMAT_DECIMAL = 2,
    DOCX_LIST_FORMAT_LOWER_LETTER = 3,
    DOCX_LIST_FORMAT_UPPER_LETTER = 4,
    DOCX_LIST_FORMAT_LOWER_ROMAN = 5,
    DOCX_LIST_FORMAT_UPPER_ROMAN = 6
};

typedef struct QwDocxXmlTag {
    size_t name_start;
    size_t name_size;
    size_t tag_end;
    int closing;
} QwDocxXmlTag;

typedef struct QwDocxAbstractNumber {
    uint32_t id;
    uint8_t formats[DOCX_NUMBERING_LEVEL_COUNT];
    int used;
} QwDocxAbstractNumber;

typedef struct QwDocxNumberInstance {
    uint32_t id;
    uint32_t abstract_id;
    int used;
} QwDocxNumberInstance;

typedef struct QwDocxNumbering {
    QwDocxAbstractNumber abstracts[DOCX_NUMBERING_RECORD_LIMIT];
    QwDocxNumberInstance instances[DOCX_NUMBERING_RECORD_LIMIT];
    size_t abstract_count;
    size_t instance_count;
} QwDocxNumbering;

typedef struct QwDocxStyleNumbering {
    char id[DOCX_STYLE_ID_CAPACITY];
    char based_on[DOCX_STYLE_ID_CAPACITY];
    uint32_t num_id;
    uint32_t level;
    int has_num_id;
} QwDocxStyleNumbering;

typedef struct QwDocxStyles {
    QwDocxStyleNumbering entries[DOCX_STYLE_RECORD_LIMIT];
    size_t count;
} QwDocxStyles;

typedef struct QwDocxListState {
    uint32_t num_ids[DOCX_NUMBERING_LEVEL_COUNT];
    uint8_t formats[DOCX_NUMBERING_LEVEL_COUNT];
    uint8_t lists_open[DOCX_NUMBERING_LEVEL_COUNT];
    uint8_t items_open[DOCX_NUMBERING_LEVEL_COUNT];
    size_t depth;
} QwDocxListState;

static int docx_xml_next_tag(const uint8_t *xml, size_t size, size_t *offset,
                             QwDocxXmlTag *tag)
{
    size_t i = *offset;
    while (i < size) {
        size_t tag_end;
        if (xml[i] != '<') {
            ++i;
            continue;
        }
        if (i + 3u < size && memcmp(xml + i, "<!--", 4u) == 0) {
            i += 4u;
            while (i + 2u < size && memcmp(xml + i, "-->", 3u) != 0) ++i;
            i = i + 2u < size ? i + 3u : size;
            continue;
        }
        ++i;
        if (i < size && (xml[i] == '?' || xml[i] == '!')) {
            while (i < size && xml[i] != '>') ++i;
            if (i < size) ++i;
            continue;
        }
        memset(tag, 0, sizeof(*tag));
        if (i < size && xml[i] == '/') {
            tag->closing = 1;
            ++i;
        }
        tag->name_start = i;
        while (i < size && xml[i] != '>' && xml[i] != '/' &&
               xml[i] != ' ' && xml[i] != '\t' && xml[i] != '\r' &&
               xml[i] != '\n') ++i;
        tag->name_size = i - tag->name_start;
        if (tag->name_size == 0) {
            ++i;
            continue;
        }
        tag_end = i;
        {
            uint8_t quote = 0;
            while (tag_end < size) {
                uint8_t ch = xml[tag_end++];
                if (quote) {
                    if (ch == quote) quote = 0;
                } else if (ch == '\'' || ch == '"') {
                    quote = ch;
                } else if (ch == '>') {
                    break;
                }
            }
        }
        if (tag_end > size || xml[tag_end - 1u] != '>') return -1;
        tag->tag_end = tag_end;
        *offset = tag_end;
        return 1;
    }
    *offset = size;
    return 0;
}

static int docx_parse_u32(const uint8_t *value, size_t size, uint32_t *out)
{
    uint32_t parsed = 0;
    size_t i;
    if (!value || !out || size == 0) return 0;
    for (i = 0; i < size; ++i) {
        unsigned int digit;
        if (value[i] < '0' || value[i] > '9') return 0;
        digit = value[i] - '0';
        if (parsed > (UINT32_MAX - digit) / 10u) return 0;
        parsed = parsed * 10u + digit;
    }
    *out = parsed;
    return 1;
}

static uint8_t docx_numbering_format(const uint8_t *value, size_t size)
{
    if (size == 6u && memcmp(value, "bullet", 6u) == 0)
        return DOCX_LIST_FORMAT_BULLET;
    if ((size == 7u && memcmp(value, "decimal", 7u) == 0) ||
        (size == 11u && memcmp(value, "decimalZero", 11u) == 0))
        return DOCX_LIST_FORMAT_DECIMAL;
    if (size == 11u && memcmp(value, "lowerLetter", 11u) == 0)
        return DOCX_LIST_FORMAT_LOWER_LETTER;
    if (size == 11u && memcmp(value, "upperLetter", 11u) == 0)
        return DOCX_LIST_FORMAT_UPPER_LETTER;
    if (size == 10u && memcmp(value, "lowerRoman", 10u) == 0)
        return DOCX_LIST_FORMAT_LOWER_ROMAN;
    if (size == 10u && memcmp(value, "upperRoman", 10u) == 0)
        return DOCX_LIST_FORMAT_UPPER_ROMAN;
    return DOCX_LIST_FORMAT_NONE;
}

static int docx_get_attribute_u32(const uint8_t *xml,
                                  const QwDocxXmlTag *tag,
                                  const char *attribute,
                                  uint32_t *value)
{
    const uint8_t *text;
    size_t text_size;
    return xml_attribute_value(xml, tag->name_start + tag->name_size,
                               tag->tag_end - 1u, attribute,
                               &text, &text_size) &&
            docx_parse_u32(text, text_size, value);
}

static int docx_xml_tag_self_closing(const uint8_t *xml,
                                     const QwDocxXmlTag *tag);

static void docx_page_layout_defaults(QwDocxPageLayout *layout)
{
    memset(layout, 0, sizeof(*layout));
    layout->width_twips = 11900u;
    layout->height_twips = 16840u;
    layout->top_twips = 1134u;
    layout->right_twips = 1134u;
    layout->bottom_twips = 1134u;
    layout->left_twips = 1134u;
}

static int docx_page_layout_equal(const QwDocxPageLayout *a,
                                 const QwDocxPageLayout *b)
{
    return a->width_twips == b->width_twips &&
           a->height_twips == b->height_twips &&
           a->top_twips == b->top_twips &&
           a->right_twips == b->right_twips &&
           a->bottom_twips == b->bottom_twips &&
           a->left_twips == b->left_twips;
}

static int docx_page_section_properties(const uint8_t *xml,
                                        size_t xml_size,
                                        size_t *cursor,
                                        QwDocxPageLayout *layout)
{
    size_t depth = 1u;
    QwDocxXmlTag tag;
    int next;

    for (;;) {
        next = docx_xml_next_tag(xml, xml_size, cursor, &tag);
        if (next <= 0) return 0;

        if (!tag.closing && depth == 1u &&
            xml_local_name_is(xml + tag.name_start, tag.name_size, "pgSz")) {
            uint32_t width;
            uint32_t height;
            const uint8_t *orientation = NULL;
            size_t orientation_size = 0;
            int is_landscape = 0;
            int is_portrait = 0;

            if (docx_get_attribute_u32(xml, &tag, "w", &width) &&
                docx_get_attribute_u32(xml, &tag, "h", &height) &&
                width >= 1440u && height >= 1440u &&
                width <= 288000u && height <= 288000u) {
                if (xml_attribute_value(xml,
                                        tag.name_start + tag.name_size,
                                        tag.tag_end - 1u, "orient",
                                        &orientation, &orientation_size)) {
                    is_landscape = orientation_size == 9u &&
                                   memcmp(orientation, "landscape", 9u) == 0;
                    is_portrait = orientation_size == 7u &&
                                  memcmp(orientation, "portrait", 7u) == 0;
                }
                if ((is_landscape && width < height) ||
                    (is_portrait && width > height)) {
                    uint32_t temporary = width;
                    width = height;
                    height = temporary;
                }
                layout->width_twips = width;
                layout->height_twips = height;
            }
        } else if (!tag.closing && depth == 1u &&
                   xml_local_name_is(xml + tag.name_start, tag.name_size,
                                     "pgMar")) {
            uint32_t value;
            if (docx_get_attribute_u32(xml, &tag, "top", &value) &&
                value <= 288000u) layout->top_twips = value;
            if (docx_get_attribute_u32(xml, &tag, "right", &value) &&
                value <= 288000u) layout->right_twips = value;
            if (docx_get_attribute_u32(xml, &tag, "bottom", &value) &&
                value <= 288000u) layout->bottom_twips = value;
            if (docx_get_attribute_u32(xml, &tag, "left", &value) &&
                value <= 288000u) layout->left_twips = value;
        }

        if (tag.closing) {
            if (depth == 1u) {
                return xml_local_name_is(xml + tag.name_start,
                                         tag.name_size, "sectPr");
            }
            --depth;
        } else if (!docx_xml_tag_self_closing(xml, &tag)) {
            if (depth >= 64u) return 0;
            ++depth;
        }
    }
}

static int docx_parse_page_layout(const uint8_t *xml,
                                  size_t xml_size,
                                  QwDocxPageLayout *out_layout,
                                  int *out_mixed_sections)
{
    size_t cursor = 0;
    size_t section_count = 0;
    QwDocxXmlTag tag;
    QwDocxPageLayout current;
    QwDocxPageLayout first;
    int next;

    if (!xml || !out_layout || !out_mixed_sections) return 0;
    docx_page_layout_defaults(&current);
    first = current;
    *out_mixed_sections = 0;

    for (;;) {
        next = docx_xml_next_tag(xml, xml_size, &cursor, &tag);
        if (next < 0) return 0;
        if (next == 0) break;
        if (tag.closing ||
            !xml_local_name_is(xml + tag.name_start, tag.name_size, "sectPr")) {
            continue;
        }

        if (!docx_xml_tag_self_closing(xml, &tag) &&
            !docx_page_section_properties(xml, xml_size, &cursor, &current)) {
            return 0;
        }
        if (section_count == 0u) first = current;
        else if (!docx_page_layout_equal(&first, &current))
            *out_mixed_sections = 1;
        ++section_count;
    }

    *out_layout = first;
    return 1;
}

static int docx_append_page_rule(QwDocxHtml *html,
                                 const QwDocxPageLayout *layout)
{
    char rule[192];
    uint64_t top = (uint64_t)layout->top_twips * 5u;
    uint64_t right = (uint64_t)layout->right_twips * 5u;
    uint64_t bottom = (uint64_t)layout->bottom_twips * 5u;
    uint64_t left = (uint64_t)layout->left_twips * 5u;
    int length = snprintf(rule, sizeof(rule),
                          "@page{margin:%lu.%02lupt %lu.%02lupt %lu.%02lupt %lu.%02lupt}",
                          (unsigned long)(top / 100u),
                          (unsigned long)(top % 100u),
                          (unsigned long)(right / 100u),
                          (unsigned long)(right % 100u),
                          (unsigned long)(bottom / 100u),
                          (unsigned long)(bottom % 100u),
                          (unsigned long)(left / 100u),
                          (unsigned long)(left % 100u));
    return length >= 0 && (size_t)length < sizeof(rule) &&
           html_append(html, rule, (size_t)length);
}

static int docx_xml_tag_self_closing(const uint8_t *xml,
                                     const QwDocxXmlTag *tag)
{
    size_t cursor;
    if (!xml || !tag || tag->tag_end == 0) return 0;
    cursor = tag->tag_end - 1u;
    while (cursor > tag->name_start &&
           (xml[cursor - 1u] == ' ' || xml[cursor - 1u] == '\t' ||
            xml[cursor - 1u] == '\r' || xml[cursor - 1u] == '\n')) {
        --cursor;
    }
    return cursor > tag->name_start && xml[cursor - 1u] == '/';
}

static int docx_cell_properties(const uint8_t *xml,
                                size_t xml_size,
                                size_t content_offset,
                                uint32_t *out_span,
                                char out_fill[7],
                                char out_vertical_align[7])
{
    size_t cursor = content_offset;
    size_t depth = 1u;
    QwDocxXmlTag tag;
    int next;

    if (!xml || !out_span || !out_fill || !out_vertical_align ||
        content_offset > xml_size) return 0;
    *out_span = 1u;
    out_fill[0] = '\0';
    out_vertical_align[0] = '\0';
    next = docx_xml_next_tag(xml, xml_size, &cursor, &tag);
    if (next <= 0 || tag.closing ||
        !xml_local_name_is(xml + tag.name_start, tag.name_size, "tcPr")) {
        return 1;
    }
    if (docx_xml_tag_self_closing(xml, &tag)) return 1;

    for (;;) {
        next = docx_xml_next_tag(xml, xml_size, &cursor, &tag);
        if (next <= 0) return 0;

        if (!tag.closing && depth == 1u &&
            xml_local_name_is(xml + tag.name_start, tag.name_size,
                              "gridSpan")) {
            if (!docx_get_attribute_u32(xml, &tag, "val", out_span) ||
                *out_span == 0u) {
                return 0;
            }
        } else if (!tag.closing && depth == 1u &&
                   xml_local_name_is(xml + tag.name_start, tag.name_size,
                                     "shd")) {
            const uint8_t *value = NULL;
            const uint8_t *fill = NULL;
            size_t value_size = 0;
            size_t fill_size = 0;
            size_t i;
            int has_value = xml_attribute_value(
                xml, tag.name_start + tag.name_size, tag.tag_end - 1u,
                "val", &value, &value_size);
            int has_fill = xml_attribute_value(
                xml, tag.name_start + tag.name_size, tag.tag_end - 1u,
                "fill", &fill, &fill_size);

            if (has_fill && fill_size == 6u &&
                (!has_value || (value_size == 5u &&
                                memcmp(value, "clear", 5u) == 0))) {
                for (i = 0; i < fill_size; ++i) {
                    uint8_t ch = fill[i];
                    if (!((ch >= '0' && ch <= '9') ||
                          (ch >= 'A' && ch <= 'F') ||
                          (ch >= 'a' && ch <= 'f'))) break;
                }
                if (i == fill_size) {
                    memcpy(out_fill, fill, fill_size);
                    out_fill[fill_size] = '\0';
                }
            }
        } else if (!tag.closing && depth == 1u &&
                   xml_local_name_is(xml + tag.name_start, tag.name_size,
                                     "vAlign")) {
            const uint8_t *value = NULL;
            size_t value_size = 0;
            if (xml_attribute_value(xml, tag.name_start + tag.name_size,
                                    tag.tag_end - 1u, "val",
                                    &value, &value_size)) {
                if (value_size == 3u && memcmp(value, "top", 3u) == 0) {
                    memcpy(out_vertical_align, "top", 4u);
                } else if (value_size == 6u &&
                           memcmp(value, "center", 6u) == 0) {
                    memcpy(out_vertical_align, "middle", 7u);
                } else if (value_size == 6u &&
                           memcmp(value, "bottom", 6u) == 0) {
                    memcpy(out_vertical_align, "bottom", 7u);
                }
            }
        }

        if (tag.closing) {
            if (depth == 1u) {
                return xml_local_name_is(xml + tag.name_start, tag.name_size,
                                         "tcPr");
            }
            --depth;
        } else if (!docx_xml_tag_self_closing(xml, &tag)) {
            if (depth >= 64u) return 0;
            ++depth;
        }
    }
}

static size_t docx_number_find_abstract(QwDocxNumbering *numbering,
                                        uint32_t id,
                                       int create)
{
    size_t i;
    for (i = 0; i < numbering->abstract_count; ++i) {
        if (numbering->abstracts[i].id == id) return i;
    }
    if (!create || numbering->abstract_count >= DOCX_NUMBERING_RECORD_LIMIT) return SIZE_MAX;
    i = numbering->abstract_count++;
    memset(&numbering->abstracts[i], 0, sizeof(numbering->abstracts[i]));
    numbering->abstracts[i].id = id;
    numbering->abstracts[i].used = 1;
    return i;
}

static size_t docx_number_find_instance(QwDocxNumbering *numbering,
                                        uint32_t id,
                                        int create)
{
    size_t i;
    for (i = 0; i < numbering->instance_count; ++i) {
        if (numbering->instances[i].id == id) return i;
    }
    if (!create || numbering->instance_count >= DOCX_NUMBERING_RECORD_LIMIT) return SIZE_MAX;
    i = numbering->instance_count++;
    memset(&numbering->instances[i], 0, sizeof(numbering->instances[i]));
    numbering->instances[i].id = id;
    numbering->instances[i].used = 1;
    return i;
}

static int docx_parse_numbering(const uint8_t *xml,
                               size_t xml_size,
                               QwDocxNumbering *numbering)
{
    size_t offset = 0;
    size_t current_abstract = SIZE_MAX;
    size_t current_instance = SIZE_MAX;
    size_t current_level = SIZE_MAX;
    QwDocxXmlTag tag;
    int next;
    if (!numbering) return 0;
    memset(numbering, 0, sizeof(*numbering));
    if (!xml || xml_size == 0) return 1;
    while ((next = docx_xml_next_tag(xml, xml_size, &offset, &tag)) > 0) {
        if (xml_local_name_is(xml + tag.name_start, tag.name_size, "abstractNum")) {
            if (tag.closing) {
                current_abstract = SIZE_MAX;
                current_level = SIZE_MAX;
            } else {
                uint32_t id;
                current_abstract = docx_get_attribute_u32(xml, &tag,
                                                          "abstractNumId", &id)
                    ? docx_number_find_abstract(numbering, id, 1) : SIZE_MAX;
                current_level = SIZE_MAX;
            }
        } else if (xml_local_name_is(xml + tag.name_start, tag.name_size, "num")) {
            if (tag.closing) {
                current_instance = SIZE_MAX;
            } else {
                uint32_t id;
                current_instance = docx_get_attribute_u32(xml, &tag, "numId", &id)
                    ? docx_number_find_instance(numbering, id, 1) : SIZE_MAX;
            }
        } else if (xml_local_name_is(xml + tag.name_start, tag.name_size, "lvl")) {
            if (tag.closing) {
                current_level = SIZE_MAX;
            } else if (current_abstract != SIZE_MAX) {
                uint32_t level;
                current_level = docx_get_attribute_u32(xml, &tag, "ilvl", &level) &&
                                level < DOCX_NUMBERING_LEVEL_COUNT
                    ? (size_t)level : SIZE_MAX;
            }
        } else if (!tag.closing && current_abstract != SIZE_MAX &&
                   current_level != SIZE_MAX &&
                   xml_local_name_is(xml + tag.name_start, tag.name_size, "numFmt")) {
            const uint8_t *value;
            size_t value_size;
            if (xml_attribute_value(xml, tag.name_start + tag.name_size,
                                    tag.tag_end - 1u, "val", &value, &value_size)) {
                numbering->abstracts[current_abstract].formats[current_level] =
                    docx_numbering_format(value, value_size);
            }
        } else if (!tag.closing && current_instance != SIZE_MAX &&
                   xml_local_name_is(xml + tag.name_start, tag.name_size,
                                     "abstractNumId")) {
            uint32_t id;
            if (docx_get_attribute_u32(xml, &tag, "val", &id)) {
                numbering->instances[current_instance].abstract_id = id;
            }
        }
        if (next < 0) return 0;
    }
    return next == 0;
}

static uint8_t docx_lookup_numbering_format(const QwDocxNumbering *numbering,
                                           uint32_t num_id,
                                           uint32_t level)
{
    size_t i;
    uint32_t abstract_id = 0;
    if (!numbering || num_id == 0) return DOCX_LIST_FORMAT_NONE;
    if (level >= DOCX_NUMBERING_LEVEL_COUNT) level = DOCX_NUMBERING_LEVEL_COUNT - 1u;
    for (i = 0; i < numbering->instance_count; ++i) {
        if (numbering->instances[i].used && numbering->instances[i].id == num_id) {
            abstract_id = numbering->instances[i].abstract_id;
            break;
        }
    }
    if (i == numbering->instance_count) return DOCX_LIST_FORMAT_NONE;
    for (i = 0; i < numbering->abstract_count; ++i) {
        if (numbering->abstracts[i].used && numbering->abstracts[i].id == abstract_id) {
            uint8_t format = numbering->abstracts[i].formats[level];
            if (format == DOCX_LIST_FORMAT_NONE && level != 0)
                format = numbering->abstracts[i].formats[0];
            return format;
        }
    }
    return DOCX_LIST_FORMAT_NONE;
}

static size_t docx_style_find(const QwDocxStyles *styles, const char *id)
{
    if (!styles || !id || id[0] == '\0') return SIZE_MAX;
    for (size_t i = 0; i < styles->count; ++i) {
        if (strcmp(styles->entries[i].id, id) == 0) return i;
    }
    return SIZE_MAX;
}

static size_t docx_style_create(QwDocxStyles *styles,
                                const uint8_t *id,
                                size_t id_size)
{
    size_t index;
    if (!styles || !id || id_size == 0 ||
        id_size >= DOCX_STYLE_ID_CAPACITY ||
        styles->count >= DOCX_STYLE_RECORD_LIMIT) return SIZE_MAX;
    index = styles->count++;
    memset(&styles->entries[index], 0, sizeof(styles->entries[index]));
    memcpy(styles->entries[index].id, id, id_size);
    styles->entries[index].id[id_size] = '\0';
    return index;
}

static int docx_copy_style_reference(char destination[DOCX_STYLE_ID_CAPACITY],
                                     const uint8_t *value,
                                     size_t value_size)
{
    if (!destination || !value || value_size == 0 ||
        value_size >= DOCX_STYLE_ID_CAPACITY) return 0;
    memcpy(destination, value, value_size);
    destination[value_size] = '\0';
    return 1;
}

static int docx_parse_styles(const uint8_t *xml,
                             size_t xml_size,
                             QwDocxStyles *styles)
{
    size_t offset = 0;
    size_t current_style = SIZE_MAX;
    int in_num_pr = 0;
    QwDocxXmlTag tag;
    int next;

    if (!styles) return 0;
    memset(styles, 0, sizeof(*styles));
    if (!xml || xml_size == 0) return 1;
    while ((next = docx_xml_next_tag(xml, xml_size, &offset, &tag)) > 0) {
        if (xml_local_name_is(xml + tag.name_start, tag.name_size, "style")) {
            current_style = SIZE_MAX;
            in_num_pr = 0;
            if (!tag.closing) {
                const uint8_t *style_type;
                const uint8_t *style_id;
                size_t style_type_size;
                size_t style_id_size;
                if (xml_attribute_value(xml, tag.name_start + tag.name_size,
                                        tag.tag_end - 1u, "type",
                                        &style_type, &style_type_size) &&
                    style_type_size == 9u &&
                    memcmp(style_type, "paragraph", 9u) == 0 &&
                    xml_attribute_value(xml, tag.name_start + tag.name_size,
                                        tag.tag_end - 1u, "styleId",
                                        &style_id, &style_id_size)) {
                    current_style = docx_style_create(styles, style_id,
                                                      style_id_size);
                }
            }
        } else if (current_style != SIZE_MAX &&
                   xml_local_name_is(xml + tag.name_start, tag.name_size,
                                     "numPr")) {
            in_num_pr = !tag.closing;
        } else if (!tag.closing && current_style != SIZE_MAX &&
                   xml_local_name_is(xml + tag.name_start, tag.name_size,
                                     "basedOn")) {
            const uint8_t *value;
            size_t value_size;
            if (xml_attribute_value(xml, tag.name_start + tag.name_size,
                                    tag.tag_end - 1u, "val",
                                    &value, &value_size)) {
                (void)docx_copy_style_reference(
                    styles->entries[current_style].based_on,
                    value, value_size);
            }
        } else if (!tag.closing && in_num_pr && current_style != SIZE_MAX &&
                   xml_local_name_is(xml + tag.name_start, tag.name_size,
                                     "numId")) {
            uint32_t num_id;
            if (docx_get_attribute_u32(xml, &tag, "val", &num_id)) {
                styles->entries[current_style].num_id = num_id;
                styles->entries[current_style].has_num_id = 1;
            }
        } else if (!tag.closing && in_num_pr && current_style != SIZE_MAX &&
                   xml_local_name_is(xml + tag.name_start, tag.name_size,
                                     "ilvl")) {
            uint32_t level;
            if (docx_get_attribute_u32(xml, &tag, "val", &level) &&
                level < DOCX_NUMBERING_LEVEL_COUNT) {
                styles->entries[current_style].level = level;
            }
        }
        if (next < 0) return 0;
    }
    return next == 0;
}

static void docx_load_styles(QwZipArchive *archive, QwDocxStyles *styles)
{
    int index;
    QwZipEntryInfo stat;
    size_t xml_size = 0;
    void *xml;

    memset(styles, 0, sizeof(*styles));
    index = qw_zip_reader_locate_file(archive, "word/styles.xml", NULL,
                                      QW_ZIP_CASE_SENSITIVE);
    if (index < 0 ||
        !qw_zip_reader_is_file_supported(archive, (size_t)index) ||
        !qw_zip_reader_file_stat(archive, (size_t)index, &stat) ||
        stat.uncompressed_size == 0 ||
        stat.uncompressed_size > DOCX_MAX_STYLES_XML_SIZE ||
        stat.uncompressed_size > SIZE_MAX) return;

    xml = qw_zip_reader_extract_to_heap(archive, (size_t)index,
                                        &xml_size, 0);
    if (!xml || xml_size != stat.uncompressed_size) {
        free(xml);
        return;
    }
    if (!docx_parse_styles((const uint8_t *)xml, xml_size, styles)) {
        memset(styles, 0, sizeof(*styles));
    }
    free(xml);
}

static int docx_lookup_style_numbering(const QwDocxStyles *styles,
                                       const uint8_t *style_id,
                                       size_t style_id_size,
                                       uint32_t *num_id,
                                       uint32_t *level)
{
    char current_id[DOCX_STYLE_ID_CAPACITY];

    if (!styles || !style_id || !num_id || !level || style_id_size == 0 ||
        style_id_size >= sizeof(current_id)) return 0;
    memcpy(current_id, style_id, style_id_size);
    current_id[style_id_size] = '\0';

    for (size_t depth = 0; depth < 32u; ++depth) {
        size_t index = docx_style_find(styles, current_id);
        const QwDocxStyleNumbering *style;
        if (index == SIZE_MAX) return 0;
        style = &styles->entries[index];
        if (style->has_num_id) {
            *num_id = style->num_id;
            *level = style->level;
            return 1;
        }
        if (style->based_on[0] == '\0') return 0;
        memcpy(current_id, style->based_on, strlen(style->based_on) + 1u);
    }
    return 0;
}

static const char *docx_list_container_tag(uint8_t format)
{
    return format == DOCX_LIST_FORMAT_BULLET ? "ul" : "ol";
}

static const char *docx_ordered_list_type(uint8_t format)
{
    switch (format) {
    case DOCX_LIST_FORMAT_LOWER_LETTER: return "a";
    case DOCX_LIST_FORMAT_UPPER_LETTER: return "A";
    case DOCX_LIST_FORMAT_LOWER_ROMAN: return "i";
    case DOCX_LIST_FORMAT_UPPER_ROMAN: return "I";
    case DOCX_LIST_FORMAT_DECIMAL: return "1";
    default: return NULL;
    }
}

static int docx_close_list_level(QwDocxHtml *html, QwDocxListState *list)
{
    size_t level;
    if (list->depth == 0) return 1;
    level = list->depth - 1u;
    if (list->items_open[level]) {
        if (!html_literal(html, "</li>")) return 0;
        list->items_open[level] = 0;
    }
    if (list->lists_open[level]) {
        if (!html_literal(html, "</") ||
            !html_literal(html, docx_list_container_tag(list->formats[level])) ||
            !html_literal(html, ">")) return 0;
        list->lists_open[level] = 0;
    }
    list->num_ids[level] = 0;
    list->formats[level] = DOCX_LIST_FORMAT_NONE;
    --list->depth;
    return 1;
}

static int docx_close_list(QwDocxHtml *html, QwDocxListState *list)
{
    while (list->depth != 0) {
        if (!docx_close_list_level(html, list)) return 0;
    }
    memset(list, 0, sizeof(*list));
    return 1;
}

static int docx_open_list_container(QwDocxHtml *html,
                                    QwDocxListState *list,
                                    size_t level,
                                    uint32_t num_id,
                                    uint8_t format)
{
    const char *tag = docx_list_container_tag(format);
    const char *ordered_type = docx_ordered_list_type(format);
    if (!html_literal(html, "<") || !html_literal(html, tag)) return 0;
    if (ordered_type && (!html_literal(html, " type=\"") ||
                         !html_literal(html, ordered_type) ||
                         !html_literal(html, "\""))) return 0;
    if (!html_literal(html, ">")) return 0;
    list->num_ids[level] = num_id;
    list->formats[level] = format;
    list->lists_open[level] = 1;
    list->items_open[level] = 0;
    list->depth = level + 1u;
    return 1;
}

static int docx_open_list_item(QwDocxHtml *html,
                               const QwDocxNumbering *numbering,
                               QwDocxListState *list,
                               uint32_t num_id,
                               uint32_t requested_level,
                               uint8_t format,
                               const char *alignment)
{
    size_t level = requested_level;
    if (format == DOCX_LIST_FORMAT_NONE) return 0;
    if (level >= DOCX_NUMBERING_LEVEL_COUNT)
        level = DOCX_NUMBERING_LEVEL_COUNT - 1u;

    while (list->depth > level + 1u) {
        if (!docx_close_list_level(html, list)) return -1;
    }
    if (list->depth == level + 1u &&
        (list->num_ids[level] != num_id || list->formats[level] != format)) {
        if (!docx_close_list_level(html, list)) return -1;
    }
    while (list->depth < level + 1u) {
        size_t next_level = list->depth;
        uint8_t next_format = next_level == level
            ? format
            : docx_lookup_numbering_format(numbering, num_id, (uint32_t)next_level);
        if (next_level != 0 && !list->items_open[next_level - 1u]) {
            if (!html_literal(html, "<li>")) return -1;
            list->items_open[next_level - 1u] = 1;
        }
        if (next_format == DOCX_LIST_FORMAT_NONE) next_format = format;
        if (!docx_open_list_container(html, list, next_level,
                                      num_id, next_format)) return -1;
    }
    if (list->depth != level + 1u || !list->lists_open[level]) return -1;
    if (list->items_open[level] && !html_literal(html, "</li>")) return -1;
    list->items_open[level] = 0;
    if (alignment) {
        char tag[96];
        (void)snprintf(tag, sizeof(tag), "<li style=\"text-align:%s\">", alignment);
        if (!html_literal(html, tag)) return -1;
    } else if (!html_literal(html, "<li>")) {
        return -1;
    }
    list->items_open[level] = 1;
    return 1;
}

static int docx_ensure_plain_paragraph(QwDocxHtml *html,
                                       QwDocxListState *list,
                                       const char *tag,
                                       const char *alignment,
                                       int *rendered)
{
    if (*rendered) return 1;
    if (!docx_close_list(html, list) ||
        !html_open_paragraph(html, tag, alignment)) return 0;
    *rendered = 1;
    return 1;
}

static int docx_document_xml_to_html(const uint8_t *xml, size_t xml_size,
                                     QwZipArchive *archive,
                                     const uint8_t *relationships,
                                     size_t relationships_size,
                                     const QwDocxNumbering *numbering,
                                     const QwDocxStyles *styles,
                                     const QwDocxPageLayout *page_layout,
                                     QwDocxHtml *html,
                                     size_t *omitted_images,
                                     size_t *omitted_list_items)
{
    static const char prefix_start[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><style>";
    static const char prefix_end[] =
        "body{font-family:sans-serif;font-size:11pt}"
        "p{margin:0 0 0.7em 0;white-space:pre-wrap;overflow-wrap:anywhere}"
        "img{max-width:100%;height:auto}"
        "table{border-collapse:collapse;width:100%}td{border:1px solid #999;"
        "vertical-align:top;padding:4px}"
        "</style></head><body>";
    static const char suffix[] = "</body></html>";
    size_t i = 0;
    int in_text = 0;
    int in_hyperlink = 0;
    int in_alternate_content = 0;
    int in_alternate_choice = 0;
    int in_alternate_fallback = 0;
    int alternate_svg_rendered = 0;
    int in_paragraph = 0;
    int paragraph_rendered = 0;
    int paragraph_is_list = 0;
    int paragraph_page_break_before = 0;
    int num_pr_active = 0;
    int paragraph_direct_num_pr = 0;
    int paragraph_style_num_present = 0;
    uint32_t paragraph_num_id = 0;
    uint32_t paragraph_num_level = 0;
    uint32_t paragraph_style_num_id = 0;
    uint32_t paragraph_style_num_level = 0;
    const char *paragraph_tag = "p";
    const char *paragraph_alignment = NULL;
    uint32_t image_width_px = 0;
    size_t embedded_image_size = 0;
    QwDocxListState list = {0};
    int bold = 0;
    int italic = 0;
    int underline = 0;
    int strike = 0;
    if (!page_layout ||
        !html_append(html, prefix_start, sizeof(prefix_start) - 1u) ||
        !docx_append_page_rule(html, page_layout) ||
        !html_append(html, prefix_end, sizeof(prefix_end) - 1u)) return 0;
    while (i < xml_size) {
        if (xml[i] == '<') {
            size_t name_start;
            size_t name_end;
            size_t tag_end;
            int closing = 0;
            int self_closing;
            ++i;
            if (i + 2u < xml_size && memcmp(xml + i, "!--", 3) == 0) {
                i += 3u;
                while (i + 2u < xml_size && memcmp(xml + i, "-->", 3) != 0) ++i;
                i = i + 2u < xml_size ? i + 3u : xml_size;
                continue;
            }
            if (i < xml_size && (xml[i] == '?' || xml[i] == '!')) {
                while (i < xml_size && xml[i] != '>') ++i;
                if (i < xml_size) ++i;
                continue;
            }
            if (i < xml_size && xml[i] == '/') {
                closing = 1;
                ++i;
            }
            name_start = i;
            while (i < xml_size && xml[i] != '>' && xml[i] != '/' &&
                   xml[i] != ' ' && xml[i] != '\t' && xml[i] != '\r' &&
                   xml[i] != '\n') ++i;
            name_end = i;
            if (name_end == name_start) return 0;
            tag_end = i;
            {
                uint8_t quote = 0;
                while (tag_end < xml_size) {
                    uint8_t ch = xml[tag_end++];
                    if (quote) {
                        if (ch == quote) quote = 0;
                    } else if (ch == '\'' || ch == '"') {
                        quote = ch;
                    } else if (ch == '>') {
                        break;
                    }
                }
            }
            if (tag_end > xml_size || xml[tag_end - 1u] != '>') return 0;
            self_closing = tag_end >= 2u && xml[tag_end - 2u] == '/';
            if (xml_local_name_is(xml + name_start,
                                  name_end - name_start, "AlternateContent")) {
                if (closing) {
                    in_alternate_content = 0;
                    in_alternate_choice = 0;
                    in_alternate_fallback = 0;
                    alternate_svg_rendered = 0;
                } else if (!self_closing) {
                    in_alternate_content = 1;
                    in_alternate_choice = 0;
                    in_alternate_fallback = 0;
                    alternate_svg_rendered = 0;
                }
            } else if (in_alternate_content &&
                       xml_local_name_is(xml + name_start,
                                         name_end - name_start, "Choice")) {
                in_alternate_choice = !closing && !self_closing;
                if (in_alternate_choice) in_alternate_fallback = 0;
            } else if (in_alternate_content &&
                       xml_local_name_is(xml + name_start,
                                         name_end - name_start, "Fallback")) {
                in_alternate_fallback = !closing && !self_closing;
                if (in_alternate_fallback) in_alternate_choice = 0;
            } else if (xml_local_name_is(xml + name_start, name_end - name_start, "p")) {
                if (closing) {
                    if (in_hyperlink) {
                        if (!html_literal(html, "</a>")) return 0;
                        in_hyperlink = 0;
                    }
                    if (in_paragraph) {
                        if (!paragraph_rendered && !paragraph_is_list &&
                            !docx_ensure_plain_paragraph(html, &list,
                                                         paragraph_tag,
                                                         paragraph_alignment,
                                                         &paragraph_rendered)) return 0;
                        if (!paragraph_is_list && paragraph_rendered &&
                                   !html_close_paragraph(html, paragraph_tag)) {
                            return 0;
                        }
                    }
                    in_paragraph = 0;
                    paragraph_rendered = 0;
                    paragraph_is_list = 0;
                    num_pr_active = 0;
                    paragraph_direct_num_pr = 0;
                    paragraph_style_num_present = 0;
                    paragraph_num_id = 0;
                    paragraph_num_level = 0;
                    paragraph_style_num_id = 0;
                    paragraph_style_num_level = 0;
                    paragraph_page_break_before = 0;
                    paragraph_tag = "p";
                    paragraph_alignment = NULL;
                } else {
                    if (in_paragraph) {
                        if (!paragraph_rendered && !paragraph_is_list &&
                            !docx_ensure_plain_paragraph(html, &list,
                                                         paragraph_tag,
                                                         paragraph_alignment,
                                                         &paragraph_rendered)) return 0;
                        if (!paragraph_is_list && paragraph_rendered &&
                                   !html_close_paragraph(html, paragraph_tag)) {
                            return 0;
                        }
                    }
                    in_paragraph = 1;
                    paragraph_rendered = 0;
                    paragraph_is_list = 0;
                    num_pr_active = 0;
                    paragraph_direct_num_pr = 0;
                    paragraph_style_num_present = 0;
                    paragraph_num_id = 0;
                    paragraph_num_level = 0;
                    paragraph_style_num_id = 0;
                    paragraph_style_num_level = 0;
                    paragraph_page_break_before = 0;
                    paragraph_tag = "p";
                    paragraph_alignment = NULL;
                }
            } else if (xml_local_name_is(xml + name_start,
                                         name_end - name_start, "hyperlink")) {
                if (closing) {
                    if (in_hyperlink) {
                        if (!html_literal(html, "</a>")) return 0;
                        in_hyperlink = 0;
                    }
                } else if (!self_closing) {
                    const uint8_t *relationship_id;
                    size_t relationship_id_size;
                    if (in_hyperlink && !html_literal(html, "</a>")) return 0;
                    in_hyperlink = 0;
                    const uint8_t *bookmark_name = NULL;
                    size_t bookmark_name_size = 0;
                    relationship_id = NULL;
                    relationship_id_size = 0;
                    (void)xml_attribute_value(xml, name_end, tag_end - 1u,
                                              "id", &relationship_id,
                                              &relationship_id_size);
                    (void)xml_attribute_value(xml, name_end, tag_end - 1u,
                                              "anchor", &bookmark_name,
                                              &bookmark_name_size);
                    {
                        int link_rc = docx_append_hyperlink_start(
                            html, relationships, relationships_size,
                            relationship_id, relationship_id_size,
                            bookmark_name, bookmark_name_size);
                        if (link_rc < 0) return 0;
                        in_hyperlink = link_rc > 0;
                    }
                }
            } else if (!closing && in_paragraph &&
                       xml_local_name_is(xml + name_start,
                                         name_end - name_start, "bookmarkStart")) {
                const uint8_t *bookmark_name;
                size_t bookmark_name_size;
                if (xml_attribute_value(xml, name_end, tag_end - 1u, "name",
                                        &bookmark_name, &bookmark_name_size)) {
                    int bookmark_rc;
                    if (!paragraph_rendered && !paragraph_is_list &&
                        !docx_ensure_plain_paragraph(html, &list,
                                                     paragraph_tag,
                                                     paragraph_alignment,
                                                     &paragraph_rendered)) return 0;
                    bookmark_rc = docx_append_bookmark_target(
                        html, bookmark_name, bookmark_name_size);
                    if (bookmark_rc < 0) return 0;
                }
            } else if (xml_local_name_is(xml + name_start,
                                         name_end - name_start, "numPr")) {
                if (closing) {
                    if (num_pr_active && paragraph_num_id != 0) {
                        uint8_t format = docx_lookup_numbering_format(
                            numbering, paragraph_num_id, paragraph_num_level);
                        if (format == DOCX_LIST_FORMAT_NONE) {
                            if (omitted_list_items) ++*omitted_list_items;
                        } else {
                            int list_rc;
                            if (paragraph_rendered && !paragraph_is_list &&
                                !html_close_paragraph(html, paragraph_tag)) return 0;
                            list_rc = docx_open_list_item(html, numbering, &list,
                                                          paragraph_num_id,
                                                          paragraph_num_level,
                                                          format,
                                                          paragraph_alignment);
                            if (list_rc < 0) return 0;
                            if (list_rc == 0) {
                                if (omitted_list_items) ++*omitted_list_items;
                            } else {
                                paragraph_rendered = 1;
                                paragraph_is_list = 1;
                                paragraph_tag = "li";
                            }
                        }
                    }
                    num_pr_active = 0;
                } else {
                    num_pr_active = 1;
                    paragraph_direct_num_pr = 1;
                    paragraph_num_id = 0;
                    paragraph_num_level = 0;
                }
            } else if (!closing && in_paragraph && num_pr_active &&
                       xml_local_name_is(xml + name_start,
                                         name_end - name_start, "numId")) {
                (void)docx_get_attribute_u32(xml, &(QwDocxXmlTag){
                    name_start, name_end - name_start, tag_end, 0
                }, "val", &paragraph_num_id);
            } else if (!closing && in_paragraph && num_pr_active &&
                       xml_local_name_is(xml + name_start,
                                         name_end - name_start, "ilvl")) {
                (void)docx_get_attribute_u32(xml, &(QwDocxXmlTag){
                    name_start, name_end - name_start, tag_end, 0
                }, "val", &paragraph_num_level);
            } else if (!closing && in_paragraph &&
                       xml_local_name_is(xml + name_start,
                                         name_end - name_start, "pStyle")) {
                const uint8_t *value;
                size_t value_size;
                if (xml_attribute_value(xml, name_end, tag_end - 1u, "val",
                                        &value, &value_size)) {
                    if (!paragraph_direct_num_pr &&
                        docx_lookup_style_numbering(styles, value, value_size,
                                                    &paragraph_style_num_id,
                                                    &paragraph_style_num_level)) {
                        paragraph_style_num_present = 1;
                    }
                    const char *heading_tag = docx_heading_tag(value, value_size);
                    if (heading_tag && !paragraph_is_list &&
                        strcmp(heading_tag, paragraph_tag) != 0) {
                        if (paragraph_rendered &&
                            (!html_close_paragraph(html, paragraph_tag) ||
                             !html_open_paragraph(html, heading_tag,
                                                  paragraph_alignment))) return 0;
                        paragraph_tag = heading_tag;
                    }
                }
            } else if (!closing && in_paragraph &&
                       xml_local_name_is(xml + name_start,
                                         name_end - name_start,
                                         "pageBreakBefore")) {
                paragraph_page_break_before =
                    !xml_tag_has_false_value(xml, name_end, tag_end - 1u);
            } else if (closing && in_paragraph &&
                       xml_local_name_is(xml + name_start,
                                         name_end - name_start, "pPr")) {
                if (!paragraph_direct_num_pr && paragraph_style_num_present &&
                    paragraph_style_num_id != 0) {
                    uint8_t format = docx_lookup_numbering_format(
                        numbering, paragraph_style_num_id,
                        paragraph_style_num_level);
                    if (format == DOCX_LIST_FORMAT_NONE) {
                        if (omitted_list_items) ++*omitted_list_items;
                    } else {
                        int list_rc = docx_open_list_item(
                            html, numbering, &list, paragraph_style_num_id,
                            paragraph_style_num_level, format,
                            paragraph_alignment);
                        if (list_rc < 0) return 0;
                        if (list_rc == 0) {
                            if (omitted_list_items) ++*omitted_list_items;
                        } else {
                            paragraph_rendered = 1;
                            paragraph_is_list = 1;
                            paragraph_num_id = paragraph_style_num_id;
                            paragraph_num_level = paragraph_style_num_level;
                            paragraph_tag = "li";
                        }
                    }
                }
                if (paragraph_page_break_before) {
                    if (!docx_append_page_break(html)) return 0;
                    paragraph_page_break_before = 0;
                }
            } else if (!closing && in_paragraph &&
                       xml_local_name_is(xml + name_start,
                                         name_end - name_start, "jc")) {
                const uint8_t *value;
                size_t value_size;
                if (xml_attribute_value(xml, name_end, tag_end - 1u, "val",
                                        &value, &value_size)) {
                    const char *alignment = docx_alignment(value, value_size);
                    if (alignment && (!paragraph_alignment ||
                                      strcmp(alignment, paragraph_alignment) != 0)) {
                        if (paragraph_is_list) {
                            int list_rc = docx_open_list_item(html, numbering, &list,
                                                          paragraph_num_id,
                                                          paragraph_num_level,
                                                          docx_lookup_numbering_format(
                                                              numbering,
                                                              paragraph_num_id,
                                                              paragraph_num_level),
                                                          alignment);
                            if (list_rc < 0) return 0;
                            if (list_rc == 0) return 0;
                        } else if (paragraph_rendered &&
                                   (!html_close_paragraph(html, paragraph_tag) ||
                                    !html_open_paragraph(html, paragraph_tag, alignment))) {
                            return 0;
                        }
                        paragraph_alignment = alignment;
                    }
                }
            } else if (!closing &&
                       xml_local_name_is(xml + name_start,
                                         name_end - name_start, "extent")) {
                const uint8_t *width_value;
                size_t width_size;
                if (xml_attribute_value(xml, name_end, tag_end - 1u, "cx",
                                        &width_value, &width_size)) {
                    image_width_px = docx_emu_width_to_pixels(width_value, width_size);
                }
            } else if (!closing &&
                       xml_local_name_is(xml + name_start,
                                         name_end - name_start, "svgBlip")) {
                const uint8_t *relationship_id;
                size_t relationship_id_size;
                if (xml_attribute_value(xml, name_end, tag_end - 1u, "embed",
                                        &relationship_id,
                                        &relationship_id_size)) {
                    int image_rc = docx_append_image(html, archive,
                                                    relationships,
                                                    relationships_size,
                                                    relationship_id,
                                                    relationship_id_size,
                                                    image_width_px,
                                                    &embedded_image_size);
                    image_width_px = 0;
                    if (image_rc < 0) return 0;
                    if (image_rc > 0 && in_alternate_content &&
                        in_alternate_choice) {
                        alternate_svg_rendered = 1;
                    } else if (image_rc == 0 && !in_alternate_content &&
                               omitted_images) {
                        ++*omitted_images;
                    }
                }
            } else if (!closing &&
                       xml_local_name_is(xml + name_start,
                                         name_end - name_start, "blip")) {
                const uint8_t *relationship_id;
                size_t relationship_id_size;
                if (xml_attribute_value(xml, name_end, tag_end - 1u, "embed",
                                        &relationship_id,
                                        &relationship_id_size)) {
                    int image_rc = 0;
                    if (!(in_alternate_fallback && alternate_svg_rendered)) {
                        image_rc = docx_append_image(html, archive,
                                                    relationships,
                                                    relationships_size,
                                                    relationship_id,
                                                    relationship_id_size,
                                                    image_width_px,
                                                    &embedded_image_size);
                    }
                    image_width_px = 0;
                    if (image_rc < 0) return 0;
                    if (image_rc == 0 && omitted_images &&
                        !(in_alternate_fallback && alternate_svg_rendered)) {
                        ++*omitted_images;
                    }
                }
            } else if (xml_local_name_is(xml + name_start,
                                         name_end - name_start, "t")) {
                if (closing) {
                    if (strike && !html_literal(html, "</s>")) return 0;
                    if (underline && !html_literal(html, "</u>")) return 0;
                    if (italic && !html_literal(html, "</em>")) return 0;
                    if (bold && !html_literal(html, "</strong>")) return 0;
                    in_text = 0;
                } else if (!self_closing) {
                    if (bold && !html_literal(html, "<strong>")) return 0;
                    if (italic && !html_literal(html, "<em>")) return 0;
                    if (underline && !html_literal(html, "<u>")) return 0;
                    if (strike && !html_literal(html, "<s>")) return 0;
                    in_text = 1;
                }
            } else if (xml_local_name_is(xml + name_start,
                                        name_end - name_start, "r")) {
                if (!closing) {
                    if (in_paragraph && !paragraph_rendered && !paragraph_is_list &&
                        !docx_ensure_plain_paragraph(html, &list,
                                                     paragraph_tag,
                                                     paragraph_alignment,
                                                     &paragraph_rendered)) return 0;
                    bold = italic = underline = strike = 0;
                }
            } else if (xml_local_name_is(xml + name_start,
                                         name_end - name_start, "tbl")) {
                if (!closing && !in_paragraph && !docx_close_list(html, &list)) return 0;
                if (!html_literal(html, closing ? "</table>" : "<table>")) return 0;
            } else if (xml_local_name_is(xml + name_start,
                                         name_end - name_start, "tr")) {
                if (!html_literal(html, closing ? "</tr>" : "<tr>")) return 0;
            } else if (xml_local_name_is(xml + name_start,
                                         name_end - name_start, "tc")) {
                if (closing) {
                    if (!html_literal(html, "</td>")) return 0;
                } else {
                    uint32_t colspan = 1u;
                    char fill[7];
                    char vertical_align[7];
                    char style[80];
                    size_t style_size = 0;
                    if (!docx_cell_properties(xml, xml_size, tag_end,
                                              &colspan, fill,
                                              vertical_align)) return 0;
                    if (!html_literal(html, "<td")) return 0;
                    if (colspan > 1u) {
                        char attribute[40];
                        int length = snprintf(attribute, sizeof(attribute),
                                              " colspan=\"%lu\"",
                                              (unsigned long)colspan);
                        if (length < 0 || (size_t)length >= sizeof(attribute) ||
                            !html_literal(html, attribute)) return 0;
                    }
                    if (fill[0]) {
                        int length = snprintf(style, sizeof(style),
                                              "background-color:#%s", fill);
                        if (length < 0 || (size_t)length >= sizeof(style)) return 0;
                        style_size = (size_t)length;
                    }
                    if (vertical_align[0]) {
                        int length = snprintf(style + style_size,
                                              sizeof(style) - style_size,
                                              "%svertical-align:%s",
                                              style_size ? ";" : "",
                                              vertical_align);
                        if (length < 0 ||
                            (size_t)length >= sizeof(style) - style_size) return 0;
                        style_size += (size_t)length;
                    }
                    if (style_size) {
                        if (!html_literal(html, " style=\"") ||
                            !html_append(html, style, style_size) ||
                            !html_literal(html, "\"")) return 0;
                    }
                    if (!html_literal(html, ">")) return 0;
                }
            } else if (!closing && xml_local_name_is(xml + name_start,
                                                     name_end - name_start, "tab")) {
                if (in_paragraph && !paragraph_rendered && !paragraph_is_list &&
                    !docx_ensure_plain_paragraph(html, &list,
                                                 paragraph_tag,
                                                 paragraph_alignment,
                                                 &paragraph_rendered)) return 0;
                if (!html_literal(html, "&nbsp;&nbsp;&nbsp;&nbsp;")) return 0;
            } else if (!closing &&
                       (xml_local_name_is(xml + name_start, name_end - name_start, "br") ||
                        xml_local_name_is(xml + name_start, name_end - name_start, "cr"))) {
                if (in_paragraph && !paragraph_rendered && !paragraph_is_list &&
                    !docx_ensure_plain_paragraph(html, &list,
                                                 paragraph_tag,
                                                 paragraph_alignment,
                                                 &paragraph_rendered)) return 0;
                if (xml_local_name_is(xml + name_start,
                                      name_end - name_start, "br")) {
                    const uint8_t *break_type;
                    size_t break_type_size;
                    int is_page_break =
                        xml_attribute_value(xml, name_end, tag_end - 1u,
                                            "type", &break_type,
                                            &break_type_size) &&
                        break_type_size == 4u &&
                        memcmp(break_type, "page", 4u) == 0;
                    if (is_page_break) {
                        if (!docx_append_page_break(html)) return 0;
                    } else if (!html_literal(html, "<br>")) {
                        return 0;
                    }
                } else if (!html_literal(html, "<br>")) {
                    return 0;
                }
            } else if (xml_local_name_is(xml + name_start,
                                         name_end - name_start, "b")) {
                if (!closing) bold = !xml_tag_has_false_value(xml, name_end, tag_end);
            } else if (xml_local_name_is(xml + name_start,
                                         name_end - name_start, "i")) {
                if (!closing) italic = !xml_tag_has_false_value(xml, name_end, tag_end);
            } else if (xml_local_name_is(xml + name_start,
                                        name_end - name_start, "u")) {
                if (!closing) underline = !xml_tag_has_false_value(xml, name_end, tag_end);
            } else if (xml_local_name_is(xml + name_start,
                                         name_end - name_start, "strike")) {
                if (!closing) strike = !xml_tag_has_false_value(xml, name_end, tag_end);
            }
            i = tag_end;
            continue;
        }
        if (in_text) {
            if (in_paragraph && !paragraph_rendered && !paragraph_is_list &&
                !docx_ensure_plain_paragraph(html, &list,
                                             paragraph_tag,
                                             paragraph_alignment,
                                             &paragraph_rendered)) return 0;
            size_t start = i;
            while (i < xml_size && xml[i] != '<') ++i;
            if (!append_word_text(html, xml + start, i - start)) return 0;
        } else {
            ++i;
        }
    }
    if (in_hyperlink && !html_literal(html, "</a>")) return 0;
    if (in_paragraph) {
        if (!paragraph_rendered && !paragraph_is_list &&
            !docx_ensure_plain_paragraph(html, &list,
                                         paragraph_tag,
                                         paragraph_alignment,
                                         &paragraph_rendered)) return 0;
        if (!paragraph_is_list && paragraph_rendered &&
                   !html_close_paragraph(html, paragraph_tag)) return 0;
    }
    return docx_close_list(html, &list) &&
           html_append(html, suffix, sizeof(suffix) - 1u);
}

static int docx_build_layout_html(const void *data, size_t size,
                                  QwDocxHtml *out_html,
                                  QwDocxPageLayout *out_page_layout,
                                  int *out_mixed_section_layout,
                                  size_t *out_omitted_images,
                                  size_t *out_omitted_list_items,
                                  QwConvertReport *report)
{
    QwZipArchive archive = {0};
    size_t file_count;
    size_t document_index = (size_t)-1;
    int relationships_index = -1;
    int numbering_index = -1;
    size_t xml_size = 0;
    size_t relationships_size = 0;
    size_t numbering_size = 0;
    void *xml = NULL;
    void *relationships = NULL;
    void *numbering_xml = NULL;
    QwDocxHtml html = {0};
    QwDocxNumbering numbering;
    QwDocxStyles styles;
    QwDocxPageLayout page_layout;
    size_t omitted_images = 0;
    size_t omitted_list_items = 0;
    int mixed_section_layout = 0;
    int archive_open = 0;
    int rc = QW_STATUS_ERR_FORMAT;
    if (!data || !size || !out_html || !out_page_layout ||
        !out_mixed_section_layout || !out_omitted_images ||
        !out_omitted_list_items) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_html, 0, sizeof(*out_html));
    memset(out_page_layout, 0, sizeof(*out_page_layout));
    *out_mixed_section_layout = 0;
    *out_omitted_images = 0u;
    *out_omitted_list_items = 0u;
#ifdef QUIREWISE_STANDALONE_DOCX_PARSER
    if (!qw_zip_reader_init_mem_with_profile(
            &archive, data, size, 0,
            QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE)) {
#else
    if (!qw_zip_reader_init_mem_with_profile(
            &archive, data, size, 0,
            qw_format_resource_profile_get(QW_FORMAT_DOCX))) {
#endif
        qw_zip_reader_end(&archive);
        return QW_STATUS_ERR_FORMAT;
    }
    archive_open = 1;
    file_count = qw_zip_reader_get_num_files(&archive);
    for (size_t i = 0; i < file_count; ++i) {
        char name[128];
        size_t name_size = qw_zip_reader_get_filename(&archive, i, name, sizeof(name));
        if (name_size == sizeof("word/document.xml") - 1u &&
            memcmp(name, "word/document.xml", sizeof("word/document.xml") - 1u) == 0) {
            document_index = i;
            break;
        }
    }
    if (document_index == (size_t)-1 ||
        !qw_zip_reader_is_file_supported(&archive, document_index)) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "WordprocessingML package has no readable word/document.xml");
        goto cleanup;
    }
    relationships_index = qw_zip_reader_locate_file(
        &archive, "word/_rels/document.xml.rels", NULL,
        QW_ZIP_CASE_SENSITIVE);
    if (relationships_index >= 0 &&
        qw_zip_reader_is_file_supported(&archive, (size_t)relationships_index)) {
        QwZipEntryInfo relationships_stat;
        if (qw_zip_reader_file_stat(&archive, (size_t)relationships_index,
                                    &relationships_stat) &&
            relationships_stat.uncompressed_size <= 16u * 1024u * 1024u &&
            relationships_stat.uncompressed_size <= SIZE_MAX) {
            relationships = qw_zip_reader_extract_to_heap(
                &archive, (size_t)relationships_index,
                &relationships_size, 0);
            if (relationships_size != relationships_stat.uncompressed_size) {
                free(relationships);
                relationships = NULL;
                relationships_size = 0;
            }
        }
    }
    numbering_index = qw_zip_reader_locate_file(
        &archive, "word/numbering.xml", NULL, QW_ZIP_CASE_SENSITIVE);
    if (numbering_index >= 0 &&
        qw_zip_reader_is_file_supported(&archive, (size_t)numbering_index)) {
        QwZipEntryInfo numbering_stat;
        if (qw_zip_reader_file_stat(&archive, (size_t)numbering_index,
                                    &numbering_stat) &&
            numbering_stat.uncompressed_size <= 16u * 1024u * 1024u &&
            numbering_stat.uncompressed_size <= SIZE_MAX) {
            numbering_xml = qw_zip_reader_extract_to_heap(
                &archive, (size_t)numbering_index, &numbering_size, 0);
            if (numbering_size != numbering_stat.uncompressed_size) {
                free(numbering_xml);
                numbering_xml = NULL;
                numbering_size = 0;
            }
        }
    }
    memset(&numbering, 0, sizeof(numbering));
    if (numbering_xml &&
        !docx_parse_numbering((const uint8_t *)numbering_xml,
                              numbering_size, &numbering)) {
        memset(&numbering, 0, sizeof(numbering));
    }
    docx_load_styles(&archive, &styles);
    xml = qw_zip_reader_extract_to_heap(&archive, document_index, &xml_size, 0);
    if (!xml) {
        rc = QW_STATUS_ERR_DECODE;
        goto cleanup;
    }
    if (!docx_parse_page_layout((const uint8_t *)xml, xml_size,
                                &page_layout, &mixed_section_layout)) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "WordprocessingML section layout is malformed");
        goto cleanup;
    }
    if (!docx_document_xml_to_html((const uint8_t *)xml, xml_size,
                                   &archive,
                                    (const uint8_t *)relationships,
                                    relationships_size,
                                    &numbering,
                                    &styles,
                                    &page_layout,
                                    &html, &omitted_images,
                                    &omitted_list_items)) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "WordprocessingML document XML is malformed or exceeds available memory");
        goto cleanup;
    }
    rc = QW_STATUS_OK;

cleanup:
    free(xml);
    free(relationships);
    free(numbering_xml);
    if (archive_open) qw_zip_reader_end(&archive);
    if (rc == QW_STATUS_OK) {
        *out_html = html;
        *out_page_layout = page_layout;
        *out_mixed_section_layout = mixed_section_layout;
        *out_omitted_images = omitted_images;
        *out_omitted_list_items = omitted_list_items;
        memset(&html, 0, sizeof(html));
    }
    free(html.data);
    return rc;
}

#ifdef QUIREWISE_STANDALONE_DOCX_PARSER
int qw_docx_parse_native_html(const void *data, size_t size,
                              char **out_html, size_t *out_html_size,
                              QwDocxPageLayout *out_page_layout)
{
    QwDocxHtml html = {0};
    QwDocxPageLayout page_layout;
    size_t omitted_images = 0u;
    size_t omitted_list_items = 0u;
    int mixed_section_layout = 0;
    int status;
    if (!out_html || !out_html_size || !out_page_layout)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_html = NULL;
    *out_html_size = 0u;
    memset(out_page_layout, 0, sizeof(*out_page_layout));
    status = docx_build_layout_html(data, size, &html, &page_layout,
                                    &mixed_section_layout, &omitted_images,
                                    &omitted_list_items, NULL);
    if (status != QW_STATUS_OK) return status;
    page_layout.mixed_section_layout = mixed_section_layout;
    page_layout.omitted_images = omitted_images;
    page_layout.omitted_list_items = omitted_list_items;
    *out_html = html.data;
    *out_html_size = html.size;
    *out_page_layout = page_layout;
    return QW_STATUS_OK;
}
#else
static void docx_report_layout_features(const QwDocxPageLayout *page_layout,
                                       int mixed_section_layout,
                                       size_t omitted_images,
                                       size_t omitted_list_items,
                                       QwConvertReport *report,
                                       const char *prefix)
{
    if (!report) return;
    if (mixed_section_layout && (omitted_images || omitted_list_items)) {
        snprintf(report->note, sizeof(report->note),
                 "%s section page settings vary; uses first section layout; omitted %zu image(s) and %zu list item(s)",
                 prefix, omitted_images, omitted_list_items);
    } else if (mixed_section_layout) {
        snprintf(report->note, sizeof(report->note),
                 "%s section page settings vary; uses the first section layout",
                 prefix);
    } else if (omitted_images || omitted_list_items) {
        snprintf(report->note, sizeof(report->note),
                 "%s omitted %zu unsupported/unavailable embedded image(s) and %zu unsupported list item(s)",
                 prefix, omitted_images, omitted_list_items);
    } else {
        (void)page_layout;
        snprintf(report->note, sizeof(report->note),
                 "%s text, styles, and page layout opened", prefix);
    }
}

int qw_docx_native_open_layout(const void *data, size_t size,
                               QwInputFormat input_format,
                               const QwConvertOptions *options,
                               QwLayoutDocument *out_document,
                               QwConvertReport *report)
{
    QwDocxHtml html = {0};
    QwDocxPageLayout page_layout;
    size_t omitted_images = 0u;
    size_t omitted_list_items = 0u;
    int mixed_section_layout = 0;
    int rc;
    (void)options;
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (input_format != QW_FORMAT_DOCX && input_format != QW_FORMAT_DOCM &&
        input_format != QW_FORMAT_DOTX && input_format != QW_FORMAT_DOTM) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    rc = docx_build_layout_html(data, size, &html, &page_layout,
                               &mixed_section_layout, &omitted_images,
                               &omitted_list_items, report);
    if (rc != QW_STATUS_OK) return rc;
    out_document->html_utf8 = (const uint8_t *)html.data;
    out_document->html_size = html.size;
    out_document->context = html.data;
    out_document->dispose = free;
    out_document->page_width_points = (float)page_layout.width_twips / 20.0f;
    out_document->page_height_points = (float)page_layout.height_twips / 20.0f;
    docx_report_layout_features(&page_layout, mixed_section_layout,
                                omitted_images, omitted_list_items, report,
                                input_format == QW_FORMAT_DOCM ? "DOCM layout" :
                                input_format == QW_FORMAT_DOTX ? "DOTX layout" :
                                input_format == QW_FORMAT_DOTM ? "DOTM layout" :
                                "DOCX layout");
    if (report && input_format == QW_FORMAT_DOCM) {
        char detail[sizeof(report->note)];
        (void)snprintf(detail, sizeof(detail), "%s", report->note);
        (void)snprintf(report->note, sizeof(report->note),
                       "VBA macros and package metadata are discarded; %s",
                       detail);
    } else if (report && input_format == QW_FORMAT_DOTX) {
        char detail[sizeof(report->note)];
        (void)snprintf(detail, sizeof(detail), "%s", report->note);
        (void)snprintf(report->note, sizeof(report->note),
                       "Template behavior and package metadata are not preserved; %s",
                       detail);
    } else if (report && input_format == QW_FORMAT_DOTM) {
        char detail[sizeof(report->note)];
        (void)snprintf(detail, sizeof(detail), "%s", report->note);
        (void)snprintf(report->note, sizeof(report->note),
                       "VBA macros and template behavior are not preserved; %s",
                       detail);
    }
    return QW_STATUS_OK;
}



static int docx_convert_to_pdf(const void *data,
                               size_t size,
                               const QwConvertOptions *options,
                               uint8_t **out_pdf,
                               size_t *out_size,
                               QwConvertReport *report)
{
    QwDocxHtml html = {0};
    QwDocxPageLayout page_layout;
    size_t omitted_images = 0u;
    size_t omitted_list_items = 0u;
    int mixed_section_layout = 0;
    int rc;
    (void)options;
    if (!out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    rc = docx_build_layout_html(data, size, &html, &page_layout,
                               &mixed_section_layout, &omitted_images,
                               &omitted_list_items, report);
    if (rc != QW_STATUS_OK) return rc;
    rc = qw_mupdf_convert_html_with_page_size(
        html.data, html.size,
        (float)page_layout.width_twips / 20.0f,
        (float)page_layout.height_twips / 20.0f,
        out_pdf, out_size, report);
    free(html.data);
    if (rc == 0 && report &&
        (mixed_section_layout || omitted_images || omitted_list_items)) {
        docx_report_layout_features(&page_layout, mixed_section_layout,
                                    omitted_images, omitted_list_items, report,
                                    "DOCX PDF conversion");
    }
    return rc;
}









const QwFormatDescriptor qw_format_docx = {
    .id = QW_FORMAT_DOCX,
    .name = "DOCX",
    .capabilities = (QW_FORMAT_CAP_CONTAINER | QW_FORMAT_CAP_MULTI_PAGE) | (QW_FORMAT_CAP_ARCHIVE_PROBE),
    .probe = NULL,
    .open_memory = NULL,
    .open_layout = qw_docx_native_open_layout,
    .open_audio = NULL,
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_DEFAULT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE,
    .convert_to_pdf = docx_convert_to_pdf
};

#define DOCX_LAYOUT_DESCRIPTOR(symbol, format_id, format_name) \
const QwFormatDescriptor symbol = { \
    .id = format_id, \
    .name = format_name, \
    .capabilities = QW_FORMAT_CAP_CONTAINER | QW_FORMAT_CAP_MULTI_PAGE | \
                    QW_FORMAT_CAP_ARCHIVE_PROBE, \
    .probe = NULL, \
    .open_memory = NULL, \
    .open_layout = qw_docx_native_open_layout, \
    .open_audio = NULL, \
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_DEFAULT, \
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE, \
    .convert_to_pdf = NULL \
}

DOCX_LAYOUT_DESCRIPTOR(qw_format_docm, QW_FORMAT_DOCM, "DOCM");
DOCX_LAYOUT_DESCRIPTOR(qw_format_dotx, QW_FORMAT_DOTX, "DOTX");
DOCX_LAYOUT_DESCRIPTOR(qw_format_dotm, QW_FORMAT_DOTM, "DOTM");

#undef DOCX_LAYOUT_DESCRIPTOR
#endif
