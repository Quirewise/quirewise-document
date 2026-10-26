#ifndef QUIREWISE_DOCX_H
#define QUIREWISE_DOCX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum QwDocxStatus {
    QW_DOCX_OK = 0,
    QW_DOCX_ERROR_INVALID_ARGUMENT,
    QW_DOCX_ERROR_OUT_OF_MEMORY,
    QW_DOCX_ERROR_FORMAT,
    QW_DOCX_ERROR_UNSUPPORTED,
    QW_DOCX_ERROR_DECODE,
    QW_DOCX_ERROR_RESOURCE_LIMIT,
    QW_DOCX_ERROR_UNKNOWN
} QwDocxStatus;

typedef struct QwDocxDocument QwDocxDocument;

typedef struct QwDocxPageLayout {
    /* WordprocessingML twips (one twentieth of a point). */
    uint32_t width_twips;
    uint32_t height_twips;
    uint32_t top_twips;
    uint32_t right_twips;
    uint32_t bottom_twips;
    uint32_t left_twips;
    int mixed_section_layout;
    size_t omitted_images;
    size_t omitted_list_items;
} QwDocxPageLayout;

/* Returns nonzero when the ZIP package contains a supported Word document part. */
int qw_docx_probe(const void *data, size_t size);

/* Parses DOCX-family WordprocessingML input and owns the returned document. */
QwDocxStatus qw_docx_parse(const void *data, size_t size,
                           QwDocxDocument **out_document);

/* Returned HTML bytes remain valid until the document is freed. */
const uint8_t *qw_docx_document_html(const QwDocxDocument *document,
                                    size_t *out_size);

QwDocxStatus qw_docx_document_get_page_layout(
    const QwDocxDocument *document, QwDocxPageLayout *out_layout);

void qw_docx_document_free(QwDocxDocument *document);

#ifdef __cplusplus
}
#endif

#endif
