/* Public conversion dispatch for the document package.
 *
 * `qw_convert_to_pdf_memory` was declared in `quirewise/convert.h` but never
 * implemented. This file provides a real dispatcher for the formats this
 * package can render internally:
 *
 *  - `%PDF-` input is passed through unchanged (valid PDF stays valid);
 *  - HTML input is rendered by the internal layout/PDF writer;
 *  - flat ODF (`<?xml ... office:document`) goes through the ODT converter;
 *  - ZIP containers are offered to the DOCX and ODT converters in turn, which
 *    validate the package structure themselves.
 *
 * Anything else returns `QW_STATUS_ERR_UNSUPPORTED` with an explanatory note.
 * No MuPDF code is involved. */

#include <quirewise/convert.h>

#include "format_registry.h"
#include "../backends/mupdf_backend.h"
#include "../formats/odt/odt_format.h"
#include "../formats/docx/docx_format.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const QwFormatDescriptor qw_format_odt;
#if !defined(QUIREWISE_STANDALONE_DOCX_PARSER)
extern const QwFormatDescriptor qw_format_docx;
#endif

static int convert_region_equals(const uint8_t *data, size_t size, size_t offset,
                                 const char *text)
{
    size_t length = strlen(text);
    if (!data || offset + length > size) return 0;
    return memcmp(data + offset, text, length) == 0;
}

static int contains_text(const uint8_t *data, size_t size, const char *text)
{
    size_t length = strlen(text);
    size_t index;
    if (!length || size < length) return 0;
    for (index = 0u; index + length <= size; ++index) {
        if (memcmp(data + index, text, length) == 0) return 1;
    }
    return 0;
}

static int pdf_passthrough(const uint8_t *data, size_t size, uint8_t **out_pdf,
                           size_t *out_size, QwConvertReport *report)
{
    uint8_t *copy = (uint8_t *)malloc(size ? size : 1u);
    if (!copy) return QW_STATUS_ERR_OUT_OF_MEMORY;
    if (size) memcpy(copy, data, size);
    *out_pdf = copy;
    *out_size = size;
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "Input already is a PDF document; passed through unchanged");
    }
    return QW_STATUS_OK;
}

int qw_convert_to_pdf_memory(const void *data, size_t size,
                             const QwConvertOptions *options,
                             uint8_t **out_pdf, size_t *out_size,
                             QwConvertReport *report)
{
    const uint8_t *bytes = (const uint8_t *)data;
    int rc;

    if (!out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    if (!bytes || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;

    if (convert_region_equals(bytes, size, 0u, "%PDF-")) {
        return pdf_passthrough(bytes, size, out_pdf, out_size, report);
    }
    if (convert_region_equals(bytes, size, 0u, "PK\x03\x04")) {
#if !defined(QUIREWISE_STANDALONE_DOCX_PARSER)
        if (qw_format_docx.convert_to_pdf) {
            rc = qw_format_docx.convert_to_pdf(bytes, size, options, out_pdf,
                                               out_size, report);
            if (rc == QW_STATUS_OK) return rc;
        }
#endif
        if (qw_format_odt.convert_to_pdf) {
            rc = qw_format_odt.convert_to_pdf(bytes, size, options, out_pdf,
                                              out_size, report);
            if (rc == QW_STATUS_OK) return rc;
        }
        if (report) {
            snprintf(report->note, sizeof(report->note),
                     "ZIP container is not a supported DOCX/ODT document for PDF conversion");
        }
        return QW_STATUS_ERR_FORMAT;
    }
    if (convert_region_equals(bytes, size, 0u, "<?xml") ||
        contains_text(bytes, size, "office:document")) {
        /* The ODT converter recognises flat OpenDocument XML itself. */
        if (qw_format_odt.convert_to_pdf) {
            rc = qw_format_odt.convert_to_pdf(bytes, size, options, out_pdf,
                                              out_size, report);
            if (rc == QW_STATUS_OK) return rc;
        }
        if (report) {
            snprintf(report->note, sizeof(report->note),
                     "Flat XML is not a supported OpenDocument text file");
        }
        return QW_STATUS_ERR_FORMAT;
    }
    if (bytes[0] == '<' &&
        (contains_text(bytes, size, "<html") || contains_text(bytes, size, "<p") ||
         contains_text(bytes, size, "<h1"))) {
        rc = qw_mupdf_convert_html((const char *)bytes, size, out_pdf, out_size,
                                   report);
        return rc;
    }
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "No internal PDF renderer for this input format");
    }
    return QW_STATUS_ERR_UNSUPPORTED;
}

/* Map the historical MuPDF "convert memory" entry onto the internal writers. */
int qw_mupdf_convert_memory(const uint8_t *data, size_t size,
                            const char *input_extension, int reflow_epub,
                            uint8_t **out_pdf, size_t *out_size,
                            QwConvertReport *report)
{
    uint8_t *html = NULL;
    size_t html_size = 0u;
    char *docx_html = NULL;
    size_t docx_html_size = 0u;
    QwDocxPageLayout layout;
    int rc = QW_STATUS_OK;
    int is_docx = 0;
    int is_odt = 0;
    int is_html = 0;
    int is_pdf = 0;
    const char *extension = input_extension ? input_extension : "";

    if (!out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    if (!data || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (reflow_epub) {
        if (report) {
            snprintf(report->note, sizeof(report->note),
                     "EPUB reflow is not implemented by the internal renderer");
        }
        return QW_STATUS_ERR_UNSUPPORTED;
    }

    if (convert_region_equals(data, size, 0u, "%PDF-")) is_pdf = 1;
    else if (convert_region_equals(data, size, 0u, "PK\x03\x04")) {
        if (strstr(extension, "docx") || strstr(extension, "docm") ||
            strstr(extension, "dotx")) {
            is_docx = 1;
        } else if (strstr(extension, "odt") || strstr(extension, "ott")) {
            is_odt = 1;
        } else {
            is_docx = qw_docx_probe(data, size) != 0;
            is_odt = !is_docx;
        }
    } else if (convert_region_equals(data, size, 0u, "<?xml") ||
               contains_text((const char *)data, size, "office:document")) {
        is_odt = 1;
    } else if (data[0] == '<' &&
               (contains_text((const char *)data, size, "<html") ||
                contains_text((const char *)data, size, "<p") ||
                contains_text((const char *)data, size, "<h1"))) {
        is_html = 1;
    } else {
        if (report) snprintf(report->note, sizeof(report->note),
                             "No internal renderer for this input");
        return QW_STATUS_ERR_UNSUPPORTED;
    }

    if (is_pdf) {
        uint8_t *copy = (uint8_t *)malloc(size);
        if (!copy) return QW_STATUS_ERR_OUT_OF_MEMORY;
        memcpy(copy, data, size);
        *out_pdf = copy;
        *out_size = size;
        if (report) snprintf(report->note, sizeof(report->note),
                             "Input already is a PDF document; passed through");
        return QW_STATUS_OK;
    }
    memset(&layout, 0, sizeof(layout));
    if (is_docx) {
        rc = qw_docx_parse_native_html(data, size, &docx_html, &docx_html_size,
                                       &layout);
        if (rc != QW_STATUS_OK) {
            if (report) snprintf(report->note, sizeof(report->note),
                                 "DOCX layout extraction failed (%d)", rc);
            return rc;
        }
        html = (uint8_t *)docx_html;
        html_size = docx_html_size;
    } else if (is_odt) {
        QwInputFormat format = QW_FORMAT_ODT;
        if (convert_region_equals(data, size, 0u, "<?xml")) format = QW_FORMAT_FODT;
        rc = qw_odt_parse_native_html(data, size, format, &html, &html_size, report);
        if (rc != QW_STATUS_OK) return rc;
    } else if (is_html) {
        html = (uint8_t *)malloc(size);
        if (!html) return QW_STATUS_ERR_OUT_OF_MEMORY;
        memcpy(html, data, size);
        html_size = size;
    }

    if (!html) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "No layout HTML produced for this input");
        return QW_STATUS_ERR_FORMAT;
    }

    if (layout.width_twips > 0u && layout.height_twips > 0u) {
        float width = (float)layout.width_twips / 20.0f;
        float height = (float)layout.height_twips / 20.0f;
        rc = qw_mupdf_convert_html_with_page_size((const char *)html, html_size,
                                                  width, height, out_pdf, out_size,
                                                  report);
    } else {
        rc = qw_mupdf_convert_html((const char *)html, html_size, out_pdf,
                                   out_size, report);
    }
    free(html);
    return rc;
}
