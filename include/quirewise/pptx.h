#ifndef QUIREWISE_PPTX_H
#define QUIREWISE_PPTX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum QwPptxStatus {
    QW_PPTX_OK = 0,
    QW_PPTX_ERR_INVALID_ARGUMENT = -1,
    QW_PPTX_ERR_OUT_OF_MEMORY = -2,
    QW_PPTX_ERR_FORMAT = -3,
    QW_PPTX_ERR_UNSUPPORTED = -4,
    QW_PPTX_ERR_RESOURCE_LIMIT = -5,
    QW_PPTX_ERR_DECODE = -6
} QwPptxStatus;

typedef enum QwPptxFormat {
    QW_PPTX_FORMAT_PPTX = 0,
    QW_PPTX_FORMAT_PPTM,
    QW_PPTX_FORMAT_POTX,
    QW_PPTX_FORMAT_POTM,
    QW_PPTX_FORMAT_PPSX,
    QW_PPTX_FORMAT_PPSM
} QwPptxFormat;

typedef struct QwPptxDocument QwPptxDocument;

QwPptxStatus qw_pptx_parse(QwPptxFormat format, const void *data, size_t size,
                           QwPptxDocument **out_document);
/* HTML bytes are borrowed from the document and remain valid until freed. */
const uint8_t *qw_pptx_document_html(const QwPptxDocument *document,
                                    size_t *out_size);
void qw_pptx_document_free(QwPptxDocument *document);

#ifdef __cplusplus
}
#endif

#endif
