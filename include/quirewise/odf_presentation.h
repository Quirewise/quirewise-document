#ifndef QUIREWISE_ODF_PRESENTATION_H
#define QUIREWISE_ODF_PRESENTATION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum QwOdpStatus {
    QW_ODP_OK = 0,
    QW_ODP_ERR_INVALID_ARGUMENT = -1,
    QW_ODP_ERR_OUT_OF_MEMORY = -2,
    QW_ODP_ERR_FORMAT = -3,
    QW_ODP_ERR_UNSUPPORTED = -4,
    QW_ODP_ERR_RESOURCE_LIMIT = -5,
    QW_ODP_ERR_DECODE = -6
} QwOdpStatus;

typedef enum QwOdpFormat {
    QW_ODP_FORMAT_ODP = 0,
    QW_ODP_FORMAT_OTP,
    QW_ODP_FORMAT_FODP
} QwOdpFormat;

typedef struct QwOdpDocument QwOdpDocument;

QwOdpStatus qw_odp_parse(QwOdpFormat format, const void *data, size_t size,
                         QwOdpDocument **out_document);
/* HTML bytes are borrowed and remain valid until the document is freed. */
const uint8_t *qw_odp_document_html(const QwOdpDocument *document,
                                   size_t *out_size);
size_t qw_odp_document_slide_count(const QwOdpDocument *document);
void qw_odp_document_free(QwOdpDocument *document);

#ifdef __cplusplus
}
#endif

#endif
