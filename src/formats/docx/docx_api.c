#include "quirewise/docx.h"

#include "docx_format.h"
#include "../archive/zip_native.h"

#include "quirewise/status.h"

#include <stdlib.h>
#include <string.h>

struct QwDocxDocument {
    uint8_t *html;
    size_t html_size;
    QwDocxPageLayout page_layout;
};

static QwDocxStatus docx_status_from_parser(int status)
{
    switch (status) {
    case QW_STATUS_OK: return QW_DOCX_OK;
    case QW_STATUS_ERR_INVALID_ARGUMENT: return QW_DOCX_ERROR_INVALID_ARGUMENT;
    case QW_STATUS_ERR_OUT_OF_MEMORY: return QW_DOCX_ERROR_OUT_OF_MEMORY;
    case QW_STATUS_ERR_FORMAT: return QW_DOCX_ERROR_FORMAT;
    case QW_STATUS_ERR_UNSUPPORTED: return QW_DOCX_ERROR_UNSUPPORTED;
    case QW_STATUS_ERR_DECODE: return QW_DOCX_ERROR_DECODE;
    case QW_STATUS_ERR_RESOURCE_LIMIT: return QW_DOCX_ERROR_RESOURCE_LIMIT;
    default: return QW_DOCX_ERROR_UNKNOWN;
    }
}

int qw_docx_probe(const void *data, size_t size)
{
    QwZipArchive archive = {0};
    int document_index;
    int supported;
    if (!data || !size) return 0;
    if (!qw_zip_reader_init_mem_with_profile(
            &archive, data, size, 0,
            QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE)) {
        qw_zip_reader_end(&archive);
        return 0;
    }
    document_index = qw_zip_reader_locate_file(
        &archive, "word/document.xml", NULL, QW_ZIP_CASE_SENSITIVE);
    supported = document_index >= 0 &&
        qw_zip_reader_is_file_supported(&archive, (size_t)document_index);
    qw_zip_reader_end(&archive);
    return supported;
}

QwDocxStatus qw_docx_parse(const void *data, size_t size,
                           QwDocxDocument **out_document)
{
    QwDocxDocument *document;
    char *html = NULL;
    size_t html_size = 0u;
    int status;
    if (!out_document) return QW_DOCX_ERROR_INVALID_ARGUMENT;
    *out_document = NULL;
    if (!data || !size) return QW_DOCX_ERROR_INVALID_ARGUMENT;
    document = (QwDocxDocument *)calloc(1u, sizeof(*document));
    if (!document) return QW_DOCX_ERROR_OUT_OF_MEMORY;
    status = qw_docx_parse_native_html(data, size, &html, &html_size,
                                       &document->page_layout);
    if (status != QW_STATUS_OK) {
        free(html);
        free(document);
        return docx_status_from_parser(status);
    }
    document->html = (uint8_t *)html;
    document->html_size = html_size;
    *out_document = document;
    return QW_DOCX_OK;
}

const uint8_t *qw_docx_document_html(const QwDocxDocument *document,
                                    size_t *out_size)
{
    if (out_size) *out_size = 0u;
    if (!document || !out_size) return NULL;
    *out_size = document->html_size;
    return document->html;
}

QwDocxStatus qw_docx_document_get_page_layout(
    const QwDocxDocument *document, QwDocxPageLayout *out_layout)
{
    if (!document || !out_layout) return QW_DOCX_ERROR_INVALID_ARGUMENT;
    *out_layout = document->page_layout;
    return QW_DOCX_OK;
}

void qw_docx_document_free(QwDocxDocument *document)
{
    if (!document) return;
    free(document->html);
    free(document);
}
