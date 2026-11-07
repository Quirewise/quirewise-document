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
#include "../backends/document_backend.h"
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
        rc = qw_doc_convert_html((const char *)bytes, size, out_pdf, out_size,
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

#include <quirewise/epub.h>
#include <quirewise/fb2.h>
#include <quirewise/spreadsheet.h>

#define QW_EPUB_HTML_LIMIT (8u * 1024u * 1024u)

static int epub_html_append(char **buffer, size_t *size, size_t *capacity,
                            const char *text, size_t length)
{
    char *grown;
    size_t needed = *size + length;
    if (needed < *size || needed > QW_EPUB_HTML_LIMIT) return 0;
    if (needed + 1u > *capacity) {
        size_t next = *capacity ? *capacity : 1024u;
        while (next < needed + 1u) {
            if (next > QW_EPUB_HTML_LIMIT / 2u) { next = QW_EPUB_HTML_LIMIT; break; }
            next *= 2u;
        }
        if (next < needed + 1u) return 0;
        grown = (char *)realloc(*buffer, next);
        if (!grown) return 0;
        *buffer = grown;
        *capacity = next;
    }
    if (length) memcpy(*buffer + *size, text, length);
    *size = needed;
    (*buffer)[*size] = '\0';
    return 1;
}

static int epub_html_escape(char **buffer, size_t *size, size_t *capacity,
                            const char *text, size_t length)
{
    size_t i;
    for (i = 0u; i < length; ++i) {
        const char *replacement = NULL;
        switch (text[i]) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        default: break;
        }
        if (replacement) {
            if (!epub_html_append(buffer, size, capacity, replacement,
                                  strlen(replacement))) return 0;
        } else if (!epub_html_append(buffer, size, capacity, text + i, 1u)) {
            return 0;
        }
    }
    return 1;
}

static int epub_to_html_internal(const uint8_t *data, size_t size,
                                 char **out_html, size_t *out_size,
                                 QwConvertReport *report)
{
    QwEpubDocument *document = NULL;
    QwEpubStatus status;
    char *html = NULL;
    size_t length = 0u;
    size_t capacity = 0u;
    size_t section_index;
    size_t section_count;
    int ok = 0;
    status = qw_epub_parse(data, size, &document);
    if (status != QW_EPUB_OK || !document) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "EPUB parse failed with status %d", (int)status);
        return status == QW_EPUB_ERR_RESOURCE_LIMIT
                   ? QW_STATUS_ERR_RESOURCE_LIMIT
                   : QW_STATUS_ERR_FORMAT;
    }
    section_count = qw_epub_section_count(document);
    ok = epub_html_append(&html, &length, &capacity,
                          "<html><body>", strlen("<html><body>"));
    for (section_index = 0u; ok && section_index < section_count;
         ++section_index) {
        QwEpubSectionView section;
        size_t paragraph_index;
        memset(&section, 0, sizeof(section));
        if (qw_epub_section_at(document, section_index, &section) !=
            QW_EPUB_OK) {
            ok = 0;
            break;
        }
        if (section.title && section.title_size) {
            ok = epub_html_append(&html, &length, &capacity, "<h1>", 4u) &&
                 epub_html_escape(&html, &length, &capacity, section.title,
                                  section.title_size) &&
                 epub_html_append(&html, &length, &capacity, "</h1>", 5u);
        }
        for (paragraph_index = 0u;
             ok && paragraph_index < section.paragraph_count;
             ++paragraph_index) {
            QwEpubParagraphView paragraph;
            size_t run_index;
            char tag[8];
            memset(&paragraph, 0, sizeof(paragraph));
            if (qw_epub_paragraph_at(document, section_index, paragraph_index,
                                     &paragraph) != QW_EPUB_OK) {
                ok = 0;
                break;
            }
            if (paragraph.page_break_before) {
                ok = epub_html_append(&html, &length, &capacity,
                                      "<div style=\"page-break-before:always\"></div>",
                                      strlen("<div style=\"page-break-before:always\"></div>"));
            }
            if (!ok) break;
            if (paragraph.heading_level) {
                snprintf(tag, sizeof(tag), "<h%u>",
                         paragraph.heading_level > 6u ? 6u
                                                       : paragraph.heading_level);
                ok = epub_html_append(&html, &length, &capacity, tag,
                                      strlen(tag));
            } else {
                ok = epub_html_append(&html, &length, &capacity, "<p>", 3u);
            }
            for (run_index = 0u; ok && run_index < paragraph.run_count;
                 ++run_index) {
                QwEpubRunView run;
                memset(&run, 0, sizeof(run));
                if (qw_epub_run_at(document, section_index, paragraph_index,
                                   run_index, &run) != QW_EPUB_OK) {
                    ok = 0;
                    break;
                }
                if (run.link_target && run.link_target_size)
                    ok = epub_html_append(&html, &length, &capacity,
                                          "<a href=\"", strlen("<a href=\"")) &&
                         epub_html_escape(&html, &length, &capacity,
                                          run.link_target,
                                          run.link_target_size) &&
                         epub_html_append(&html, &length, &capacity, "\">", 2u);
                if (ok && (run.style_flags & QW_EPUB_STYLE_BOLD))
                    ok = epub_html_append(&html, &length, &capacity, "<b>", 3u);
                if (ok && (run.style_flags & QW_EPUB_STYLE_ITALIC))
                    ok = epub_html_append(&html, &length, &capacity, "<i>", 3u);
                if (ok && (run.style_flags & QW_EPUB_STYLE_CODE))
                    ok = epub_html_append(&html, &length, &capacity, "<code>", 6u);
                if (ok && run.utf8_text && run.text_size)
                    ok = epub_html_escape(&html, &length, &capacity,
                                          (const char *)run.utf8_text,
                                          run.text_size);
                if (ok && (run.style_flags & QW_EPUB_STYLE_CODE))
                    ok = epub_html_append(&html, &length, &capacity, "</code>", 7u);
                if (ok && (run.style_flags & QW_EPUB_STYLE_ITALIC))
                    ok = epub_html_append(&html, &length, &capacity, "</i>", 4u);
                if (ok && (run.style_flags & QW_EPUB_STYLE_BOLD))
                    ok = epub_html_append(&html, &length, &capacity, "</b>", 4u);
                if (ok && run.link_target && run.link_target_size)
                    ok = epub_html_append(&html, &length, &capacity, "</a>", 4u);
            }
            if (ok && paragraph.heading_level) {
                snprintf(tag, sizeof(tag), "</h%u>",
                         paragraph.heading_level > 6u ? 6u
                                                       : paragraph.heading_level);
                ok = epub_html_append(&html, &length, &capacity, tag,
                                      strlen(tag));
            } else if (ok) {
                ok = epub_html_append(&html, &length, &capacity, "</p>", 4u);
            }
        }
    }
    if (ok)
        ok = epub_html_append(&html, &length, &capacity, "</body></html>",
                              strlen("</body></html>"));
    qw_epub_document_free(document);
    if (!ok) {
        free(html);
        if (report) snprintf(report->note, sizeof(report->note),
                             "EPUB reflow hit the HTML size limit");
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    *out_html = html;
    *out_size = length;
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "EPUB reflow: text/styles rendered; images and CSS approximated "
                 "(QW_ROUTE_LOSS_TEXT_LAYOUT|QW_ROUTE_LOSS_FORMAT_DEPENDENT)");
    }
    return QW_STATUS_OK;
}


static int csv_html_append(char **buffer, size_t *size, size_t *capacity,
                           const char *text, size_t length)
{
    char *grown;
    size_t needed = *size + length;
    if (needed < *size) return 0;
    if (needed + 1u > *capacity) {
        size_t next = *capacity ? *capacity : 1024u;
        while (next < needed + 1u) next *= 2u;
        grown = (char *)realloc(*buffer, next);
        if (!grown) return 0;
        *buffer = grown;
        *capacity = next;
    }
    if (length) memcpy(*buffer + *size, text, length);
    *size = needed;
    (*buffer)[*size] = '\0';
    return 1;
}

static int csv_html_escape(char **buffer, size_t *size, size_t *capacity,
                           const uint8_t *text, size_t length)
{
    size_t i;
    for (i = 0u; i < length; ++i) {
        const char *replacement = NULL;
        switch (text[i]) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        default: break;
        }
        if (replacement) {
            if (!csv_html_append(buffer, size, capacity, replacement,
                                 strlen(replacement))) return 0;
        } else if (!csv_html_append(buffer, size, capacity, (const char *)text + i,
                                    1u)) {
            return 0;
        }
    }
    return 1;
}

static int fb2_to_pdf_internal(const uint8_t *data, size_t size,
                               uint8_t **out_pdf, size_t *out_size,
                               QwConvertReport *report)
{
    QwFB2Document *document = NULL;
    const uint8_t *html;
    size_t html_size = 0u;
    int status;
    status = qw_fb2_parse(data, size, &document);
    if (status != QW_FB2_OK || !document) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "FB2 parse failed with status %d", status);
        return status == QW_FB2_ERROR_RESOURCE_LIMIT
                   ? QW_STATUS_ERR_RESOURCE_LIMIT
                   : QW_STATUS_ERR_FORMAT;
    }
    html = qw_fb2_document_html(document, &html_size);
    if (!html || !html_size) {
        qw_fb2_document_free(document);
        return QW_STATUS_ERR_FORMAT;
    }
    status = qw_doc_convert_html((const char *)html, html_size, out_pdf,
                                   out_size, report);
    qw_fb2_document_free(document);
    if (status == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "FB2 layout: text rendered; images and CSS approximated "
                 "(QW_ROUTE_LOSS_TEXT_LAYOUT|QW_ROUTE_LOSS_FORMAT_DEPENDENT)");
    }
    return status;
}

static int csv_to_pdf_internal(const uint8_t *data, size_t size,
                               QwSpreadsheetFormat format,
                               uint8_t **out_pdf, size_t *out_size,
                               QwConvertReport *report)
{
    QwSpreadsheetDocument *document = NULL;
    const QwSpreadsheetSheet *sheet;
    size_t cell_count, index;
    size_t current_row = (size_t)-1;
    char *html = NULL;
    size_t html_size = 0u, capacity = 0u;
    int status;
    status = qw_spreadsheet_parse(format, data, size, &document);
    if (status != QW_SPREADSHEET_OK || !document) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "Spreadsheet parse failed with status %d", status);
        return status == QW_SPREADSHEET_ERR_RESOURCE_LIMIT
                   ? QW_STATUS_ERR_RESOURCE_LIMIT
                   : QW_STATUS_ERR_FORMAT;
    }
    if (qw_spreadsheet_sheet_count(document) == 0u) {
        qw_spreadsheet_document_free(document);
        return QW_STATUS_ERR_FORMAT;
    }
    sheet = qw_spreadsheet_sheet_at(document, 0u);
    cell_count = sheet ? qw_spreadsheet_cell_count(sheet) : 0u;
    if (!csv_html_append(&html, &html_size, &capacity, "<html><body><table>",
                         strlen("<html><body><table>"))) {
        qw_spreadsheet_document_free(document);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    for (index = 0u; index < cell_count; ++index) {
        QwSpreadsheetCellView cell;
        memset(&cell, 0, sizeof(cell));
        if (qw_spreadsheet_cell_at(sheet, index, &cell) != QW_SPREADSHEET_OK)
            continue;
        if (cell.row != current_row) {
            if (current_row != (size_t)-1)
                if (!csv_html_append(&html, &html_size, &capacity, "</tr>", 5u))
                    break;
            if (!csv_html_append(&html, &html_size, &capacity, "<tr>", 4u))
                break;
            current_row = cell.row;
        }
        if (!csv_html_append(&html, &html_size, &capacity, "<td>", 4u) ||
            !csv_html_escape(&html, &html_size, &capacity, cell.value,
                             cell.value ? cell.value_size : 0u) ||
            !csv_html_append(&html, &html_size, &capacity, "</td>", 5u))
            break;
    }
    if (cell_count && current_row != (size_t)-1)
        (void)csv_html_append(&html, &html_size, &capacity, "</tr>", 5u);
    (void)csv_html_append(&html, &html_size, &capacity, "</table></body></html>",
                          strlen("</table></body></html>"));
    qw_spreadsheet_document_free(document);
    if (!html) return QW_STATUS_ERR_OUT_OF_MEMORY;
    status = qw_doc_convert_html(html, html_size, out_pdf, out_size, report);
    free(html);
    if (status == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "CSV layout: cells rendered as an HTML table; styles omitted "
                 "(QW_ROUTE_LOSS_TEXT_LAYOUT|QW_ROUTE_LOSS_FORMAT_DEPENDENT)");
    }
    return status;
}

int qw_doc_convert_memory(const uint8_t *data, size_t size,
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
    int is_fb2 = 0;
    int is_csv = 0;
    const char *extension = input_extension ? input_extension : "";

    if (!out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    if (!data || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (reflow_epub && convert_region_equals(data, size, 0u, "PK\x03\x04")) {
        char *epub_html = NULL;
        size_t epub_html_size = 0u;
        int epub_status = epub_to_html_internal(data, size, &epub_html,
                                                &epub_html_size, report);
        if (epub_status != QW_STATUS_OK) return epub_status;
        epub_status = qw_doc_convert_html(epub_html, epub_html_size, out_pdf,
                                            out_size, report);
        free(epub_html);
        if (epub_status == QW_STATUS_OK && report) {
            snprintf(report->note, sizeof(report->note),
                     "EPUB reflow: text/styles rendered; images and CSS approximated "
                     "(QW_ROUTE_LOSS_TEXT_LAYOUT|QW_ROUTE_LOSS_FORMAT_DEPENDENT)");
        }
        return epub_status;
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
    } else if (strstr(extension, "fb2") ||
               contains_text((const char *)data, size, "<FictionBook")) {
        is_fb2 = 1;
    } else if (strstr(extension, "csv") || strstr(extension, "tsv")) {
        is_csv = 1;
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

    if (is_fb2) {
        return fb2_to_pdf_internal(data, size, out_pdf, out_size, report);
    }
    if (is_csv) {
        QwSpreadsheetFormat format =
            strstr(extension, "tsv") ? QW_SPREADSHEET_FORMAT_TSV
                                      : QW_SPREADSHEET_FORMAT_CSV;
        return csv_to_pdf_internal(data, size, format, out_pdf, out_size,
                                   report);
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
        rc = qw_doc_convert_html_with_page_size((const char *)html, html_size,
                                                  width, height, out_pdf, out_size,
                                                  report);
    } else {
        rc = qw_doc_convert_html((const char *)html, html_size, out_pdf,
                                   out_size, report);
    }
    free(html);
    return rc;
}
