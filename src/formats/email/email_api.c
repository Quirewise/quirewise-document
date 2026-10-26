#include "quirewise/email.h"

#include "eml_format.h"
#include "format_registry.h"
#include "layout_document.h"

#include <stdlib.h>

struct QwEmailDocument {
    QwLayoutDocument layout;
};

static QwEmailStatus qw_email_status_from_parser(int status)
{
    switch (status) {
    case QW_STATUS_OK: return QW_EMAIL_OK;
    case QW_STATUS_ERR_INVALID_ARGUMENT: return QW_EMAIL_ERROR_INVALID_ARGUMENT;
    case QW_STATUS_ERR_OUT_OF_MEMORY: return QW_EMAIL_ERROR_OUT_OF_MEMORY;
    case QW_STATUS_ERR_FORMAT: return QW_EMAIL_ERROR_FORMAT;
    case QW_STATUS_ERR_UNSUPPORTED: return QW_EMAIL_ERROR_UNSUPPORTED;
    case QW_STATUS_ERR_DECODE: return QW_EMAIL_ERROR_DECODE;
    case QW_STATUS_ERR_RESOURCE_LIMIT: return QW_EMAIL_ERROR_RESOURCE_LIMIT;
    default: return QW_EMAIL_ERROR_UNKNOWN;
    }
}

int qw_email_probe(const void *data, size_t size)
{
    return qw_eml_probe(data, size);
}

QwEmailStatus qw_email_parse(const void *data, size_t size,
                             QwEmailDocument **out_document)
{
    QwEmailDocument *document;
    int status;

    if (!out_document) return QW_EMAIL_ERROR_INVALID_ARGUMENT;
    *out_document = NULL;
    document = (QwEmailDocument *)calloc(1u, sizeof(*document));
    if (!document) return QW_EMAIL_ERROR_OUT_OF_MEMORY;

    status = qw_eml_native_open_layout(data, size, QW_FORMAT_EML, NULL,
                                       &document->layout, NULL);
    if (status != QW_STATUS_OK) {
        if (document->layout.dispose)
            document->layout.dispose(document->layout.context);
        free(document);
        return qw_email_status_from_parser(status);
    }
    *out_document = document;
    return QW_EMAIL_OK;
}

const uint8_t *qw_email_document_html(const QwEmailDocument *document,
                                     size_t *out_size)
{
    if (out_size) *out_size = 0u;
    if (!document || !out_size) return NULL;
    *out_size = document->layout.html_size;
    return document->layout.html_utf8;
}

size_t qw_email_document_attachment_count(const QwEmailDocument *document)
{
    return document && document->layout.attachments
        ? document->layout.attachments->count : 0u;
}

QwEmailStatus qw_email_document_get_attachment(
    const QwEmailDocument *document, size_t index,
    QwEmailAttachment *out_attachment)
{
    const QwAttachmentSet *attachments;
    const QwAttachment *source;

    if (!document || !out_attachment)
        return QW_EMAIL_ERROR_INVALID_ARGUMENT;
    *out_attachment = (QwEmailAttachment){0};
    attachments = document->layout.attachments;
    if (!attachments || index >= attachments->count)
        return QW_EMAIL_ERROR_INVALID_ARGUMENT;

    source = &attachments->items[index];
    out_attachment->name = source->name;
    out_attachment->media_type = source->media_type;
    out_attachment->data = source->data;
    out_attachment->size = source->size;
    return QW_EMAIL_OK;
}

void qw_email_document_free(QwEmailDocument *document)
{
    if (!document) return;
    if (document->layout.dispose)
        document->layout.dispose(document->layout.context);
    free(document);
}
