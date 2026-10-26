#include "quirewise/xps.h"

#include "xps_package.h"

#include <stdlib.h>

struct QwXpsDocument {
    QwXpsPackage *package;
};

static QwXpsStatus xps_public_status(int status)
{
    switch (status) {
    case QW_STATUS_OK: return QW_XPS_OK;
    case QW_STATUS_ERR_INVALID_ARGUMENT: return QW_XPS_ERR_INVALID_ARGUMENT;
    case QW_STATUS_ERR_OUT_OF_MEMORY: return QW_XPS_ERR_OUT_OF_MEMORY;
    case QW_STATUS_ERR_UNSUPPORTED: return QW_XPS_ERR_UNSUPPORTED;
    case QW_STATUS_ERR_RESOURCE_LIMIT: return QW_XPS_ERR_RESOURCE_LIMIT;
    case QW_STATUS_ERR_DECODE: return QW_XPS_ERR_DECODE;
    default: return QW_XPS_ERR_FORMAT;
    }
}

QwXpsStatus qw_xps_document_open(const void *data, size_t size,
                                 QwXpsDocument **out_document)
{
    QwXpsDocument *document;
    int status;
    if (!out_document) return QW_XPS_ERR_INVALID_ARGUMENT;
    *out_document = NULL;
    document = (QwXpsDocument *)calloc(1u, sizeof(*document));
    if (!document) return QW_XPS_ERR_OUT_OF_MEMORY;
    status = qw_xps_package_open(&document->package, data, size, NULL);
    if (status != QW_STATUS_OK) {
        free(document);
        return xps_public_status(status);
    }
    *out_document = document;
    return QW_XPS_OK;
}

size_t qw_xps_document_page_count(const QwXpsDocument *document)
{
    return document ? qw_xps_package_page_count(document->package) : 0u;
}

QwXpsStatus qw_xps_document_read_page(QwXpsDocument *document,
                                      size_t page_index,
                                      uint8_t **out_xml,
                                      size_t *out_size,
                                      QwXpsPageInfo *out_info)
{
    double width = 0.0;
    double height = 0.0;
    int status;
    if (!document || !out_xml || !out_size)
        return QW_XPS_ERR_INVALID_ARGUMENT;
    *out_xml = NULL;
    *out_size = 0u;
    status = qw_xps_package_read_page(document->package, page_index,
                                      out_xml, out_size, &width, &height, NULL);
    if (status != QW_STATUS_OK) return xps_public_status(status);
    if (out_info) {
        out_info->width = width;
        out_info->height = height;
    }
    return QW_XPS_OK;
}

void qw_xps_data_free(uint8_t *data)
{
    free(data);
}

void qw_xps_document_close(QwXpsDocument *document)
{
    if (!document) return;
    qw_xps_package_close(document->package);
    free(document);
}
