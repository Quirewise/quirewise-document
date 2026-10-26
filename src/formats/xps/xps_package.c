#include "xps_package.h"

#include "../../core/xml_parser.h"
#include "../archive/zip_native.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    XPS_PACKAGE_MAX_PAGES = 10000u,
    XPS_PACKAGE_MAX_PATH = 4096u,
    XPS_PACKAGE_MAX_XML_BYTES = 64u * 1024u * 1024u,
    XPS_PACKAGE_MAX_DEPTH = 128u
};

typedef struct QwXpsPackagePage {
    char *path;
    double width;
    double height;
} QwXpsPackagePage;

struct QwXpsPackage {
    QwZipArchive zip;
    QwXpsPackagePage *pages;
    size_t page_count;
    size_t page_capacity;
};

static int xps_package_error(QwConvertReport *report, int code,
                             const char *message)
{
    if (report && message) {
        (void)snprintf(report->note, sizeof(report->note), "%s", message);
    }
    return code;
}

static const char *xps_local_name(const QwXmlNode *node)
{
    const char *tag = qw_xml_tag(node);
    const char *colon;
    if (!tag) return NULL;
    colon = strrchr(tag, ':');
    return colon ? colon + 1u : tag;
}

static int xps_is_tag(const QwXmlNode *node, const char *name)
{
    const char *local = xps_local_name(node);
    return local && strcmp(local, name) == 0;
}

static int xps_parse_number(const char *text, double *out_value)
{
    char *end;
    double value;
    if (!text || !*text || !out_value) return 0;
    errno = 0;
    value = strtod(text, &end);
    if (end == text || errno == ERANGE || !isfinite(value)) return 0;
    while (*end && isspace((unsigned char)*end)) ++end;
    if (*end) return 0;
    *out_value = value;
    return 1;
}

static int xps_normalize_path(const char *base_path, const char *target,
                              char output[XPS_PACKAGE_MAX_PATH])
{
    char decoded[XPS_PACKAGE_MAX_PATH];
    char joined[XPS_PACKAGE_MAX_PATH * 2u];
    size_t decoded_size = 0u;
    size_t joined_size = 0u;
    size_t output_size = 0u;
    size_t segment_starts[256];
    size_t segment_count = 0u;
    const char *base_end;
    const char *cursor;
    int absolute;

    if (!target || !*target || !output) return 0;
    absolute = target[0] == '/';
    if (absolute && target[1] == '/') return 0;
    for (cursor = target; *cursor; ++cursor) {
        unsigned char value = (unsigned char)*cursor;
        if (value == '?' || value == '#') return 0;
        if (value == '%') {
            int high;
            int low;
            int first;
            int second;
            if (!cursor[1]) return 0;
            first = (unsigned char)cursor[1];
            if (!cursor[2]) return 0;
            second = (unsigned char)cursor[2];
            high = isdigit(first) ? first - '0'
                : tolower(first) >= 'a' && tolower(first) <= 'f'
                    ? tolower(first) - 'a' + 10 : -1;
            low = isdigit(second) ? second - '0'
                : tolower(second) >= 'a' && tolower(second) <= 'f'
                    ? tolower(second) - 'a' + 10 : -1;
            if (high < 0 || low < 0) return 0;
            value = (unsigned char)((high << 4) | low);
            cursor += 2;
        }
        if (!value || value == '\\' || value == ':' || value < 0x20u)
            return 0;
        if (decoded_size + 1u >= sizeof(decoded)) return 0;
        decoded[decoded_size++] = (char)value;
    }
    if (!decoded_size) return 0;
    decoded[decoded_size] = '\0';

    if (!absolute && base_path && *base_path) {
        base_end = strrchr(base_path, '/');
        if (base_end) {
            size_t directory_size = (size_t)(base_end - base_path) + 1u;
            if (directory_size + decoded_size >= sizeof(joined)) return 0;
            memcpy(joined, base_path, directory_size);
            joined_size = directory_size;
        }
    }
    if (joined_size + decoded_size >= sizeof(joined)) return 0;
    memcpy(joined + joined_size, decoded, decoded_size + 1u);

    output[0] = '\0';
    cursor = joined;
    while (*cursor) {
        const char *segment;
        size_t segment_size;
        size_t previous_size;
        while (*cursor == '/') ++cursor;
        if (!*cursor) break;
        segment = cursor;
        while (*cursor && *cursor != '/') ++cursor;
        segment_size = (size_t)(cursor - segment);
        if (segment_size == 1u && segment[0] == '.') continue;
        if (segment_size == 2u && segment[0] == '.' && segment[1] == '.') {
            if (!segment_count) return 0;
            output_size = segment_starts[--segment_count];
            output[output_size] = '\0';
            continue;
        }
        {
            size_t separator = output_size ? 1u : 0u;
            if (segment_count >= sizeof(segment_starts) /
                                     sizeof(segment_starts[0]) ||
                output_size > XPS_PACKAGE_MAX_PATH - 1u - separator ||
                segment_size > XPS_PACKAGE_MAX_PATH - 1u - output_size -
                    separator) return 0;
        }
        previous_size = output_size;
        if (output_size) output[output_size++] = '/';
        segment_starts[segment_count++] = previous_size;
        memcpy(output + output_size, segment, segment_size);
        output_size += segment_size;
        output[output_size] = '\0';
    }
    return output_size != 0u;
}

static int xps_extract_entry(QwXpsPackage *package, const char *path,
                             uint8_t **out_data, size_t *out_size)
{
    size_t entry_count;
    if (!package || !path || !out_data || !out_size)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    entry_count = qw_zip_reader_get_num_files(&package->zip);
    for (size_t i = 0u; i < entry_count; ++i) {
        const QwZipEntry *entry = &package->zip.entries[i];
        if (entry->is_directory || strcmp(entry->name, path) != 0) continue;
        if (entry->uncompressed_size > XPS_PACKAGE_MAX_XML_BYTES ||
            entry->uncompressed_size > SIZE_MAX) return QW_STATUS_ERR_RESOURCE_LIMIT;
        if (!qw_zip_reader_is_file_supported(&package->zip, i))
            return QW_STATUS_ERR_UNSUPPORTED;
        *out_data = (uint8_t *)qw_zip_reader_extract_to_heap(
            &package->zip, i, out_size, 0u);
        return *out_data ? QW_STATUS_OK : QW_STATUS_ERR_FORMAT;
    }
    return QW_STATUS_ERR_FORMAT;
}

static QwXmlNode *xps_parse_xml(const uint8_t *data, size_t size)
{
    QwXmlNode *document = qw_xml_parse(data, size, NULL);
    if (!document) return NULL;
    for (QwXmlNode *node = qw_xml_down(document); node;
         node = qw_xml_next(node)) {
        if (qw_xml_tag(node)) return document;
    }
    qw_xml_drop(document);
    return NULL;
}

static QwXmlNode *xps_find_descendant(QwXmlNode *nodes, const char *name,
                                      size_t depth)
{
    if (depth > XPS_PACKAGE_MAX_DEPTH) return NULL;
    for (QwXmlNode *node = nodes; node; node = qw_xml_next(node)) {
        QwXmlNode *found;
        if (xps_is_tag(node, name)) return node;
        found = xps_find_descendant(qw_xml_down(node), name, depth + 1u);
        if (found) return found;
    }
    return NULL;
}

static int xps_page_append(QwXpsPackage *package, const char *source,
                           const char *width_text, const char *height_text)
{
    QwXpsPackagePage page = {0};
    QwXpsPackagePage *grown;
    size_t capacity;
    if (!package || !source || !*source ||
        package->page_count >= XPS_PACKAGE_MAX_PAGES) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if ((width_text && !xps_parse_number(width_text, &page.width)) ||
        (height_text && !xps_parse_number(height_text, &page.height)) ||
        (width_text && !(page.width > 0.0 && page.width <= 19200.0)) ||
        (height_text && !(page.height > 0.0 && page.height <= 19200.0))) {
        return QW_STATUS_ERR_FORMAT;
    }
    page.path = (char *)malloc(strlen(source) + 1u);
    if (!page.path) return QW_STATUS_ERR_OUT_OF_MEMORY;
    strcpy(page.path, source);
    if (package->page_count == package->page_capacity) {
        capacity = package->page_capacity ? package->page_capacity * 2u : 32u;
        if (capacity > XPS_PACKAGE_MAX_PAGES) capacity = XPS_PACKAGE_MAX_PAGES;
        grown = (QwXpsPackagePage *)realloc(
            package->pages, capacity * sizeof(*grown));
        if (!grown) {
            free(page.path);
            return QW_STATUS_ERR_OUT_OF_MEMORY;
        }
        package->pages = grown;
        package->page_capacity = capacity;
    }
    package->pages[package->page_count++] = page;
    return QW_STATUS_OK;
}

static int xps_collect_page_contents(QwXpsPackage *package,
                                     const char *document_path,
                                     QwXmlNode *nodes,
                                     size_t depth)
{
    if (depth > XPS_PACKAGE_MAX_DEPTH) return QW_STATUS_ERR_RESOURCE_LIMIT;
    for (QwXmlNode *node = nodes; node; node = qw_xml_next(node)) {
        int rc;
        if (xps_is_tag(node, "PageContent")) {
            const char *source = qw_xml_att(node, "Source");
            char resolved[XPS_PACKAGE_MAX_PATH];
            if (!source || !xps_normalize_path(document_path, source, resolved))
                return QW_STATUS_ERR_FORMAT;
            rc = xps_page_append(package, resolved,
                                 qw_xml_att(node, "Width"),
                                 qw_xml_att(node, "Height"));
            if (rc != QW_STATUS_OK) return rc;
        } else {
            rc = xps_collect_page_contents(package, document_path,
                                           qw_xml_down(node), depth + 1u);
            if (rc != QW_STATUS_OK) return rc;
        }
    }
    return QW_STATUS_OK;
}

static int xps_parse_fixed_document(QwXpsPackage *package,
                                    const char *document_path)
{
    uint8_t *bytes = NULL;
    size_t size = 0u;
    QwXmlNode *xml;
    QwXmlNode *root;
    int rc = xps_extract_entry(package, document_path, &bytes, &size);
    if (rc != QW_STATUS_OK) return rc;
    xml = xps_parse_xml(bytes, size);
    free(bytes);
    if (!xml) return QW_STATUS_ERR_FORMAT;
    root = xps_find_descendant(qw_xml_down(xml), "FixedDocument", 0u);
    rc = root ? xps_collect_page_contents(package, document_path,
                                          qw_xml_down(root), 0u)
              : QW_STATUS_ERR_FORMAT;
    qw_xml_drop(xml);
    return rc;
}

static int xps_collect_documents(QwXpsPackage *package,
                                 const char *sequence_path,
                                 QwXmlNode *nodes,
                                 size_t depth)
{
    if (depth > XPS_PACKAGE_MAX_DEPTH) return QW_STATUS_ERR_RESOURCE_LIMIT;
    for (QwXmlNode *node = nodes; node; node = qw_xml_next(node)) {
        int rc;
        if (xps_is_tag(node, "DocumentReference")) {
            const char *source = qw_xml_att(node, "Source");
            char resolved[XPS_PACKAGE_MAX_PATH];
            if (!source || !xps_normalize_path(sequence_path, source, resolved))
                return QW_STATUS_ERR_FORMAT;
            rc = xps_parse_fixed_document(package, resolved);
            if (rc != QW_STATUS_OK) return rc;
        } else {
            rc = xps_collect_documents(package, sequence_path,
                                       qw_xml_down(node), depth + 1u);
            if (rc != QW_STATUS_OK) return rc;
        }
    }
    return QW_STATUS_OK;
}

static int xps_ascii_ends_with_folded(const char *text, const char *suffix)
{
    size_t text_size;
    size_t suffix_size;
    size_t offset;
    if (!text || !suffix) return 0;
    text_size = strlen(text);
    suffix_size = strlen(suffix);
    if (suffix_size > text_size) return 0;
    offset = text_size - suffix_size;
    for (size_t i = 0u; i < suffix_size; ++i) {
        unsigned char left = (unsigned char)text[offset + i];
        unsigned char right = (unsigned char)suffix[i];
        if (tolower(left) != tolower(right)) return 0;
    }
    return 1;
}

static int xps_find_sequence_relationship(QwXmlNode *nodes, size_t depth,
                                          char output[XPS_PACKAGE_MAX_PATH])
{
    if (depth > XPS_PACKAGE_MAX_DEPTH) return QW_STATUS_ERR_RESOURCE_LIMIT;
    for (QwXmlNode *node = nodes; node; node = qw_xml_next(node)) {
        int rc;
        if (xps_is_tag(node, "Relationship")) {
            const char *type = qw_xml_att(node, "Type");
            const char *target = qw_xml_att(node, "Target");
            const char *mode = qw_xml_att(node, "TargetMode");
            if (type && target &&
                (!mode || strcmp(mode, "External") != 0) &&
                xps_ascii_ends_with_folded(type, "/fixedrepresentation")) {
                return xps_normalize_path(NULL, target, output)
                    ? QW_STATUS_OK : QW_STATUS_ERR_FORMAT;
            }
        }
        rc = xps_find_sequence_relationship(qw_xml_down(node), depth + 1u,
                                            output);
        if (rc != QW_STATUS_ERR_FORMAT) return rc;
    }
    return QW_STATUS_ERR_FORMAT;
}

static int xps_collect_pages(QwXpsPackage *package)
{
    uint8_t *bytes = NULL;
    size_t size = 0u;
    char sequence_path[XPS_PACKAGE_MAX_PATH];
    QwXmlNode *xml;
    QwXmlNode *root;
    int rc = xps_extract_entry(package, "_rels/.rels", &bytes, &size);
    if (rc != QW_STATUS_OK) return rc;
    xml = xps_parse_xml(bytes, size);
    free(bytes);
    if (!xml) return QW_STATUS_ERR_FORMAT;
    rc = xps_find_sequence_relationship(qw_xml_down(xml), 0u, sequence_path);
    qw_xml_drop(xml);
    if (rc != QW_STATUS_OK) return rc;

    rc = xps_extract_entry(package, sequence_path, &bytes, &size);
    if (rc != QW_STATUS_OK) return rc;
    xml = xps_parse_xml(bytes, size);
    free(bytes);
    if (!xml) return QW_STATUS_ERR_FORMAT;
    root = xps_find_descendant(qw_xml_down(xml),
                               "FixedDocumentSequence", 0u);
    rc = root ? xps_collect_documents(package, sequence_path,
                                      qw_xml_down(root), 0u)
              : QW_STATUS_ERR_FORMAT;
    qw_xml_drop(xml);
    if (rc == QW_STATUS_OK && !package->page_count) return QW_STATUS_ERR_FORMAT;
    return rc;
}

int qw_xps_package_open(QwXpsPackage **out_package,
                        const void *data,
                        size_t size,
                        QwConvertReport *report)
{
    QwXpsPackage *package;
    int rc;
    if (!out_package || !data || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_package = NULL;
    package = (QwXpsPackage *)calloc(1u, sizeof(*package));
    if (!package) return QW_STATUS_ERR_OUT_OF_MEMORY;
    if (!qw_zip_reader_init_mem_with_profile(
            &package->zip, data, size, 0u,
            QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE)) {
        rc = xps_package_error(report, QW_STATUS_ERR_FORMAT,
                               "XPS package ZIP directory is invalid");
        qw_xps_package_close(package);
        return rc;
    }
    rc = xps_collect_pages(package);
    if (rc != QW_STATUS_OK) {
        if (rc == QW_STATUS_ERR_UNSUPPORTED) {
            xps_package_error(report, rc,
                              "XPS package uses an unsupported ZIP entry method");
        } else if (rc == QW_STATUS_ERR_RESOURCE_LIMIT) {
            xps_package_error(report, rc,
                              "XPS package exceeds a parser resource limit");
        } else if (rc == QW_STATUS_ERR_OUT_OF_MEMORY) {
            xps_package_error(report, rc,
                              "XPS package parser ran out of memory");
        } else {
            xps_package_error(report, QW_STATUS_ERR_FORMAT,
                              "XPS package relationships or page list are invalid");
        }
        qw_xps_package_close(package);
        return rc;
    }
    *out_package = package;
    return QW_STATUS_OK;
}

size_t qw_xps_package_page_count(const QwXpsPackage *package)
{
    return package ? package->page_count : 0u;
}

int qw_xps_package_read_page(QwXpsPackage *package,
                             size_t page_index,
                             uint8_t **out_data,
                             size_t *out_size,
                             double *out_width,
                             double *out_height,
                             QwConvertReport *report)
{
    int rc;
    if (!package || page_index >= package->page_count || !out_data || !out_size)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    rc = xps_extract_entry(package, package->pages[page_index].path,
                           out_data, out_size);
    if (rc != QW_STATUS_OK) {
        if (rc == QW_STATUS_ERR_UNSUPPORTED) {
            xps_package_error(report, rc,
                              "XPS page uses an unsupported ZIP entry method");
        } else if (rc == QW_STATUS_ERR_RESOURCE_LIMIT) {
            xps_package_error(report, rc,
                              "XPS page XML exceeds its parser byte limit");
        } else {
            xps_package_error(report, QW_STATUS_ERR_FORMAT,
                              "XPS page XML could not be extracted");
        }
        return rc;
    }
    if (out_width) *out_width = package->pages[page_index].width;
    if (out_height) *out_height = package->pages[page_index].height;
    return QW_STATUS_OK;
}

void qw_xps_package_close(QwXpsPackage *package)
{
    if (!package) return;
    for (size_t i = 0u; i < package->page_count; ++i) {
        free(package->pages[i].path);
    }
    free(package->pages);
    qw_zip_reader_end(&package->zip);
    free(package);
}
