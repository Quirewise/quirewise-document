#if !defined(QUIREWISE_STANDALONE_ODP)
#include "../../core/format_registry.h"
#endif
#include "../../core/html_output.h"
#include "quirewise/odf_presentation.h"
#include "quirewise/xml.h"
#include "../archive/zip_native.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ODP_FLAT_PROBE_BYTES = 8192,
    ODP_MAX_XML_BYTES = 32 * 1024 * 1024,
    ODP_MAX_FLAT_XML_BYTES = 64 * 1024 * 1024,
    ODP_MAX_HTML_BYTES = 64 * 1024 * 1024,
    ODP_MAX_SLIDES = 1000,
    ODP_MAX_XML_NODES = 2000000,
    ODP_MAX_XML_DEPTH = 128,
    ODP_MAX_SPACES_PER_RUN = 4096
};

static int odp_bytes_contain(const uint8_t *data, size_t size,
                             const char *pattern, size_t pattern_size)
{
    size_t i;
    if (!data || !pattern || !pattern_size || pattern_size > size) return 0;
    for (i = 0u; i <= size - pattern_size; ++i) {
        if (memcmp(data + i, pattern, pattern_size) == 0) return 1;
    }
    return 0;
}

static int odp_flat_probe(const void *data, size_t size)
{
    static const char office_ns[] =
        "urn:oasis:names:tc:opendocument:xmlns:office:1.0";
    static const char mime_attr[] = "office:mimetype";
    static const char presentation_mime[] =
        "application/vnd.oasis.opendocument.presentation";
    static const char template_mime[] =
        "application/vnd.oasis.opendocument.presentation-template";
    const uint8_t *bytes = (const uint8_t *)data;
    size_t probe_size = size < ODP_FLAT_PROBE_BYTES ? size : ODP_FLAT_PROBE_BYTES;
    size_t i;
    size_t root_end = 0u;
    if (!bytes || !size || size > ODP_MAX_FLAT_XML_BYTES ||
        (size >= 4u && bytes[0] == 'P' && bytes[1] == 'K' &&
         ((bytes[2] == 3u && bytes[3] == 4u) ||
          (bytes[2] == 5u && bytes[3] == 6u) ||
          (bytes[2] == 7u && bytes[3] == 8u)))) return 0;
    for (i = 0u; i < probe_size; ++i) {
        if (bytes[i] == '<' && i + 1u < probe_size &&
            bytes[i + 1u] != '?' && bytes[i + 1u] != '!' &&
            bytes[i + 1u] != '/') {
            size_t j;
            for (j = i + 1u; j < probe_size; ++j) {
                if (bytes[j] == '>') {
                    root_end = j + 1u;
                    break;
                }
            }
            break;
        }
    }
    if (!root_end) return 0;
    return odp_bytes_contain(bytes, root_end, office_ns, sizeof(office_ns) - 1u) &&
           odp_bytes_contain(bytes, root_end, mime_attr, sizeof(mime_attr) - 1u) &&
           (odp_bytes_contain(bytes, root_end, presentation_mime,
                              sizeof(presentation_mime) - 1u) ||
            odp_bytes_contain(bytes, root_end, template_mime,
                              sizeof(template_mime) - 1u));
}

typedef struct OdpReader {
    QwZipArchive *zip;
    QwTextBuffer html;
    size_t visited_nodes;
    size_t slide_count;
} OdpReader;

struct QwOdpDocument {
    QwTextBuffer html;
    size_t slide_count;
};

static const char *odp_local_name(const char *qualified_name)
{
    const char *colon;
    if (!qualified_name) return "";
    colon = strrchr(qualified_name, ':');
    return colon ? colon + 1u : qualified_name;
}

static int odp_is_tag(QwXmlNode *node, const char *local_name)
{
    const char *tag = qw_xml_tag(node);
    return tag && strcmp(odp_local_name(tag), local_name) == 0;
}

static int odp_html_literal(OdpReader *reader, const char *text)
{
    return reader && qw_html_output_literal(&reader->html, text,
                                             ODP_MAX_HTML_BYTES, NULL);
}

static int odp_html_escape(OdpReader *reader, const char *text)
{
    return reader && qw_html_output_escape(&reader->html, text,
                                            ODP_MAX_HTML_BYTES, NULL);
}

static void *odp_extract_xml(OdpReader *reader, const char *name,
                             size_t *out_size)
{
    size_t index;
    size_t extracted_size = 0u;
    QwZipEntryInfo stat;
    void *contents;
    if (out_size) *out_size = 0u;
    if (!reader || !reader->zip || !out_size) return NULL;
    index = qw_zip_reader_locate_file(reader->zip, name, NULL,
                                      QW_ZIP_CASE_SENSITIVE);
    if (index == (size_t)-1 ||
        !qw_zip_reader_file_stat(reader->zip, index, &stat) ||
        !stat.uncompressed_size || stat.uncompressed_size > ODP_MAX_XML_BYTES ||
        !qw_zip_reader_is_file_supported(reader->zip, index)) return NULL;
    contents = qw_zip_reader_extract_to_heap(reader->zip, index,
                                              &extracted_size, 0u);
    if (!contents || extracted_size != stat.uncompressed_size ||
        extracted_size > ODP_MAX_XML_BYTES) {
        free(contents);
        return NULL;
    }
    *out_size = extracted_size;
    return contents;
}

static QwXmlNode *odp_parse_xml(const uint8_t *data, size_t size,
                                QwXmlError *error)
{
    return qw_xml_parse(data, size, error);
}

static QwXmlNode *odp_find_descendant(OdpReader *reader, QwXmlNode *node,
                                   const char *local_name, size_t depth)
{
    QwXmlNode *found;
    if (!reader || depth > ODP_MAX_XML_DEPTH) return NULL;
    for (; node; node = qw_xml_next(node)) {
        if (++reader->visited_nodes > ODP_MAX_XML_NODES) return NULL;
        if (odp_is_tag(node, local_name)) return node;
        found = odp_find_descendant(reader, qw_xml_down(node), local_name,
                                    depth + 1u);
        if (found || reader->visited_nodes > ODP_MAX_XML_NODES) return found;
    }
    return NULL;
}

static int odp_parse_space_count(const char *text, size_t *out_count)
{
    unsigned long value;
    char *end = NULL;
    if (!out_count) return 0;
    *out_count = 1u;
    if (!text || !*text) return 1;
    if (*text < '0' || *text > '9') return 0;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || !end || *end || value == 0u ||
        value > ODP_MAX_SPACES_PER_RUN) return 0;
    *out_count = (size_t)value;
    return 1;
}

static int odp_append_spaces(OdpReader *reader, size_t count)
{
    size_t i;
    for (i = 0u; i < count; ++i) {
        if (!odp_html_literal(reader, " ")) return 0;
    }
    return 1;
}

static int odp_render_inline(OdpReader *reader, QwXmlNode *node, size_t depth)
{
    if (depth > ODP_MAX_XML_DEPTH) return 0;
    for (; node; node = qw_xml_next(node)) {
        const char *text;
        if (++reader->visited_nodes > ODP_MAX_XML_NODES) return 0;
        text = qw_xml_text(node);
        if (text) {
            if (!odp_html_escape(reader, text)) return 0;
        } else if (odp_is_tag(node, "line-break")) {
            if (!odp_html_literal(reader, "<br>")) return 0;
        } else if (odp_is_tag(node, "tab")) {
            if (!odp_html_literal(reader, "&nbsp;&nbsp;&nbsp;")) return 0;
        } else if (odp_is_tag(node, "s")) {
            size_t count;
            if (!odp_parse_space_count(qw_xml_att(node, "text:c"), &count) ||
                !odp_append_spaces(reader, count)) return 0;
        } else if (!odp_render_inline(reader, qw_xml_down(node), depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

static int odp_render_blocks(OdpReader *reader, QwXmlNode *node, size_t depth)
{
    if (depth > ODP_MAX_XML_DEPTH) return 0;
    for (; node; node = qw_xml_next(node)) {
        const char *tag;
        if (++reader->visited_nodes > ODP_MAX_XML_NODES) return 0;
        if (odp_is_tag(node, "notes") || odp_is_tag(node, "page")) continue;
        if (odp_is_tag(node, "p") || odp_is_tag(node, "h")) {
            tag = odp_is_tag(node, "h") ? "h3" : "p";
            if (!odp_html_literal(reader, "<") ||
                !odp_html_literal(reader, tag) ||
                !odp_html_literal(reader, ">") ||
                !odp_render_inline(reader, qw_xml_down(node), depth + 1u) ||
                !odp_html_literal(reader, "</") ||
                !odp_html_literal(reader, tag) ||
                !odp_html_literal(reader, ">")) return 0;
        } else if (odp_is_tag(node, "list")) {
            if (!odp_html_literal(reader, "<ul>") ||
                !odp_render_blocks(reader, qw_xml_down(node), depth + 1u) ||
                !odp_html_literal(reader, "</ul>")) return 0;
        } else if (odp_is_tag(node, "list-item")) {
            if (!odp_html_literal(reader, "<li>") ||
                !odp_render_blocks(reader, qw_xml_down(node), depth + 1u) ||
                !odp_html_literal(reader, "</li>")) return 0;
        } else if (odp_is_tag(node, "image") || odp_is_tag(node, "object")) {
            if (!odp_html_literal(reader, "<p class=\"omitted\">[embedded object omitted]</p>"))
                return 0;
        } else if (!odp_render_blocks(reader, qw_xml_down(node), depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

static int odp_render_slide(OdpReader *reader, QwXmlNode *slide)
{
    const char *name = qw_xml_att(slide, "draw:name");
    size_t slide_number = reader->slide_count + 1u;
    if (reader->slide_count >= ODP_MAX_SLIDES) return 0;
    ++reader->slide_count;
    if (!odp_html_literal(reader, "<section class=\"slide\"><header><h2>Slide "))
        return 0;
    {
        char number[24];
        (void)snprintf(number, sizeof(number), "%zu", slide_number);
        if (!odp_html_escape(reader, number)) return 0;
    }
    if (name && *name && (!odp_html_literal(reader, " — ") ||
                          !odp_html_escape(reader, name))) return 0;
    if (!odp_html_literal(reader, "</h2></header><div class=\"slide-content\">"))
        return 0;
    if (!odp_render_blocks(reader, qw_xml_down(slide), 0u))
        return 0;
    return odp_html_literal(reader, "</div></section>");
}

static void odp_set_error(char *error, size_t error_capacity,
                          const char *message)
{
    if (error && error_capacity) {
        (void)snprintf(error, error_capacity, "%s",
                       message ? message : "OpenDocument presentation parsing failed");
    }
}

static QwOdpStatus odp_build_html(const void *data, size_t size,
                                  QwOdpFormat format,
                                  const QwFormatInputResourceProfile *profile,
                                  OdpReader *reader,
                                  char *error, size_t error_capacity)
{
    QwZipArchive zip;
    void *content_data = NULL;
    size_t content_size = 0u;
    QwXmlNode *xml = NULL;
    QwXmlNode *presentation = NULL;
    QwXmlNode *slide;
    QwXmlError xml_error;
    int archive_open = 0;
    QwOdpStatus rc = QW_ODP_ERR_FORMAT;
    if (!reader) return QW_ODP_ERR_INVALID_ARGUMENT;
    memset(reader, 0, sizeof(*reader));
    memset(&zip, 0, sizeof(zip));
    if (!data || !size || format < QW_ODP_FORMAT_ODP ||
        format > QW_ODP_FORMAT_FODP) return QW_ODP_ERR_INVALID_ARGUMENT;
    if (format == QW_ODP_FORMAT_FODP) {
        if (size > ODP_MAX_FLAT_XML_BYTES) {
            odp_set_error(error, error_capacity,
                          "FODP XML exceeds the parser limit");
            return QW_ODP_ERR_RESOURCE_LIMIT;
        }
        if (!odp_flat_probe(data, size)) {
            odp_set_error(error, error_capacity,
                          "FODP is not a recognized flat presentation XML file");
            return QW_ODP_ERR_FORMAT;
        }
        content_data = malloc(size);
        if (!content_data) {
            odp_set_error(error, error_capacity,
                          "Cannot allocate FODP input buffer");
            return QW_ODP_ERR_OUT_OF_MEMORY;
        }
        memcpy(content_data, data, size);
        content_size = size;
    } else {
        if (profile) {
            archive_open = qw_zip_reader_init_mem_with_profile(
                &zip, data, size, 0u, profile);
        } else {
            archive_open = qw_zip_reader_init_mem(&zip, data, size, 0u);
        }
        if (!archive_open) {
            odp_set_error(error, error_capacity,
                          "OpenDocument presentation package could not be opened as ZIP");
            return QW_ODP_ERR_FORMAT;
        }
        reader->zip = &zip;
        content_data = odp_extract_xml(reader, "content.xml", &content_size);
        if (!content_data) {
            odp_set_error(error, error_capacity,
                          "OpenDocument presentation content.xml is missing, unsupported, or too large");
            rc = QW_ODP_ERR_FORMAT;
            goto cleanup;
        }
    }

    xml = odp_parse_xml((const uint8_t *)content_data, content_size,
                        &xml_error);
    if (!xml) {
        odp_set_error(error, error_capacity, xml_error.message);
        rc = xml_error.code == QW_XML_ERROR_RESOURCE_LIMIT
                 ? QW_ODP_ERR_RESOURCE_LIMIT : QW_ODP_ERR_DECODE;
        goto cleanup;
    }
    presentation = odp_find_descendant(reader, qw_xml_root(xml),
                                       "presentation", 0u);
    if (!presentation) {
        odp_set_error(error, error_capacity,
                      "OpenDocument content has no presentation body");
        rc = QW_ODP_ERR_FORMAT;
        goto cleanup;
    }
    reader->html.limit = (size_t)ODP_MAX_HTML_BYTES + 1u;
    if (!odp_html_literal(reader,
            "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
            "@page{size:landscape;margin:12mm}body{font-family:sans-serif;"
            "font-size:18pt;color:#151515;line-height:1.35}"
            ".slide{break-after:page;page-break-after:always;min-height:90vh;"
            "display:flex;flex-direction:column;justify-content:center;"
            "padding:1rem 2rem}header{border-bottom:1px solid #aaa;"
            "margin-bottom:1.2rem}h2{font-size:12pt;font-weight:400;color:#555}"
            "h3{font-size:30pt}p{margin:.35em 0}li{margin:.3em 0}"
            ".omitted{font-size:10pt;color:#666}ul{margin-top:.25em}"
            "</style></head><body>")) {
        odp_set_error(error, error_capacity,
                      "Presentation HTML header exceeds output limits");
        rc = QW_ODP_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    for (slide = qw_xml_down(presentation); slide; slide = qw_xml_next(slide)) {
        if (odp_is_tag(slide, "page") && !odp_render_slide(reader, slide)) {
            odp_set_error(error, error_capacity,
                          "OpenDocument presentation exceeds conversion resource limits");
            rc = QW_ODP_ERR_RESOURCE_LIMIT;
            goto cleanup;
        }
    }
    if (!reader->slide_count) {
        odp_set_error(error, error_capacity,
                      "OpenDocument presentation has no slides");
        rc = QW_ODP_ERR_FORMAT;
        goto cleanup;
    }
    if (!odp_html_literal(reader, "</body></html>")) {
        odp_set_error(error, error_capacity,
                      "Presentation HTML output exceeds the size limit");
        rc = QW_ODP_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    rc = QW_ODP_OK;

cleanup:
    qw_xml_drop(xml);
    free(content_data);
    if (archive_open) qw_zip_reader_end(&zip);
    return rc;
}

#if !defined(QUIREWISE_STANDALONE_ODP)
static int odp_open_layout(const void *data, size_t size,
                           QwInputFormat input_format,
                           const QwConvertOptions *options,
                           QwLayoutDocument *out_document,
                           QwConvertReport *report)
{
    OdpReader reader;
    QwOdpStatus rc;
    QwOdpFormat format;
    char error[160] = {0};
    (void)options;
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    memset(&reader, 0, sizeof(reader));
    if (input_format == QW_FORMAT_ODP) format = QW_ODP_FORMAT_ODP;
    else if (input_format == QW_FORMAT_OTP) format = QW_ODP_FORMAT_OTP;
    else if (input_format == QW_FORMAT_FODP) format = QW_ODP_FORMAT_FODP;
    else return QW_STATUS_ERR_INVALID_ARGUMENT;
    rc = odp_build_html(data, size, format,
        format == QW_ODP_FORMAT_FODP ? NULL :
            qw_format_resource_profile_get(input_format),
        &reader, error, sizeof(error));
    if (rc != QW_ODP_OK) {
        free(reader.html.data);
        if (report) (void)snprintf(report->note, sizeof(report->note),
                                   "OpenDocument presentation parse: %.120s",
                                   error);
        switch (rc) {
        case QW_ODP_ERR_INVALID_ARGUMENT: return QW_STATUS_ERR_INVALID_ARGUMENT;
        case QW_ODP_ERR_OUT_OF_MEMORY: return QW_STATUS_ERR_OUT_OF_MEMORY;
        case QW_ODP_ERR_UNSUPPORTED: return QW_STATUS_ERR_UNSUPPORTED;
        case QW_ODP_ERR_RESOURCE_LIMIT: return QW_STATUS_ERR_RESOURCE_LIMIT;
        case QW_ODP_ERR_DECODE: return QW_STATUS_ERR_DECODE;
        default: return QW_STATUS_ERR_FORMAT;
        }
    }
    out_document->html_utf8 = (const uint8_t *)reader.html.data;
    out_document->html_size = reader.html.size;
    out_document->context = reader.html.data;
    out_document->dispose = free;
    if (report) {
        if (input_format == QW_FORMAT_OTP) {
            (void)snprintf(report->note, sizeof(report->note),
                "OTP template slides opened as landscape layout HTML; source page size, master pages, shapes, images, animation and template behavior are not preserved");
        } else if (input_format == QW_FORMAT_FODP) {
            (void)snprintf(report->note, sizeof(report->note),
                "FODP flat presentation pages and text opened as landscape layout HTML; source page size, master pages, shapes, images, animation and styles are not preserved");
        } else {
            (void)snprintf(report->note, sizeof(report->note),
                "ODP slide text and lists opened as landscape layout HTML; source page size, master pages, shapes, images, animation and styles are not preserved");
        }
    }
    return QW_STATUS_OK;
}
#endif

QwOdpStatus qw_odp_parse(QwOdpFormat format, const void *data, size_t size,
                         QwOdpDocument **out_document)
{
    OdpReader reader;
    QwOdpDocument *document;
    QwOdpStatus status;
    if (!out_document) return QW_ODP_ERR_INVALID_ARGUMENT;
    *out_document = NULL;
    memset(&reader, 0, sizeof(reader));
    status = odp_build_html(data, size, format, NULL, &reader, NULL, 0u);
    if (status != QW_ODP_OK) {
        free(reader.html.data);
        return status;
    }
    document = (QwOdpDocument *)calloc(1u, sizeof(*document));
    if (!document) {
        free(reader.html.data);
        return QW_ODP_ERR_OUT_OF_MEMORY;
    }
    document->html = reader.html;
    document->slide_count = reader.slide_count;
    *out_document = document;
    return QW_ODP_OK;
}

const uint8_t *qw_odp_document_html(const QwOdpDocument *document,
                                   size_t *out_size)
{
    if (out_size) *out_size = document ? document->html.size : 0u;
    return document ? (const uint8_t *)document->html.data : NULL;
}

size_t qw_odp_document_slide_count(const QwOdpDocument *document)
{
    return document ? document->slide_count : 0u;
}

void qw_odp_document_free(QwOdpDocument *document)
{
    if (!document) return;
    free(document->html.data);
    free(document);
}

#if !defined(QUIREWISE_STANDALONE_ODP)
const QwFormatDescriptor qw_format_odp = {
    .id = QW_FORMAT_ODP,
    .name = "ODP",
    .capabilities = QW_FORMAT_CAP_CONTAINER | QW_FORMAT_CAP_MULTI_PAGE |
                    QW_FORMAT_CAP_ARCHIVE_PROBE,
    .probe = NULL,
    .open_memory = NULL,
    .open_layout = odp_open_layout,
    .open_audio = NULL,
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_DEFAULT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE,
    .convert_to_pdf = NULL
};

const QwFormatDescriptor qw_format_otp = {
    .id = QW_FORMAT_OTP,
    .name = "OTP",
    .capabilities = QW_FORMAT_CAP_CONTAINER | QW_FORMAT_CAP_MULTI_PAGE |
                    QW_FORMAT_CAP_ARCHIVE_PROBE,
    .probe = NULL,
    .open_memory = NULL,
    .open_layout = odp_open_layout,
    .open_audio = NULL,
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_DEFAULT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE,
    .convert_to_pdf = NULL
};

const QwFormatDescriptor qw_format_fodp = {
    .id = QW_FORMAT_FODP,
    .name = "FODP",
    .capabilities = QW_FORMAT_CAP_MULTI_PAGE,
    .probe = odp_flat_probe,
    .open_memory = NULL,
    .open_layout = odp_open_layout,
    .open_audio = NULL,
    .max_input_bytes = ODP_MAX_FLAT_XML_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .convert_to_pdf = NULL
};
#endif
