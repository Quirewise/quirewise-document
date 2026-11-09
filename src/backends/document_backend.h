#ifndef QUIREWISE_DOCUMENT_BACKEND_H
#define QUIREWISE_DOCUMENT_BACKEND_H

#include "quirewise/convert.h"
#include "quirewise/document.h"

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

typedef struct QwPdfReader QwPdfReader;

typedef struct QwPdfOutlineItem {
    const char *title;
    size_t page_index;
    size_t depth;
} QwPdfOutlineItem;

int qw_doc_convert_memory(const uint8_t *data,
                            size_t size,
                            const char *input_extension,
                            int reflow_epub,
                            uint8_t **out_pdf,
                            size_t *out_size,
                            QwConvertReport *report);
int qw_doc_convert_memory_with_outline(const uint8_t *data,
                                         size_t size,
                                         const char *input_extension,
                                         const QwPdfOutlineItem *outline,
                                         size_t outline_count,
                                         uint8_t **out_pdf,
                                         size_t *out_size,
                                         QwConvertReport *report);

int qw_doc_convert_html(const char *html,
                          size_t html_size,
                          uint8_t **out_pdf,
                          size_t *out_size,
                          QwConvertReport *report);
int qw_doc_convert_html_to_stream(const char *html,
                                   size_t html_size,
                                   FILE *stream,
                                   size_t max_output_bytes,
                                   size_t *out_size,
                                   QwConvertReport *report);
int qw_doc_convert_html_with_page_size(const char *html,
                                        size_t html_size,
                                        float page_width_points,
                                        float page_height_points,
                                        uint8_t **out_pdf,
                                        size_t *out_size,
                                        QwConvertReport *report);
int qw_doc_convert_html_with_page_size_to_stream(
    const char *html,
    size_t html_size,
    float page_width_points,
    float page_height_points,
    FILE *stream,
    size_t max_output_bytes,
    size_t *out_size,
    QwConvertReport *report);
int qw_doc_convert_html_file(const char *path,
                               uint8_t **out_pdf,
                               size_t *out_size,
                               QwConvertReport *report);
int qw_doc_convert_html_file_to_stream(const char *path,
                                         FILE *stream,
                                         size_t max_output_bytes,
                                         size_t *out_size,
                                         QwConvertReport *report);
int qw_doc_decode_caj_jp2(const uint8_t *data,
                            size_t size,
                            QwPageImage *out_page,
                            QwConvertReport *report);

int qw_pdf_render_page_memory(const uint8_t *data,
                             size_t size,
                             size_t page_index,
                             unsigned int dpi,
                             QwPageImage *out_page,
                             QwConvertReport *report);

int qw_pdf_reader_open(const uint8_t *data,
                             size_t size,
                             QwPdfReader **out_reader,
                             size_t *out_page_count,
                             QwConvertReport *report);
/* Open without requiring a supported security handler: page dictionaries are
 * unencrypted, so page count and boxes remain available. */
int qw_pdf_reader_open_structure(const uint8_t *data,
                                       size_t size,
                                       QwPdfReader **out_reader,
                                       size_t *out_page_count,
                                       QwConvertReport *report);
/* Media/crop boxes (points) and rotation for one page. */
int qw_pdf_reader_page_geometry(const QwPdfReader *reader,
                                      size_t page_index,
                                      double media[4],
                                      double crop[4],
                                      int *rotation);
int qw_pdf_reader_render_page(QwPdfReader *reader,
                                    size_t page_index,
                                    unsigned int dpi,
                                    QwPageImage *out_page,
                                    QwConvertReport *report);
int qw_pdf_reader_extract_text(QwPdfReader *reader,
                                     uint8_t **out_text,
                                     size_t *out_size,
                                     QwConvertReport *report);
int qw_pdf_reader_extract_text_to_stream(
    QwPdfReader *reader,
    FILE *stream,
    size_t max_output_bytes,
    size_t *out_size,
    QwConvertReport *report);
/* Object number of a page in document order; 0 when the index is invalid. */
size_t qw_pdf_reader_page_object(const QwPdfReader *reader, size_t index);
void qw_pdf_reader_close(QwPdfReader *reader);

int qw_pdf_merge_documents(const uint8_t *const *pdfs,
                                 const size_t *sizes,
                                 size_t count,
                                 uint8_t **out_pdf,
                                 size_t *out_size,
                                 size_t *out_pages,
                                 QwConvertReport *report);


/* Decode an ITU-T T.6 (CCITT Group 4) stream into GRAY8 samples (0 = black,
 * 255 = white) with the polarity PDF's /BlackIs1 flag asks for. Returns 1 on
 * success and hands ownership of *out_gray8 to the caller. */
int pdf_ccitt_decode_image(const uint8_t *data,
                           size_t size,
                           uint32_t width,
                           uint32_t height,
                           int black_is1,
                           uint8_t **out_gray8);

/* Decode a PDF JBIG2 image stream (optionally preceded by the /JBIG2Globals
 * segments) into GRAY8 samples (0 = black, 255 = white). Returns 1 on success,
 * QW_STATUS_ERR_UNSUPPORTED when the stream uses segment types this decoder
 * does not implement, or 0 for malformed input. */
int pdf_jbig2_decode_image(const uint8_t *globals,
                           size_t globals_size,
                           const uint8_t *data,
                           size_t size,
                           uint32_t width_hint,
                           uint32_t height_hint,
                           uint8_t **out_gray8);
#endif
