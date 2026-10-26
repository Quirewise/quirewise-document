#ifndef QUIREWISE_ODT_H
#define QUIREWISE_ODT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum QwOdtStatus {
    QW_ODT_OK = 0,
    QW_ODT_ERR_INVALID_ARGUMENT = -1,
    QW_ODT_ERR_OUT_OF_MEMORY = -2,
    QW_ODT_ERR_FORMAT = -3,
    QW_ODT_ERR_UNSUPPORTED = -4,
    QW_ODT_ERR_RESOURCE_LIMIT = -5,
    QW_ODT_ERR_DECODE = -6
} QwOdtStatus;

typedef enum QwOdtFormat {
    QW_ODT_FORMAT_ODT = 0,
    QW_ODT_FORMAT_OTT,
    QW_ODT_FORMAT_FODT
} QwOdtFormat;

typedef struct QwOdtDocument QwOdtDocument;

QwOdtStatus qw_odt_parse(QwOdtFormat format, const void *data, size_t size,
                         QwOdtDocument **out_document);
/* HTML bytes are borrowed from the document and remain valid until freed. */
const uint8_t *qw_odt_document_html(const QwOdtDocument *document,
                                   size_t *out_size);
void qw_odt_document_free(QwOdtDocument *document);

#ifdef __cplusplus
}
#endif

#endif
