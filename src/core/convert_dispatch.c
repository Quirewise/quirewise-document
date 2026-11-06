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
