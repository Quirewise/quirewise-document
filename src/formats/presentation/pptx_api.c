#include "quirewise/pptx.h"

#include "pptx_native.h"
#include "../../core/layout_document.h"

#include <stdlib.h>
#include <string.h>

struct QwPptxDocument {
    QwLayoutDocument layout;
};

static void pptx_layout_dispose(QwLayoutDocument *layout)
{
    if (!layout) return;
    if (layout->dispose) layout->dispose(layout->context);
    memset(layout, 0, sizeof(*layout));
}

static QwPptxStatus pptx_status_map(int status)
{
    switch (status) {
    case QW_STATUS_OK: return QW_PPTX_OK;
    case QW_STATUS_ERR_INVALID_ARGUMENT: return QW_PPTX_ERR_INVALID_ARGUMENT;
    case QW_STATUS_ERR_OUT_OF_MEMORY: return QW_PPTX_ERR_OUT_OF_MEMORY;
    case QW_STATUS_ERR_UNSUPPORTED: return QW_PPTX_ERR_UNSUPPORTED;
    case QW_STATUS_ERR_RESOURCE_LIMIT: return QW_PPTX_ERR_RESOURCE_LIMIT;
    case QW_STATUS_ERR_DECODE: return QW_PPTX_ERR_DECODE;
    default: return QW_PPTX_ERR_FORMAT;
    }
}

QwPptxStatus qw_pptx_parse(QwPptxFormat format, const void *data, size_t size,
                           QwPptxDocument **out_document)
{
    QwPptxDocument *document;
    QwConvertReport report;
    QwInputFormat input_format;
    int status;
    if (!out_document) return QW_PPTX_ERR_INVALID_ARGUMENT;
    *out_document = NULL;
    if (!data || !size) return QW_PPTX_ERR_INVALID_ARGUMENT;
    switch (format) {
    case QW_PPTX_FORMAT_PPTX: input_format = QW_FORMAT_PPTX; break;
    case QW_PPTX_FORMAT_PPTM: input_format = QW_FORMAT_PPTM; break;
    case QW_PPTX_FORMAT_POTX: input_format = QW_FORMAT_POTX; break;
    case QW_PPTX_FORMAT_POTM: input_format = QW_FORMAT_POTM; break;
    case QW_PPTX_FORMAT_PPSX: input_format = QW_FORMAT_PPSX; break;
    case QW_PPTX_FORMAT_PPSM: input_format = QW_FORMAT_PPSM; break;
    default: return QW_PPTX_ERR_UNSUPPORTED;
    }
    document = (QwPptxDocument *)calloc(1u, sizeof(*document));
    if (!document) return QW_PPTX_ERR_OUT_OF_MEMORY;
    memset(&report, 0, sizeof(report));
    status = qw_pptx_native_open_layout(data, size, input_format, NULL,
                                        &document->layout, &report);
    if (status != QW_STATUS_OK) {
        pptx_layout_dispose(&document->layout);
        free(document);
        return pptx_status_map(status);
    }
    *out_document = document;
    return QW_PPTX_OK;
}

const uint8_t *qw_pptx_document_html(const QwPptxDocument *document,
                                    size_t *out_size)
{
    if (out_size) *out_size = 0u;
    if (!document) return NULL;
    if (out_size) *out_size = document->layout.html_size;
    return document->layout.html_utf8;
}

void qw_pptx_document_free(QwPptxDocument *document)
{
    if (!document) return;
    pptx_layout_dispose(&document->layout);
    free(document);
}
