#include "format_registry.h"
#include "odt_format.h"

#if !defined(QUIREWISE_STANDALONE_ODT)
#include "../../backends/mupdf_backend.h"
#endif
#include "../../core/xml_parser.h"
#include "../archive/zip_native.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QW_ODT_FLAT_XML_MAX_INPUT_BYTES ((size_t)64u * 1024u * 1024u)
#define QW_ODT_FODT_PROBE_BYTES 65536u

static int odt_probe_bytes_contain(const uint8_t *data, size_t size,
                                   const char *pattern, size_t pattern_size)
{
    size_t i;
    if (!data || !pattern || !pattern_size || pattern_size > size) return 0;
    for (i = 0u; i <= size - pattern_size; ++i) {
        if (memcmp(data + i, pattern, pattern_size) == 0) return 1;
    }
    return 0;
}

static int odt_fodt_root_header(const uint8_t *data, size_t size,
                                size_t *out_start, size_t *out_end)
{
    static const char root[] = "<office:document";
    size_t offset = 0u;
    size_t next;
    unsigned char quote = 0u;
    if (!data || !out_start || !out_end) return 0;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu) {
        offset = 3u;
    }
    for (;;) {
        while (offset < size && (data[offset] == ' ' || data[offset] == '\t' ||
                                 data[offset] == '\r' || data[offset] == '\n')) {
            ++offset;
        }
        if (offset + 2u <= size && data[offset] == '<' && data[offset + 1u] == '?') {
            next = offset + 2u;
            while (next + 1u < size &&
                   !(data[next] == '?' && data[next + 1u] == '>')) ++next;
            if (next + 1u >= size) return 0;
            offset = next + 2u;
            continue;
        }
        if (offset + 4u <= size && data[offset] == '<' &&
            data[offset + 1u] == '!' && data[offset + 2u] == '-' &&
            data[offset + 3u] == '-') {
            next = offset + 4u;
            while (next + 2u < size &&
                   !(data[next] == '-' && data[next + 1u] == '-' &&
                     data[next + 2u] == '>')) ++next;
            if (next + 2u >= size) return 0;
            offset = next + 3u;
            continue;
        }
        break;
    }
    if (offset + sizeof(root) - 1u > size ||
        memcmp(data + offset, root, sizeof(root) - 1u) != 0) return 0;
    next = offset + sizeof(root) - 1u;
    if (next >= size || data[next] == '-' || data[next] == '_' ||
        data[next] == ':' || data[next] == '.') return 0;
    while (next < size) {
        unsigned char ch = data[next++];
        if (quote) {
            if (ch == quote) quote = 0u;
        } else if (ch == '\'' || ch == '"') {
            quote = ch;
        } else if (ch == '>') {
            *out_start = offset;
            *out_end = next;
            return 1;
        }
    }
    return 0;
}

static int odt_fodt_probe(const void *data, size_t size)
{
    static const char office_namespace[] =
        "urn:oasis:names:tc:opendocument:xmlns:office:1.0";
    static const char mime_attribute[] = "office:mimetype";
    static const char text_mimetype[] =
        "application/vnd.oasis.opendocument.text";
    const uint8_t *bytes = (const uint8_t *)data;
    size_t probe_size = size < QW_ODT_FODT_PROBE_BYTES
        ? size : QW_ODT_FODT_PROBE_BYTES;
    size_t root_start;
    size_t root_end;
    if (!bytes || !size || size > QW_ODT_FLAT_XML_MAX_INPUT_BYTES ||
        (size >= 4u && bytes[0] == 'P' && bytes[1] == 'K' &&
         ((bytes[2] == 3u && bytes[3] == 4u) ||
          (bytes[2] == 5u && bytes[3] == 6u) ||
          (bytes[2] == 7u && bytes[3] == 8u)))) {
        return 0;
    }
    return odt_fodt_root_header(bytes, probe_size, &root_start, &root_end) &&
           odt_probe_bytes_contain(bytes + root_start,
                                   root_end - root_start, office_namespace,
                                   sizeof(office_namespace) - 1u) &&
           odt_probe_bytes_contain(bytes + root_start,
                                   root_end - root_start, mime_attribute,
                                   sizeof(mime_attribute) - 1u) &&
           odt_probe_bytes_contain(bytes + root_start,
                                   root_end - root_start, text_mimetype,
                                   sizeof(text_mimetype) - 1u);
}

#if defined(QUIREWISE_WITH_MUPDF) || defined(QUIREWISE_STANDALONE_ODT)
#if defined(QUIREWISE_WITH_MUPDF)
#include <mupdf/fitz.h>
#endif

enum {
    ODT_XML_LIMIT = 64 * 1024 * 1024,
    ODT_IMAGE_LIMIT = 32 * 1024 * 1024,
    ODT_IMAGE_TOTAL_LIMIT = 64 * 1024 * 1024,
    ODT_HTML_LIMIT = 128 * 1024 * 1024,
    ODT_STYLE_NAME_LIMIT = 128,
    ODT_STYLE_VALUE_LIMIT = 64,
    ODT_MAX_STYLES = 8192,
    ODT_MAX_NODES = 1000000,
    ODT_MAX_DEPTH = 128
};

typedef struct OdtHtml {
    char *data;
    size_t size;
    size_t capacity;
} OdtHtml;

typedef struct OdtStyle {
    char name[ODT_STYLE_NAME_LIMIT];
    char family[32];
    char text_css[384];
    char paragraph_css[384];
    int ordered_list;
} OdtStyle;

typedef struct OdtStyles {
    OdtStyle *items;
    size_t count;
    size_t capacity;
} OdtStyles;

typedef struct OdtRenderContext {
    QwZipArchive *zip;
    OdtHtml *html;
    OdtStyles *styles;
    size_t embedded_image_bytes;
    size_t skipped_images;
    size_t visited_nodes;
} OdtRenderContext;

static int odt_html_append(OdtHtml *html, const char *data, size_t size)
{
    size_t required;
    size_t capacity;
    char *grown;
    if (!html || (!data && size) || size > SIZE_MAX - html->size - 1u) return 0;
    required = html->size + size + 1u;
    if (required > ODT_HTML_LIMIT) return 0;
    if (required > html->capacity) {
        capacity = html->capacity ? html->capacity : 1024u;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2u) return 0;
            capacity *= 2u;
        }
        if (capacity > ODT_HTML_LIMIT) capacity = ODT_HTML_LIMIT;
        grown = (char *)realloc(html->data, capacity);
        if (!grown) return 0;
        html->data = grown;
        html->capacity = capacity;
    }
    if (size) memcpy(html->data + html->size, data, size);
    html->size += size;
    html->data[html->size] = '\0';
    return 1;
}

static int odt_html_literal(OdtHtml *html, const char *text)
{
    return text && odt_html_append(html, text, strlen(text));
}

static int odt_html_escape(OdtHtml *html, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    const unsigned char *start = cursor;
    if (!text) return 1;
    while (*cursor) {
        const char *replacement = NULL;
        switch (*cursor) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        default: break;
        }
        if (replacement) {
            if (!odt_html_append(html, (const char *)start,
                                 (size_t)(cursor - start)) ||
                !odt_html_literal(html, replacement)) return 0;
            start = cursor + 1u;
        }
        ++cursor;
    }
    return odt_html_append(html, (const char *)start,
                           (size_t)(cursor - start));
}

static const char *odt_local_name(const char *qualified_name)
{
    const char *colon;
    if (!qualified_name) return "";
    colon = strrchr(qualified_name, ':');
    return colon ? colon + 1u : qualified_name;
}

static int odt_is_tag(QwXmlNode *node, const char *local_name)
{
    const char *tag = qw_xml_tag(node);
    return tag && strcmp(odt_local_name(tag), local_name) == 0;
}

static int odt_copy_bounded(char *destination, size_t capacity,
                            const char *source)
{
    size_t size;
    if (!destination || capacity == 0u || !source) return 0;
    size = strlen(source);
    if (size >= capacity) return 0;
    memcpy(destination, source, size + 1u);
    return 1;
}

static int odt_is_safe_css_length(const char *value)
{
    size_t i = 0;
    int digits = 0;
    size_t value_size;
    if (!value || !(value_size = strlen(value)) ||
        value_size >= ODT_STYLE_VALUE_LIMIT) return 0;
    while (i < value_size &&
           ((value[i] >= '0' && value[i] <= '9') || value[i] == '.' ||
            value[i] == '-')) {
        if (value[i] >= '0' && value[i] <= '9') digits = 1;
        ++i;
    }
    if (!digits) return 0;
    return strcmp(value + i, "pt") == 0 || strcmp(value + i, "pc") == 0 ||
           strcmp(value + i, "in") == 0 || strcmp(value + i, "cm") == 0 ||
           strcmp(value + i, "mm") == 0 || strcmp(value + i, "px") == 0 ||
           strcmp(value + i, "%") == 0 || value[i] == '\0';
}

static int odt_is_safe_color(const char *value)
{
    size_t i;
    if (!value || value[0] != '#' || strlen(value) != 7u) return 0;
    for (i = 1u; i < 7u; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f') ||
              (value[i] >= 'A' && value[i] <= 'F'))) return 0;
    }
    return 1;
}

static int odt_css_add(char *css, size_t capacity,
                       const char *property, const char *value)
{
    size_t used;
    int written;
    if (!css || !capacity || !property || !value) return 0;
    used = strlen(css);
    if (used >= capacity) return 0;
    written = snprintf(css + used, capacity - used, "%s%s:%s;",
                       used ? "" : "", property, value);
    return written >= 0 && (size_t)written < capacity - used;
}

static int odt_css_add_length(char *css, size_t capacity,
                              const char *property, const char *value)
{
    return odt_is_safe_css_length(value) &&
           odt_css_add(css, capacity, property, value);
}

static int odt_css_add_weight(char *css, size_t capacity, const char *value)
{
    size_t i;
    if (!value) return 1;
    if (strcmp(value, "bold") == 0 || strcmp(value, "normal") == 0) {
        return odt_css_add(css, capacity, "font-weight", value);
    }
    if (strlen(value) != 3u) return 1;
    for (i = 0; i < 3u; ++i) {
        if (value[i] < '0' || value[i] > '9') return 1;
    }
    return odt_css_add(css, capacity, "font-weight", value);
}

static int odt_style_add_or_replace(OdtStyles *styles, OdtStyle *style)
{
    size_t i;
    OdtStyle *grown;
    size_t capacity;
    for (i = 0; i < styles->count; ++i) {
        if (strcmp(styles->items[i].name, style->name) == 0 &&
            strcmp(styles->items[i].family, style->family) == 0) {
            styles->items[i] = *style;
            return 1;
        }
    }
    if (styles->count >= ODT_MAX_STYLES) return 0;
    if (styles->count == styles->capacity) {
        capacity = styles->capacity ? styles->capacity * 2u : 64u;
        if (capacity > ODT_MAX_STYLES) capacity = ODT_MAX_STYLES;
        grown = (OdtStyle *)realloc(styles->items,
                                    capacity * sizeof(*styles->items));
        if (!grown) return 0;
        styles->items = grown;
        styles->capacity = capacity;
    }
    styles->items[styles->count++] = *style;
    return 1;
}

static int odt_collect_styles_node(QwXmlNode *node, OdtStyles *styles,
                                   size_t depth)
{
    QwXmlNode *child;
    if (depth > ODT_MAX_DEPTH) return 0;
    for (; node; node = qw_xml_next(node)) {
        if (odt_is_tag(node, "style")) {
            const char *name = qw_xml_att(node, "style:name");
            const char *family = qw_xml_att(node, "style:family");
            OdtStyle style;
            QwXmlNode *property;
            memset(&style, 0, sizeof(style));
            if (name && family && odt_copy_bounded(style.name,
                    sizeof(style.name), name) && odt_copy_bounded(style.family,
                    sizeof(style.family), family)) {
                for (property = qw_xml_down(node); property;
                     property = qw_xml_next(property)) {
                    const char *value;
                    if (odt_is_tag(property, "text-properties")) {
                        value = qw_xml_att(property, "fo:font-weight");
                        if (!odt_css_add_weight(style.text_css,
                                sizeof(style.text_css), value)) return 0;
                        value = qw_xml_att(property, "fo:font-style");
                        if (value && (strcmp(value, "italic") == 0 ||
                                      strcmp(value, "oblique") == 0 ||
                                      strcmp(value, "normal") == 0) &&
                            !odt_css_add(style.text_css,
                                sizeof(style.text_css), "font-style", value)) return 0;
                        value = qw_xml_att(property, "style:text-underline-style");
                        if (value && strcmp(value, "none") != 0 &&
                            !odt_css_add(style.text_css,
                                sizeof(style.text_css), "text-decoration", "underline")) return 0;
                        value = qw_xml_att(property, "style:text-line-through-style");
                        if (value && strcmp(value, "none") != 0 &&
                            !odt_css_add(style.text_css,
                                sizeof(style.text_css), "text-decoration", "line-through")) return 0;
                        value = qw_xml_att(property, "fo:color");
                        if (odt_is_safe_color(value) &&
                            !odt_css_add(style.text_css,
                                sizeof(style.text_css), "color", value)) return 0;
                        value = qw_xml_att(property, "fo:font-size");
                        if (odt_is_safe_css_length(value) &&
                            !odt_css_add_length(style.text_css,
                                sizeof(style.text_css), "font-size", value)) return 0;
                    } else if (odt_is_tag(property, "paragraph-properties")) {
                        static const struct { const char *attribute; const char *css; } lengths[] = {
                            { "fo:margin-left", "margin-left" },
                            { "fo:margin-right", "margin-right" },
                            { "fo:margin-top", "margin-top" },
                            { "fo:margin-bottom", "margin-bottom" },
                            { "fo:text-indent", "text-indent" }
                        };
                        size_t i;
                        value = qw_xml_att(property, "fo:text-align");
                        if (value && (strcmp(value, "left") == 0 ||
                                      strcmp(value, "right") == 0 ||
                                      strcmp(value, "center") == 0 ||
                                      strcmp(value, "justify") == 0) &&
                            !odt_css_add(style.paragraph_css,
                                sizeof(style.paragraph_css), "text-align", value)) return 0;
                        for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
                            value = qw_xml_att(property, lengths[i].attribute);
                            if (value && odt_is_safe_css_length(value) &&
                                !odt_css_add_length(style.paragraph_css,
                                    sizeof(style.paragraph_css), lengths[i].css,
                                    value)) return 0;
                        }
                        value = qw_xml_att(property, "fo:break-before");
                        if (value && strcmp(value, "page") == 0 &&
                            !odt_css_add(style.paragraph_css,
                                sizeof(style.paragraph_css),
                                "break-before", "page")) return 0;
                    }
                }
                if (strcmp(style.family, "list") == 0) {
                    QwXmlNode *level;
                    for (level = qw_xml_down(node); level;
                         level = qw_xml_next(level)) {
                        if (odt_is_tag(level, "list-level-style-number")) {
                            style.ordered_list = 1;
                            break;
                        }
                    }
                }
                if (!odt_style_add_or_replace(styles, &style)) return 0;
            }
        } else if (odt_is_tag(node, "list-style")) {
            const char *name = qw_xml_att(node, "style:name");
            OdtStyle style;
            QwXmlNode *level;
            memset(&style, 0, sizeof(style));
            if (name && odt_copy_bounded(style.name, sizeof(style.name), name)) {
                memcpy(style.family, "list", sizeof("list"));
                for (level = qw_xml_down(node); level;
                     level = qw_xml_next(level)) {
                    if (odt_is_tag(level, "list-level-style-number")) {
                        style.ordered_list = 1;
                        break;
                    }
                }
                if (!odt_style_add_or_replace(styles, &style)) return 0;
            }
        }
        child = qw_xml_down(node);
        if (child && !odt_collect_styles_node(child, styles, depth + 1u)) return 0;
    }
    return 1;
}

static const OdtStyle *odt_find_style(const OdtStyles *styles,
                                     const char *name,
                                     const char *family)
{
    size_t i;
    if (!styles || !name) return NULL;
    for (i = 0; i < styles->count; ++i) {
        if (strcmp(styles->items[i].name, name) == 0 &&
            (!family || strcmp(styles->items[i].family, family) == 0)) {
            return &styles->items[i];
        }
    }
    return NULL;
}

static int odt_append_style_attribute(OdtHtml *html, const char *css)
{
    return !css || !*css ||
           (odt_html_literal(html, " style=\"") &&
            odt_html_escape(html, css) && odt_html_literal(html, "\""));
}

static int odt_append_css_attribute(OdtHtml *html, const OdtStyle *style,
                                    int paragraph)
{
    return odt_append_style_attribute(html, style
        ? (paragraph ? style->paragraph_css : style->text_css) : NULL);
}

static int odt_is_hex(unsigned char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

static int odt_decode_image_path(const char *href, char *path, size_t capacity)
{
    size_t in = 0;
    size_t out = 0;
    size_t segment_start = 0;
    if (!href || !*href || !path || capacity < 2u ||
        strlen(href) >= capacity || href[0] == '/' || href[0] == '\\') return 0;
    while (href[in]) {
        unsigned char ch = (unsigned char)href[in++];
        if (ch == '%') {
            unsigned int high;
            unsigned int low;
            if (!href[in] || !href[in + 1u] ||
                !odt_is_hex((unsigned char)href[in]) ||
                !odt_is_hex((unsigned char)href[in + 1u])) return 0;
            high = href[in] >= '0' && href[in] <= '9'
                ? (unsigned int)(href[in] - '0')
                : (unsigned int)((href[in] | 32) - 'a' + 10);
            low = href[in + 1u] >= '0' && href[in + 1u] <= '9'
                ? (unsigned int)(href[in + 1u] - '0')
                : (unsigned int)((href[in + 1u] | 32) - 'a' + 10);
            ch = (unsigned char)((high << 4) | low);
            in += 2u;
        }
        if (ch == '\0' || ch == ':' || ch < 0x20u) return 0;
        if (ch == '\\') ch = '/';
        if (ch == '/') {
            size_t segment_size = out - segment_start;
            if (segment_size == 2u && path[segment_start] == '.' &&
                path[segment_start + 1u] == '.') return 0;
            if (!segment_size || (segment_size == 1u && path[segment_start] == '.')) {
                segment_start = out;
                continue;
            }
            if (out + 1u >= capacity) return 0;
            path[out++] = '/';
            segment_start = out;
            continue;
        }
        if (out + 1u >= capacity) return 0;
        path[out++] = (char)ch;
    }
    {
        size_t segment_size = out - segment_start;
        if (segment_size == 2u && path[segment_start] == '.' &&
            path[segment_start + 1u] == '.') return 0;
    }
    while (out && path[out - 1u] == '/') --out;
    if (!out) return 0;
    path[out] = '\0';
    return 1;
}

static const char *odt_image_mime(const char *path)
{
    const char *extension = strrchr(path, '.');
    char lower[8];
    size_t i;
    if (!extension || strlen(extension + 1u) >= sizeof(lower)) return NULL;
    ++extension;
    for (i = 0; extension[i]; ++i) {
        unsigned char ch = (unsigned char)extension[i];
        lower[i] = (char)(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    }
    lower[i] = '\0';
    if (strcmp(lower, "png") == 0) return "image/png";
    if (strcmp(lower, "jpg") == 0 || strcmp(lower, "jpeg") == 0) return "image/jpeg";
    if (strcmp(lower, "gif") == 0) return "image/gif";
    if (strcmp(lower, "svg") == 0) return "image/svg+xml";
    if (strcmp(lower, "webp") == 0) return "image/webp";
    return NULL;
}

static int odt_base64(const uint8_t *data, size_t size,
                      char **out_text, size_t *out_size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t encoded_size;
    char *encoded;
    size_t i;
    size_t out = 0;
    if (!data || !out_text || !out_size || size > (SIZE_MAX - 2u) / 3u) return 0;
    encoded_size = ((size + 2u) / 3u) * 4u;
    if (encoded_size == SIZE_MAX) return 0;
    encoded = (char *)malloc(encoded_size + 1u);
    if (!encoded) return 0;
    for (i = 0; i < size; i += 3u) {
        uint32_t value = (uint32_t)data[i] << 16;
        size_t remain = size - i;
        if (remain > 1u) value |= (uint32_t)data[i + 1u] << 8;
        if (remain > 2u) value |= data[i + 2u];
        encoded[out++] = alphabet[(value >> 18) & 63u];
        encoded[out++] = alphabet[(value >> 12) & 63u];
        encoded[out++] = remain > 1u ? alphabet[(value >> 6) & 63u] : '=';
        encoded[out++] = remain > 2u ? alphabet[value & 63u] : '=';
    }
    encoded[out] = '\0';
    *out_text = encoded;
    *out_size = out;
    return 1;
}

static int odt_render_image(OdtRenderContext *context, QwXmlNode *node)
{
    char path[2048];
    const char *href = qw_xml_att(node, "xlink:href");
    const char *mime;
    size_t file_index;
    QwZipEntryInfo stat;
    size_t extracted_size = 0;
    void *bytes;
    char *encoded = NULL;
    size_t encoded_size = 0;
    int ok;
    if (!context->zip) {
        ++context->skipped_images;
        return 1;
    }
    if (!odt_decode_image_path(href, path, sizeof(path)) ||
        !(mime = odt_image_mime(path))) {
        ++context->skipped_images;
        return 1;
    }
    file_index = qw_zip_reader_locate_file(context->zip, path, NULL, 0);
    if (file_index == (size_t)-1 ||
        !qw_zip_reader_file_stat(context->zip, file_index, &stat) ||
        stat.uncompressed_size > ODT_IMAGE_LIMIT ||
        stat.uncompressed_size > ODT_IMAGE_TOTAL_LIMIT - context->embedded_image_bytes ||
        !qw_zip_reader_is_file_supported(context->zip, file_index)) {
        ++context->skipped_images;
        return 1;
    }
    bytes = qw_zip_reader_extract_to_heap(context->zip, file_index,
                                           &extracted_size, 0);
    if (!bytes || extracted_size != stat.uncompressed_size ||
        !odt_base64((const uint8_t *)bytes, extracted_size,
                    &encoded, &encoded_size)) {
        free(bytes);
        free(encoded);
        ++context->skipped_images;
        return 1;
    }
    context->embedded_image_bytes += extracted_size;
    ok = odt_html_literal(context->html, "<img src=\"data:") &&
         odt_html_literal(context->html, mime) &&
         odt_html_literal(context->html, ";base64,") &&
         odt_html_append(context->html, encoded, encoded_size) &&
         odt_html_literal(context->html, "\" alt=\"\"/>");
    free(bytes);
    free(encoded);
    return ok;
}

static int odt_append_row_span(OdtHtml *html, const char *value,
                               const char *attribute)
{
    unsigned long number = 0;
    size_t i;
    if (!value || !*value) return 1;
    for (i = 0; value[i]; ++i) {
        if (value[i] < '0' || value[i] > '9' || number > 64u) return 1;
        number = number * 10u + (unsigned long)(value[i] - '0');
    }
    if (number < 2u || number > 64u) return 1;
    return odt_html_literal(html, " ") && odt_html_literal(html, attribute) &&
           odt_html_literal(html, "=\"") &&
           odt_html_append(html, value, i) && odt_html_literal(html, "\"");
}

static const OdtStyle *odt_style_for_node(OdtRenderContext *context,
                                          QwXmlNode *node,
                                          const char *family)
{
    return odt_find_style(context->styles,
                          qw_xml_att(node, "text:style-name"), family);
}

static int odt_render_nodes(OdtRenderContext *context, QwXmlNode *node,
                            size_t depth);

static int odt_render_element(OdtRenderContext *context, QwXmlNode *node,
                              size_t depth)
{
    const char *tag = odt_local_name(qw_xml_tag(node));
    const OdtStyle *style;
    const char *html_tag = NULL;
    int paragraph = 0;
    if (strcmp(tag, "p") == 0) {
        html_tag = "p";
        paragraph = 1;
        style = odt_style_for_node(context, node, "paragraph");
    } else if (strcmp(tag, "h") == 0) {
        const char *outline = qw_xml_att(node, "text:outline-level");
        unsigned int level = outline && outline[0] >= '1' && outline[0] <= '6'
            ? (unsigned int)(outline[0] - '0') : 1u;
        char heading[4] = { '<', 'h', (char)('0' + level), '\0' };
        char closing[5] = { '<', '/', 'h', (char)('0' + level), '\0' };
        style = odt_style_for_node(context, node, "paragraph");
        if (!odt_html_literal(context->html, heading) ||
            !odt_append_css_attribute(context->html, style, 1) ||
            !odt_html_literal(context->html, ">") ||
            !odt_render_nodes(context, qw_xml_down(node), depth + 1u) ||
            !odt_html_literal(context->html, closing) ||
            !odt_html_literal(context->html, ">")) return 0;
        return 1;
    } else if (strcmp(tag, "span") == 0) {
        html_tag = "span";
        style = odt_style_for_node(context, node, "text");
    } else if (strcmp(tag, "list") == 0) {
        const char *name = qw_xml_att(node, "text:style-name");
        const OdtStyle *list_style = odt_find_style(context->styles, name, "list");
        html_tag = list_style && list_style->ordered_list ? "ol" : "ul";
        style = NULL;
    } else if (strcmp(tag, "list-item") == 0) {
        html_tag = "li";
        style = NULL;
    } else if (strcmp(tag, "table") == 0) {
        html_tag = "table";
        style = NULL;
    } else if (strcmp(tag, "table-row") == 0) {
        html_tag = "tr";
        style = NULL;
    } else if (strcmp(tag, "table-cell") == 0) {
        html_tag = "td";
        style = NULL;
    } else if (strcmp(tag, "covered-table-cell") == 0) {
        return 1;
    } else if (strcmp(tag, "line-break") == 0 || strcmp(tag, "soft-page-break") == 0) {
        return odt_html_literal(context->html, "<br/>");
    } else if (strcmp(tag, "tab") == 0) {
        return odt_html_literal(context->html, "&emsp;&emsp;");
    } else if (strcmp(tag, "s") == 0) {
        const char *count_text = qw_xml_att(node, "text:c");
        unsigned int count = 1u;
        unsigned int i;
        if (count_text && *count_text) {
            char *end = NULL;
            unsigned long parsed = strtoul(count_text, &end, 10);
            if (end && !*end && parsed <= 256u) count = (unsigned int)parsed;
        }
        for (i = 0; i < count; ++i) {
            if (!odt_html_literal(context->html, "&nbsp;")) return 0;
        }
        return 1;
    } else if (strcmp(tag, "image") == 0) {
        return odt_render_image(context, node);
    } else if (strcmp(tag, "frame") == 0) {
        return odt_render_nodes(context, qw_xml_down(node), depth + 1u);
    } else if (strcmp(tag, "a") == 0) {
        /* Keep link labels but do not emit active external URLs. */
        return odt_render_nodes(context, qw_xml_down(node), depth + 1u);
    } else {
        return odt_render_nodes(context, qw_xml_down(node), depth + 1u);
    }
    if (!odt_html_literal(context->html, "<") ||
        !odt_html_literal(context->html, html_tag) ||
        !odt_append_css_attribute(context->html, style, paragraph)) return 0;
    if (strcmp(tag, "table-cell") == 0 &&
        (!odt_append_row_span(context->html,
                              qw_xml_att(node, "table:number-columns-spanned"),
                              "colspan") ||
         !odt_append_row_span(context->html,
                              qw_xml_att(node, "table:number-rows-spanned"),
                              "rowspan"))) return 0;
    if (!odt_html_literal(context->html, ">") ||
        !odt_render_nodes(context, qw_xml_down(node), depth + 1u) ||
        !odt_html_literal(context->html, "</") ||
        !odt_html_literal(context->html, html_tag) ||
        !odt_html_literal(context->html, ">")) return 0;
    return 1;
}

static int odt_render_nodes(OdtRenderContext *context, QwXmlNode *node,
                            size_t depth)
{
    if (depth > ODT_MAX_DEPTH) return 0;
    for (; node; node = qw_xml_next(node)) {
        const char *text;
        if (++context->visited_nodes > ODT_MAX_NODES) return 0;
        text = qw_xml_text(node);
        if (text) {
            if (!odt_html_escape(context->html, text)) return 0;
        } else if (!odt_render_element(context, node, depth)) {
            return 0;
        }
    }
    return 1;
}

static QwXmlNode *odt_find_descendant(QwXmlNode *node, const char *local_name,
                                   size_t depth)
{
    QwXmlNode *found;
    if (depth > ODT_MAX_DEPTH) return NULL;
    for (; node; node = qw_xml_next(node)) {
        if (odt_is_tag(node, local_name)) return node;
        found = odt_find_descendant(qw_xml_down(node), local_name, depth + 1u);
        if (found) return found;
    }
    return NULL;
}

static QwXmlNode *odt_parse_xml(const uint8_t *data, size_t size)
{
    return qw_xml_parse(data, size, NULL);
}

static void *odt_extract_xml(QwZipArchive *zip, const char *name,
                             size_t *out_size)
{
    size_t index = qw_zip_reader_locate_file(zip, name, NULL, 0);
    QwZipEntryInfo stat;
    size_t extracted_size = 0;
    void *contents;
    if (index == (size_t)-1 || !qw_zip_reader_file_stat(zip, index, &stat) ||
        !stat.uncompressed_size || stat.uncompressed_size > ODT_XML_LIMIT ||
        !qw_zip_reader_is_file_supported(zip, index)) return NULL;
    contents = qw_zip_reader_extract_to_heap(zip, index, &extracted_size, 0);
    if (!contents || extracted_size != stat.uncompressed_size) {
        free(contents);
        return NULL;
    }
    *out_size = extracted_size;
    return contents;
}

static int odt_set_error(QwConvertReport *report, int code,
                         const char *message)
{
    if (report && message) {
        snprintf(report->note, sizeof(report->note), "%s", message);
    }
    return code;
}

static int odt_build_layout_html(const void *data, size_t size,
                                QwInputFormat input_format,
                                OdtHtml *out_html,
                                size_t *out_skipped_images,
                                QwConvertReport *report)
{
    QwZipArchive zip;
    void *content_data = NULL;
    void *styles_data = NULL;
    size_t content_size = 0;
    size_t styles_size = 0;
    QwXmlNode *content_xml = NULL;
    QwXmlNode *styles_xml = NULL;
    QwXmlNode *office_text = NULL;
    OdtHtml html = {0};
    OdtStyles styles = {0};
    OdtRenderContext render;
    int rc = QW_STATUS_ERR_FORMAT;
    int archive_open = 0;
    int flat_xml = 0;
    if (!out_html || !out_skipped_images) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    memset(out_html, 0, sizeof(*out_html));
    *out_skipped_images = 0u;
    if (!data || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(&zip, 0, sizeof(zip));
    if (input_format == QW_FORMAT_FODT) {
        if (size > ODT_XML_LIMIT) {
            return odt_set_error(report, QW_STATUS_ERR_RESOURCE_LIMIT,
                                 "FODT exceeds the 64 MiB flat XML limit");
        }
        if (!odt_fodt_probe(data, size)) {
            return odt_set_error(report, QW_STATUS_ERR_FORMAT,
                                 "FODT is not a recognized OpenDocument text XML file");
        }
        content_data = malloc(size);
        if (!content_data) {
            return odt_set_error(report, QW_STATUS_ERR_OUT_OF_MEMORY,
                                 "Could not allocate FODT XML input");
        }
        memcpy(content_data, data, size);
        content_size = size;
        flat_xml = 1;
    } else {
#ifdef QUIREWISE_STANDALONE_ODT
        if (!qw_zip_reader_init_mem_with_profile(
                &zip, data, size, 0,
                QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE)) {
#else
        if (!qw_zip_reader_init_mem_with_profile(
                &zip, data, size, 0,
                qw_format_resource_profile_get(QW_FORMAT_ODT))) {
#endif
            return odt_set_error(report, QW_STATUS_ERR_FORMAT,
                                 "OpenDocument text package could not be opened as ZIP");
        }
        archive_open = 1;
        content_data = odt_extract_xml(&zip, "content.xml", &content_size);
        if (!content_data) {
            odt_set_error(report, QW_STATUS_ERR_FORMAT,
                          "OpenDocument content.xml is missing, unsupported, or too large");
            goto cleanup;
        }
        styles_data = odt_extract_xml(&zip, "styles.xml", &styles_size);
    }
    content_xml = odt_parse_xml((const uint8_t *)content_data, content_size);
    if (!content_xml) {
        odt_set_error(report, QW_STATUS_ERR_DECODE,
                      "OpenDocument content.xml could not be parsed");
        rc = QW_STATUS_ERR_DECODE;
        goto cleanup;
    }
    if (styles_data) {
        styles_xml = odt_parse_xml((const uint8_t *)styles_data, styles_size);
    }
    if ((styles_xml && !odt_collect_styles_node(
             qw_xml_root(styles_xml), &styles, 0u)) ||
        !odt_collect_styles_node(qw_xml_root(content_xml), &styles, 0u)) {
        odt_set_error(report, QW_STATUS_ERR_DECODE,
                      "ODT contains too many or excessively long style definitions");
        rc = QW_STATUS_ERR_DECODE;
        goto cleanup;
    }
    office_text = odt_find_descendant(qw_xml_root(content_xml), "text", 0u);
    if (!office_text) {
        odt_set_error(report, QW_STATUS_ERR_DECODE,
                      "ODT content.xml has no office text body");
        rc = QW_STATUS_ERR_DECODE;
        goto cleanup;
    }
    if (!odt_html_literal(&html,
            "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
            "@page{size:A4;margin:18mm}body{font-family:sans-serif;"
            "font-size:11pt;line-height:1.25;color:#111}"
            "table{border-collapse:collapse;width:100%}"
            "td{border:1px solid #888;padding:3pt;vertical-align:top}"
            "img{max-width:100%;height:auto}li{margin:2pt 0}"
            "</style></head><body>")) {
        odt_set_error(report, QW_STATUS_ERR_DECODE,
                      "Could not allocate ODT HTML output");
        rc = QW_STATUS_ERR_DECODE;
        goto cleanup;
    }
    memset(&render, 0, sizeof(render));
    render.zip = flat_xml ? NULL : &zip;
    render.html = &html;
    render.styles = &styles;
    if (!odt_render_nodes(&render, qw_xml_down(office_text), 0u)) {
        odt_set_error(report, QW_STATUS_ERR_DECODE,
                      "ODT document exceeds conversion limits or could not be rendered");
        rc = QW_STATUS_ERR_DECODE;
        goto cleanup;
    }
    if (!odt_html_literal(&html, "</body></html>")) {
        odt_set_error(report, QW_STATUS_ERR_DECODE,
                      "Could not finalize ODT HTML output");
        rc = QW_STATUS_ERR_DECODE;
        goto cleanup;
    }

    *out_html = html;
    memset(&html, 0, sizeof(html));
    *out_skipped_images = render.skipped_images;
    rc = QW_STATUS_OK;

cleanup:
    qw_xml_drop(content_xml);
    qw_xml_drop(styles_xml);
    free(html.data);
    free(styles.items);
    free(styles_data);
    free(content_data);
    if (archive_open) qw_zip_reader_end(&zip);
    return rc;
}

int qw_odt_native_open_layout(const void *data, size_t size,
                             QwInputFormat input_format,
                             const QwConvertOptions *options,
                             QwLayoutDocument *out_document,
                             QwConvertReport *report)
{
    OdtHtml html = {0};
    size_t skipped_images = 0u;
    int rc;
    (void)options;
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (input_format != QW_FORMAT_ODT && input_format != QW_FORMAT_OTT &&
        input_format != QW_FORMAT_FODT) return QW_STATUS_ERR_INVALID_ARGUMENT;
    rc = odt_build_layout_html(data, size, input_format, &html,
                               &skipped_images, report);
    if (rc != QW_STATUS_OK) return rc;
    out_document->html_utf8 = (const uint8_t *)html.data;
    out_document->html_size = html.size;
    out_document->context = html.data;
    out_document->dispose = free;
    if (report) {
        if (input_format == QW_FORMAT_OTT) {
            snprintf(report->note, sizeof(report->note),
                     "OTT template body opened through the ODT layout reader; template behavior and package metadata are not preserved%s",
                     skipped_images ? "; some images were skipped" : "");
        } else if (input_format == QW_FORMAT_FODT) {
            snprintf(report->note, sizeof(report->note),
                     "FODT flat XML text, styles, lists and tables opened as layout HTML; embedded images and advanced page-layout features are not preserved%s",
                     skipped_images ? "; some images were skipped" : "");
        } else {
            snprintf(report->note, sizeof(report->note),
                     "ODT text, headings, lists, tables and embedded images opened as layout HTML; advanced page-layout features are not mapped%s",
                     skipped_images ? "; some images were skipped" : "");
        }
    }
    return QW_STATUS_OK;
}

#if defined(QUIREWISE_WITH_MUPDF)
static int odt_convert_to_pdf(const void *data, size_t size,
                              const QwConvertOptions *options,
                              uint8_t **out_pdf, size_t *out_size,
                              QwConvertReport *report)
{
    OdtHtml html = {0};
    size_t skipped_images = 0u;
    int rc;
    (void)options;
    if (!out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    rc = odt_build_layout_html(data, size, QW_FORMAT_ODT, &html,
                               &skipped_images, report);
    if (rc == QW_STATUS_OK) {
        rc = qw_mupdf_convert_html_with_page_size(
            html.data, html.size, 595.0f, 842.0f,
            out_pdf, out_size, report);
    }
    if (rc == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "ODT text, headings, lists, tables and embedded images converted; advanced page-layout features are not mapped%s",
                 skipped_images ? "; some images were skipped" : "");
    }
    free(html.data);
    return rc;
}
#endif
#else
int qw_odt_native_open_layout(const void *data, size_t size,
                             QwInputFormat input_format,
                             const QwConvertOptions *options,
                             QwLayoutDocument *out_document,
                             QwConvertReport *report)
{
    (void)data;
    (void)size;
    (void)input_format;
    (void)options;
    if (out_document) memset(out_document, 0, sizeof(*out_document));
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "OpenDocument text layout parsing requires the statically linked MuPDF XML backend");
    }
    return QW_STATUS_ERR_UNSUPPORTED;
}

static int odt_convert_to_pdf(const void *data, size_t size,
                              const QwConvertOptions *options,
                              uint8_t **out_pdf, size_t *out_size,
                              QwConvertReport *report)
{
    (void)data;
    (void)size;
    (void)options;
    (void)out_pdf;
    (void)out_size;
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "ODT conversion requires the statically linked MuPDF backend");
    }
    return QW_STATUS_ERR_UNSUPPORTED;
}
#endif

#if !defined(QUIREWISE_STANDALONE_ODT)










const QwFormatDescriptor qw_format_odt = {
    .id = QW_FORMAT_ODT,
    .name = "ODT",
    .capabilities = (QW_FORMAT_CAP_CONTAINER | QW_FORMAT_CAP_MULTI_PAGE) | (QW_FORMAT_CAP_ARCHIVE_PROBE),
    .probe = NULL,
    .open_memory = NULL,
    .open_layout = qw_odt_native_open_layout,
    .open_audio = NULL,
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_DEFAULT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE,
    .convert_to_pdf = odt_convert_to_pdf
};

const QwFormatDescriptor qw_format_ott = {
    .id = QW_FORMAT_OTT,
    .name = "OTT",
    .capabilities = QW_FORMAT_CAP_CONTAINER | QW_FORMAT_CAP_MULTI_PAGE |
                    QW_FORMAT_CAP_ARCHIVE_PROBE,
    .probe = NULL,
    .open_memory = NULL,
    .open_layout = qw_odt_native_open_layout,
    .open_audio = NULL,
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_DEFAULT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE,
    .convert_to_pdf = NULL
};

const QwFormatDescriptor qw_format_fodt = {
    .id = QW_FORMAT_FODT,
    .name = "FODT",
    .capabilities = QW_FORMAT_CAP_MULTI_PAGE,
    .probe = odt_fodt_probe,
    .open_memory = NULL,
    .open_layout = qw_odt_native_open_layout,
    .open_audio = NULL,
    .max_input_bytes = QW_ODT_FLAT_XML_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .convert_to_pdf = NULL
};

#endif
