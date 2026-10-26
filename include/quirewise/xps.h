#ifndef QUIREWISE_XPS_H
#define QUIREWISE_XPS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum QwXpsStatus {
    QW_XPS_OK = 0,
    QW_XPS_ERR_INVALID_ARGUMENT = -1,
    QW_XPS_ERR_OUT_OF_MEMORY = -2,
    QW_XPS_ERR_FORMAT = -3,
    QW_XPS_ERR_UNSUPPORTED = -4,
    QW_XPS_ERR_RESOURCE_LIMIT = -5,
    QW_XPS_ERR_DECODE = -6
} QwXpsStatus;

typedef struct QwXpsDocument QwXpsDocument;

typedef struct QwXpsPageInfo {
    double width;
    double height;
} QwXpsPageInfo;

/* The input bytes are borrowed and must remain valid until document close. */
QwXpsStatus qw_xps_document_open(const void *data, size_t size,
                                 QwXpsDocument **out_document);
size_t qw_xps_document_page_count(const QwXpsDocument *document);
/* Page XML is allocated by the library and released with qw_xps_data_free.
 * Width and height use XPS FixedPage units (1/96 inch). */
QwXpsStatus qw_xps_document_read_page(QwXpsDocument *document,
                                      size_t page_index,
                                      uint8_t **out_xml,
                                      size_t *out_size,
                                      QwXpsPageInfo *out_info);
void qw_xps_data_free(uint8_t *data);
void qw_xps_document_close(QwXpsDocument *document);

#ifdef __cplusplus
}
#endif

#endif
