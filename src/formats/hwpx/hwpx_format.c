#if !defined(QUIREWISE_STANDALONE_HWPX)
#include "format_registry.h"
#include "hwpx_format.h"
#include "../archive/archive_detect.h"
#endif
#include "quirewise/hwpx.h"
#include "quirewise/xml.h"
#include "../archive/zip_native.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HWPX_MAX_INPUT_BYTES ((size_t)256u * 1024u * 1024u)

enum {
    HWPX_XML_LIMIT = 16 * 1024 * 1024,
    HWPX_TOTAL_XML_LIMIT = 64 * 1024 * 1024,
    HWPX_HTML_LIMIT = 64 * 1024 * 1024,
    HWPX_MAX_MANIFEST_ITEMS = 512,
    HWPX_MAX_SECTIONS = 128,
    HWPX_MAX_XML_DEPTH = 128,
    HWPX_MAX_XML_NODES = 500000
};

typedef struct HwpxHtml {
    char *data;
    size_t size;
    size_t capacity;
} HwpxHtml;

struct QwHwpxDocument {
    HwpxHtml html;
    size_t section_count;
    size_t omitted_object_count;
};

typedef QwHwpxDocument HwpxOwner;

typedef struct HwpxManifestItem {
    char id[128];
    char href[1024];
    int is_section;
} HwpxManifestItem;

static int hwpx_html_append(HwpxHtml *html, const char *data, size_t size)
{
    size_t required;
    size_t capacity;
    char *grown;
    if (!html || (!data && size) || size > SIZE_MAX - html->size - 1u)
        return 0;
    required = html->size + size + 1u;
    if (required > HWPX_HTML_LIMIT) return 0;
    if (required > html->capacity) {
        capacity = html->capacity ? html->capacity : 1024u;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2u) return 0;
            capacity *= 2u;
        }
        if (capacity > HWPX_HTML_LIMIT) capacity = HWPX_HTML_LIMIT;
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

static int hwpx_html_literal(HwpxHtml *html, const char *text)
{
    return text && hwpx_html_append(html, text, strlen(text));
}

static int hwpx_html_escape(HwpxHtml *html, const char *text)
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
            if (!hwpx_html_append(html, (const char *)start,
                                  (size_t)(cursor - start)) ||
                !hwpx_html_literal(html, replacement)) return 0;
            start = cursor + 1u;
        }
        ++cursor;
    }
    return hwpx_html_append(html, (const char *)start,
                           (size_t)(cursor - start));
}

static const char *hwpx_local_name(const char *qualified_name)
{
    const char *colon;
    if (!qualified_name) return "";
    colon = strrchr(qualified_name, ':');
    return colon ? colon + 1u : qualified_name;
}

static int hwpx_is_tag(QwXmlNode *node, const char *local_name)
{
    const char *tag = qw_xml_tag(node);
    return tag && strcmp(hwpx_local_name(tag), local_name) == 0;
}

static int hwpx_copy_bounded(char *destination, size_t capacity,
                             const char *source)
{
    size_t length;
    if (!destination || !capacity || !source) return 0;
    length = strlen(source);
    if (!length || length >= capacity) return 0;
    memcpy(destination, source, length + 1u);
    return 1;
}

static int hwpx_safe_package_path(const char *path)
{
    const char *segment;
    const char *cursor;
    if (!path || !path[0] || path[0] == '/' || path[0] == '\\' ||
        strchr(path, '\\') || strchr(path, ':') || strchr(path, '%') ||
        strchr(path, '?') || strchr(path, '#')) return 0;
    segment = path;
    for (cursor = path; ; ++cursor) {
        unsigned char ch = (unsigned char)*cursor;
        if ((ch && ch < 0x20u) || ch == 0x7fu) return 0;
        if (ch == '/' || ch == '\0') {
            size_t segment_size = (size_t)(cursor - segment);
            if (!segment_size ||
                (segment_size == 1u && segment[0] == '.') ||
                (segment_size == 2u && segment[0] == '.' &&
                 segment[1] == '.')) return 0;
            if (!ch) break;
            segment = cursor + 1u;
        }
    }
    return 1;
}

static int hwpx_join_manifest_href(const char *package_path,
                                   const char *href,
                                   char *out_path,
                                   size_t out_capacity)
{
    const char *slash;
    size_t directory_size;
    size_t href_size;
    if (!package_path || !href || !out_path || !out_capacity ||
        !hwpx_safe_package_path(package_path) ||
        !hwpx_safe_package_path(href)) return 0;
    if (strncmp(href, "Contents/", 9u) == 0) {
        return hwpx_copy_bounded(out_path, out_capacity, href);
    }
    slash = strrchr(package_path, '/');
    directory_size = slash ? (size_t)(slash - package_path + 1u) : 0u;
    href_size = strlen(href);
    if (directory_size > out_capacity - 1u ||
        href_size > out_capacity - directory_size - 1u) return 0;
    if (directory_size) memcpy(out_path, package_path, directory_size);
    memcpy(out_path + directory_size, href, href_size + 1u);
    return hwpx_safe_package_path(out_path);
}

static int hwpx_is_section_path(const char *path)
{
    const char *name;
    const char *cursor;
    if (!path) return 0;
    name = strrchr(path, '/');
    name = name ? name + 1u : path;
    if (strncmp(name, "section", 7u) != 0) return 0;
    cursor = name + 7u;
    if (*cursor < '0' || *cursor > '9') return 0;
    while (*cursor >= '0' && *cursor <= '9') ++cursor;
    return strcmp(cursor, ".xml") == 0;
}

static void *hwpx_extract_entry(QwZipArchive *zip, const char *name,
                                size_t max_size, size_t *out_size)
{
    size_t index;
    size_t extracted_size = 0u;
    QwZipEntryInfo stat;
    void *contents;
    if (out_size) *out_size = 0u;
    if (!zip || !name || !out_size || !hwpx_safe_package_path(name))
        return NULL;
    index = qw_zip_reader_locate_file(zip, name, NULL, QW_ZIP_CASE_SENSITIVE);
    if (index == (size_t)-1 ||
        !qw_zip_reader_file_stat(zip, index, &stat) ||
        !stat.uncompressed_size || stat.uncompressed_size > max_size ||
        !qw_zip_reader_is_file_supported(zip, index)) return NULL;
    contents = qw_zip_reader_extract_to_heap(zip, index, &extracted_size, 0u);
    if (!contents || extracted_size != stat.uncompressed_size ||
        extracted_size > max_size) {
        free(contents);
        return NULL;
    }
    *out_size = extracted_size;
    return contents;
}

static QwXmlNode *hwpx_parse_xml(const uint8_t *data, size_t size,
                                 QwXmlError *error)
{
    return qw_xml_parse(data, size, error);
}

static QwXmlNode *hwpx_find_descendant(QwXmlNode *node, const char *local_name,
                                    size_t depth, size_t *visited)
{
    QwXmlNode *found;
    if (depth > HWPX_MAX_XML_DEPTH || !visited) return NULL;
    for (; node; node = qw_xml_next(node)) {
        if (++*visited > HWPX_MAX_XML_NODES) return NULL;
        if (hwpx_is_tag(node, local_name)) return node;
        found = hwpx_find_descendant(qw_xml_down(node), local_name,
                                     depth + 1u, visited);
        if (found || *visited > HWPX_MAX_XML_NODES) return found;
    }
    return NULL;
}

static int hwpx_render_nodes(HwpxHtml *html, QwXmlNode *node, size_t depth,
                             size_t *visited, size_t *omitted_objects)
{
    if (depth > HWPX_MAX_XML_DEPTH || !visited || !omitted_objects) return 0;
    for (; node; node = qw_xml_next(node)) {
        const char *text;
        const char *tag;
        const char *open = NULL;
        const char *close = NULL;
        if (++*visited > HWPX_MAX_XML_NODES) return 0;
        text = qw_xml_text(node);
        if (text) {
            if (!hwpx_html_escape(html, text)) return 0;
            continue;
        }
        tag = hwpx_local_name(qw_xml_tag(node));
        if (strcmp(tag, "p") == 0) {
            open = "<p>";
            close = "</p>\n";
        } else if (strcmp(tag, "tbl") == 0) {
            open = "<table>";
            close = "</table>\n";
        } else if (strcmp(tag, "tr") == 0) {
            open = "<tr>";
            close = "</tr>";
        } else if (strcmp(tag, "tc") == 0) {
            open = "<td>";
            close = "</td>";
        } else if (strcmp(tag, "lineBreak") == 0 ||
                   strcmp(tag, "br") == 0) {
            if (!hwpx_html_literal(html, "<br/>")) return 0;
            continue;
        } else if (strcmp(tag, "tab") == 0) {
            if (!hwpx_html_literal(html, "&#9;")) return 0;
            continue;
        } else if (strcmp(tag, "pic") == 0 ||
                   strcmp(tag, "ole") == 0 ||
                   strcmp(tag, "chart") == 0 ||
                   strcmp(tag, "equation") == 0) {
            ++*omitted_objects;
            if (!hwpx_html_literal(html, "<span>[embedded object omitted]</span>"))
                return 0;
            continue;
        }
        if (open && !hwpx_html_literal(html, open)) return 0;
        if (!hwpx_render_nodes(html, qw_xml_down(node), depth + 1u,
                               visited, omitted_objects)) return 0;
        if (close && !hwpx_html_literal(html, close)) return 0;
    }
    return 1;
}

static int hwpx_manifest_items(QwXmlNode *manifest, const char *package_path,
                               HwpxManifestItem **out_items,
                               size_t *out_count)
{
    HwpxManifestItem *items;
    size_t count = 0u;
    QwXmlNode *node;
    if (!manifest || !out_items || !out_count) return 0;
    *out_items = NULL;
    *out_count = 0u;
    items = (HwpxManifestItem *)calloc(HWPX_MAX_MANIFEST_ITEMS,
                                       sizeof(*items));
    if (!items) return 0;
    for (node = qw_xml_down(manifest); node; node = qw_xml_next(node)) {
        const char *id;
        const char *href;
        HwpxManifestItem item;
        if (!hwpx_is_tag(node, "item")) continue;
        id = qw_xml_att(node, "id");
        href = qw_xml_att(node, "href");
        memset(&item, 0, sizeof(item));
        if (!id || !href || count == HWPX_MAX_MANIFEST_ITEMS ||
            !hwpx_copy_bounded(item.id, sizeof(item.id), id) ||
            !hwpx_join_manifest_href(package_path, href, item.href,
                                     sizeof(item.href))) {
            free(items);
            return 0;
        }
        for (size_t i = 0u; i < count; ++i) {
            if (strcmp(items[i].id, item.id) == 0) {
                free(items);
                return 0;
            }
        }
        item.is_section = hwpx_is_section_path(item.href);
        items[count++] = item;
    }
    if (!count) {
        free(items);
        return 0;
    }
    *out_items = items;
    *out_count = count;
    return 1;
}

static const HwpxManifestItem *hwpx_manifest_find(
    const HwpxManifestItem *items, size_t count, const char *id)
{
    for (size_t i = 0u; i < count; ++i) {
        if (id && strcmp(items[i].id, id) == 0) return &items[i];
    }
    return NULL;
}

#if !defined(QUIREWISE_STANDALONE_HWPX)
static void hwpx_owner_dispose(void *context)
{
    qw_hwpx_document_free((QwHwpxDocument *)context);
}
#endif

static void hwpx_set_error(char *error, size_t error_capacity,
                           const char *message)
{
    if (error && error_capacity) {
        (void)snprintf(error, error_capacity, "%s",
                       message ? message : "HWPX parsing failed");
    }
}

static QwHwpxStatus hwpx_parse_archive(
    const void *input, size_t size,
    const QwFormatInputResourceProfile *resource_profile,
    QwHwpxDocument **out_document, char *error, size_t error_capacity)
{
    QwZipArchive zip;
    HwpxOwner *owner = NULL;
    HwpxManifestItem *items = NULL;
    size_t item_count = 0u;
    size_t xml_bytes = 0u;
    size_t sections = 0u;
    size_t omitted_objects = 0u;
    size_t visited = 0u;
    void *container_data = NULL;
    void *package_data = NULL;
    void *section_data = NULL;
    size_t container_size = 0u;
    size_t package_size = 0u;
    size_t section_size = 0u;
    QwXmlNode *container_xml = NULL;
    QwXmlNode *package_xml = NULL;
    QwXmlNode *section_xml = NULL;
    int archive_open = 0;
    QwHwpxStatus rc = QW_HWPX_ERR_FORMAT;
    char package_path[1024];
    QwXmlError xml_error;
    if (!out_document) return QW_HWPX_ERR_INVALID_ARGUMENT;
    *out_document = NULL;
    if (!input || !size) return QW_HWPX_ERR_INVALID_ARGUMENT;
    if (size > HWPX_MAX_INPUT_BYTES) {
        hwpx_set_error(error, error_capacity,
                       "HWPX input exceeds the parser limit");
        return QW_HWPX_ERR_RESOURCE_LIMIT;
    }
    memset(&zip, 0, sizeof(zip));
    if (resource_profile) {
        archive_open = qw_zip_reader_init_mem_with_profile(
            &zip, input, size, 0u, resource_profile);
    } else {
        archive_open = qw_zip_reader_init_mem(&zip, input, size, 0u);
    }
    if (!archive_open) {
        hwpx_set_error(error, error_capacity, "HWPX ZIP package is invalid");
        return QW_HWPX_ERR_FORMAT;
    }
    owner = (HwpxOwner *)calloc(1u, sizeof(*owner));
    if (!owner) {
        rc = QW_HWPX_ERR_OUT_OF_MEMORY;
        hwpx_set_error(error, error_capacity, "Cannot allocate HWPX document");
        goto cleanup;
    }
    container_data = hwpx_extract_entry(&zip, "META-INF/container.xml",
                                        1024u * 1024u, &container_size);
    if (!container_data) {
        rc = QW_HWPX_ERR_FORMAT;
        hwpx_set_error(error, error_capacity,
                       "HWPX package has no readable META-INF/container.xml");
        goto cleanup;
    }
    xml_bytes = container_size;
    package_path[0] = '\0';
    container_xml = hwpx_parse_xml((const uint8_t *)container_data,
                                   container_size, &xml_error);
    if (!container_xml) {
        rc = xml_error.code == QW_XML_ERROR_RESOURCE_LIMIT
                 ? QW_HWPX_ERR_RESOURCE_LIMIT : QW_HWPX_ERR_DECODE;
        hwpx_set_error(error, error_capacity, xml_error.message);
        goto cleanup;
    }
    {
        QwXmlNode *rootfile;
        QwXmlNode *package_root;
        QwXmlNode *manifest;
        QwXmlNode *spine;
        size_t metadata_nodes = 0u;
        int rendered_any = 0;
        rootfile = hwpx_find_descendant(qw_xml_root(container_xml),
                                        "rootfile", 0u, &metadata_nodes);
        if (!rootfile ||
            !qw_xml_att(rootfile, "media-type") ||
            strcmp(qw_xml_att(rootfile, "media-type"),
                   "application/hwpml-package+xml") != 0 ||
            !hwpx_copy_bounded(package_path, sizeof(package_path),
                               qw_xml_att(rootfile, "full-path")) ||
            !hwpx_safe_package_path(package_path)) {
            rc = QW_HWPX_ERR_FORMAT;
            hwpx_set_error(error, error_capacity,
                           "HWPX container has no safe OWPML package rootfile");
            goto cleanup;
        }
        package_data = hwpx_extract_entry(&zip, package_path,
                                          HWPX_XML_LIMIT, &package_size);
        if (!package_data ||
            package_size > HWPX_TOTAL_XML_LIMIT - xml_bytes) {
            rc = package_size > HWPX_TOTAL_XML_LIMIT - xml_bytes
                     ? QW_HWPX_ERR_RESOURCE_LIMIT : QW_HWPX_ERR_FORMAT;
            hwpx_set_error(error, error_capacity,
                           "HWPX package manifest is missing or exceeds the XML limit");
            goto cleanup;
        }
        xml_bytes += package_size;
        package_xml = hwpx_parse_xml((const uint8_t *)package_data,
                                     package_size, &xml_error);
        if (!package_xml) {
            rc = xml_error.code == QW_XML_ERROR_RESOURCE_LIMIT
                     ? QW_HWPX_ERR_RESOURCE_LIMIT : QW_HWPX_ERR_DECODE;
            hwpx_set_error(error, error_capacity, xml_error.message);
            goto cleanup;
        }
        package_root = qw_xml_root(package_xml);
        manifest = hwpx_find_descendant(package_root, "manifest", 0u,
                                        &metadata_nodes);
        spine = hwpx_find_descendant(package_root, "spine", 0u,
                                     &metadata_nodes);
        if (!manifest || !spine ||
            !hwpx_manifest_items(manifest, package_path, &items,
                                 &item_count)) {
            rc = QW_HWPX_ERR_FORMAT;
            hwpx_set_error(error, error_capacity,
                           "HWPX package manifest or spine is invalid");
            goto cleanup;
        }
        if (!hwpx_html_literal(&owner->html,
                "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
                "@page{size:A4;margin:20mm}body{font-family:sans-serif;"
                "font-size:11pt;line-height:1.25;color:#111}"
                "table{border-collapse:collapse;max-width:100%}"
                "td{border:1px solid #777;padding:3pt;vertical-align:top}"
                "p{margin:0 0 6pt}span{white-space:pre-wrap}"
                "</style></head><body>")) {
            rc = QW_HWPX_ERR_RESOURCE_LIMIT;
            hwpx_set_error(error, error_capacity,
                           "HWPX HTML output exceeds the conversion limit");
            goto cleanup;
        }
        for (QwXmlNode *ref = qw_xml_down(spine); ref; ref = qw_xml_next(ref)) {
            const HwpxManifestItem *item;
            const char *idref;
            QwXmlNode *section_root;
            if (!hwpx_is_tag(ref, "itemref")) continue;
            idref = qw_xml_att(ref, "idref");
            item = hwpx_manifest_find(items, item_count, idref);
            if (!item) {
                rc = QW_HWPX_ERR_FORMAT;
                hwpx_set_error(error, error_capacity,
                               "HWPX spine references an unknown package item");
                goto cleanup;
            }
            if (!item->is_section) continue;
            if (sections == HWPX_MAX_SECTIONS) {
                rc = QW_HWPX_ERR_RESOURCE_LIMIT;
                hwpx_set_error(error, error_capacity,
                               "HWPX exceeds the section count limit");
                goto cleanup;
            }
            section_data = hwpx_extract_entry(&zip, item->href,
                                               HWPX_XML_LIMIT, &section_size);
            if (!section_data ||
                section_size > HWPX_TOTAL_XML_LIMIT - xml_bytes) {
                rc = section_size > HWPX_TOTAL_XML_LIMIT - xml_bytes
                         ? QW_HWPX_ERR_RESOURCE_LIMIT : QW_HWPX_ERR_FORMAT;
                hwpx_set_error(error, error_capacity,
                               "HWPX section is missing or exceeds the XML limit");
                goto cleanup;
            }
            xml_bytes += section_size;
            section_xml = hwpx_parse_xml((const uint8_t *)section_data,
                                         section_size, &xml_error);
            if (!section_xml) {
                rc = xml_error.code == QW_XML_ERROR_RESOURCE_LIMIT
                         ? QW_HWPX_ERR_RESOURCE_LIMIT : QW_HWPX_ERR_DECODE;
                hwpx_set_error(error, error_capacity, xml_error.message);
                goto cleanup;
            }
            section_root = qw_xml_root(section_xml);
            if (!section_root || !hwpx_html_literal(&owner->html, "<section>") ||
                !hwpx_render_nodes(&owner->html, qw_xml_down(section_root),
                                   0u, &visited, &omitted_objects) ||
                !hwpx_html_literal(&owner->html, "</section>")) {
                rc = QW_HWPX_ERR_RESOURCE_LIMIT;
                hwpx_set_error(error, error_capacity,
                               "HWPX section contains unsupported or excessive XML content");
                goto cleanup;
            }
            qw_xml_drop(section_xml);
            section_xml = NULL;
            free(section_data);
            section_data = NULL;
            section_size = 0u;
            ++sections;
            rendered_any = 1;
        }
        if (!rendered_any || !hwpx_html_literal(&owner->html,
                                                "</body></html>")) {
            rc = rendered_any ? QW_HWPX_ERR_RESOURCE_LIMIT : QW_HWPX_ERR_FORMAT;
            hwpx_set_error(error, error_capacity,
                           "HWPX package contains no readable body sections");
            goto cleanup;
        }
    }
    owner->section_count = sections;
    owner->omitted_object_count = omitted_objects;
    *out_document = owner;
    owner = NULL;
    rc = QW_HWPX_OK;

cleanup:
    qw_xml_drop(section_xml);
    qw_xml_drop(package_xml);
    qw_xml_drop(container_xml);
    if (archive_open) qw_zip_reader_end(&zip);
    free(section_data);
    free(package_data);
    free(container_data);
    free(items);
    qw_hwpx_document_free(owner);
    return rc;
}

#if !defined(QUIREWISE_STANDALONE_HWPX)
static int hwpx_native_open_layout(const void *input, size_t size,
                                   QwInputFormat input_format,
                                   const QwConvertOptions *options,
                                   QwLayoutDocument *out_document,
                                   QwConvertReport *report)
{
    QwHwpxDocument *document = NULL;
    QwHwpxStatus status;
    QwInputFormat detected_format;
    char error[160] = {0};
    (void)options;
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (input_format != QW_FORMAT_HWPX || !input || !size)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    detected_format = qw_archive_detect_format(input, size);
    if (detected_format != QW_FORMAT_HWPX) return QW_STATUS_ERR_FORMAT;
    status = hwpx_parse_archive(input, size,
        qw_format_resource_profile_get(QW_FORMAT_HWPX), &document,
        error, sizeof(error));
    if (status != QW_HWPX_OK) {
        if (report) (void)snprintf(report->note, sizeof(report->note),
                                   "HWPX parsing failed: %.120s", error);
        switch (status) {
        case QW_HWPX_ERR_INVALID_ARGUMENT: return QW_STATUS_ERR_INVALID_ARGUMENT;
        case QW_HWPX_ERR_OUT_OF_MEMORY: return QW_STATUS_ERR_OUT_OF_MEMORY;
        case QW_HWPX_ERR_UNSUPPORTED: return QW_STATUS_ERR_UNSUPPORTED;
        case QW_HWPX_ERR_RESOURCE_LIMIT: return QW_STATUS_ERR_RESOURCE_LIMIT;
        case QW_HWPX_ERR_DECODE: return QW_STATUS_ERR_DECODE;
        default: return QW_STATUS_ERR_FORMAT;
        }
    }
    out_document->html_utf8 = document->html.data;
    out_document->html_size = document->html.size;
    out_document->context = document;
    out_document->dispose = hwpx_owner_dispose;
    if (report) {
        (void)snprintf(report->note, sizeof(report->note),
            "HWPX OWPML spine order preserved across %zu section(s); paragraph text and simple tables mapped to layout HTML; styles, pagination, images, headers, footers and metadata are not preserved%s",
            document->section_count, document->omitted_object_count
                ? "; embedded objects omitted" : "");
    }
    return QW_STATUS_OK;
}

int qw_hwpx_native_open_layout(const void *data, size_t size,
                               QwInputFormat input_format,
                               const QwConvertOptions *options,
                               QwLayoutDocument *out_document,
                               QwConvertReport *report)
{
    (void)options;
    return hwpx_native_open_layout(data, size, input_format, options,
                                   out_document, report);
}
#endif

QwHwpxStatus qw_hwpx_parse(const void *data, size_t size,
                           QwHwpxDocument **out_document)
{
    return hwpx_parse_archive(data, size, NULL, out_document, NULL, 0u);
}

const uint8_t *qw_hwpx_document_html(const QwHwpxDocument *document,
                                     size_t *out_size)
{
    if (out_size) *out_size = document ? document->html.size : 0u;
    return document ? (const uint8_t *)document->html.data : NULL;
}

size_t qw_hwpx_document_section_count(const QwHwpxDocument *document)
{
    return document ? document->section_count : 0u;
}

size_t qw_hwpx_document_omitted_object_count(const QwHwpxDocument *document)
{
    return document ? document->omitted_object_count : 0u;
}

void qw_hwpx_document_free(QwHwpxDocument *document)
{
    if (!document) return;
    free(document->html.data);
    free(document);
}

#if !defined(QUIREWISE_STANDALONE_HWPX)
const QwFormatDescriptor qw_format_hwpx = {
    .id = QW_FORMAT_HWPX,
    .name = "HWPX OWPML document",
    .capabilities = QW_FORMAT_CAP_CONTAINER | QW_FORMAT_CAP_MULTI_PAGE |
                    QW_FORMAT_CAP_ARCHIVE_PROBE,
    .probe = NULL,
    .open_memory = NULL,
    .open_layout = qw_hwpx_native_open_layout,
    .open_audio = NULL,
    .max_input_bytes = HWPX_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE,
    .convert_to_pdf = NULL
};
#endif
