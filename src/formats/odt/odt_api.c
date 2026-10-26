#include "quirewise/odt.h"

#include "odt_format.h"
#include "../../core/layout_document.h"

#include <stdlib.h>
#include <string.h>

struct QwOdtDocument {
    QwLayoutDocument layout;
};

static void odt_layout_dispose(QwLayoutDocument *layout)
{
    if (!layout) return;
    if (layout->dispose) layout->dispose(layout->context);
    memset(layout, 0, sizeof(*layout));
}

static QwOdtStatus odt_status_map(int status)
{
    switch (status) {
    case QW_STATUS_OK: return QW_ODT_OK;
    case QW_STATUS_ERR_INVALID_ARGUMENT: return QW_ODT_ERR_INVALID_ARGUMENT;
    case QW_STATUS_ERR_OUT_OF_MEMORY: return QW_ODT_ERR_OUT_OF_MEMORY;
    case QW_STATUS_ERR_UNSUPPORTED: return QW_ODT_ERR_UNSUPPORTED;
    case QW_STATUS_ERR_RESOURCE_LIMIT: return QW_ODT_ERR_RESOURCE_LIMIT;
    case QW_STATUS_ERR_DECODE: return QW_ODT_ERR_DECODE;
    default: return QW_ODT_ERR_FORMAT;
    }
}

QwOdtStatus qw_odt_parse(QwOdtFormat format, const void *data, size_t size,
                         QwOdtDocument **out_document)
{
    QwOdtDocument *document;
    QwConvertReport report;
    QwInputFormat input_format;
    int status;
    if (!out_document) return QW_ODT_ERR_INVALID_ARGUMENT;
    *out_document = NULL;
    if (!data || !size) return QW_ODT_ERR_INVALID_ARGUMENT;
    switch (format) {
    case QW_ODT_FORMAT_ODT: input_format = QW_FORMAT_ODT; break;
    case QW_ODT_FORMAT_OTT: input_format = QW_FORMAT_OTT; break;
    case QW_ODT_FORMAT_FODT: input_format = QW_FORMAT_FODT; break;
    default: return QW_ODT_ERR_UNSUPPORTED;
    }
    document = (QwOdtDocument *)calloc(1u, sizeof(*document));
    if (!document) return QW_ODT_ERR_OUT_OF_MEMORY;
    memset(&report, 0, sizeof(report));
    status = qw_odt_native_open_layout(data, size, input_format, NULL,
                                       &document->layout, &report);
    if (status != QW_STATUS_OK) {
        odt_layout_dispose(&document->layout);
        free(document);
        return odt_status_map(status);
    }
    *out_document = document;
    return QW_ODT_OK;
}

const uint8_t *qw_odt_document_html(const QwOdtDocument *document,
                                   size_t *out_size)
{
    if (out_size) *out_size = 0u;
    if (!document) return NULL;
    if (out_size) *out_size = document->layout.html_size;
    return document->layout.html_utf8;
}

void qw_odt_document_free(QwOdtDocument *document)
{
    if (!document) return;
    odt_layout_dispose(&document->layout);
    free(document);
}
