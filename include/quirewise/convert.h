#ifndef QUIREWISE_CONVERT_H
#define QUIREWISE_CONVERT_H

#include "quirewise/format.h"
#include "quirewise/status.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A top-down 8-bit raster page: channels is 1 (gray) or 3 (RGB). */
typedef struct QwRasterPage {
    const uint8_t *pixels;
    size_t stride;
    uint32_t width;
    uint32_t height;
    int channels;
} QwRasterPage;

/* Build a PDF with one uncompressed image page per raster entry. */
QW_API int qw_raster_pages_to_pdf(const QwRasterPage *pages,
                                  size_t count,
                                  uint8_t **out_pdf,
                                  size_t *out_size,
                                  QwConvertReport *report);

/* Detect a known input format from its contents, including container structure. */
QW_API QwInputFormat qw_detect_format(const void *data, size_t size);
QW_API const char *qw_format_name(QwInputFormat format);

/*
 * Convert a memory buffer to a registered output format. On success,
 * *out_data is owned by the caller and must be released with qw_buffer_free.
 */
QW_API int qw_convert_to_format_memory(const void *data,
                                        size_t size,
                                        QwOutputFormat output_format,
                                        const QwConvertOptions *options,
                                        uint8_t **out_data,
                                        size_t *out_size,
                                        QwConvertReport *report);
QW_API void qw_buffer_free(void *buffer);

/*
 * Convert a memory buffer to PDF. On success, *out_pdf is malloc-owned by the
 * caller and must be released with qw_buffer_free().
 */
QW_API int qw_convert_to_pdf_memory(const void *data,
                                     size_t size,
                                     const QwConvertOptions *options,
                                     uint8_t **out_pdf,
                                     size_t *out_size,
                                     QwConvertReport *report);

/*
 * Convert one file to a new PDF file. Existing output paths are never
 * overwritten. A failed write may leave a partial output for manual review.
 */
QW_API int qw_convert_file(const char *input_path,
                            const char *output_path,
                            const QwConvertOptions *options,
                            QwConvertReport *report);

/* Generic target-format counterpart; the output extension must match target. */
QW_API int qw_convert_file_to_format(const char *input_path,
                                      const char *output_path,
                                      QwOutputFormat output_format,
                                      const QwConvertOptions *options,
                                      QwConvertReport *report);

/*
 * Check whether a file can be read through its registered Quirewise route.
 * PDF inputs are opened and every page is rendered; other non-audio inputs
 * use the PDF conversion route, while audio inputs use FLAC. Any generated
 * bytes are discarded without creating an output file. Memory use can
 * therefore be similar to a normal in-memory conversion. Only input keys,
 * archive passwords, and audio_track are used from options; output processing
 * and page-selection options are ignored so the check covers all pages.
 */
QW_API int qw_check_file(const char *input_path,
                          const QwConvertOptions *options,
                          QwConvertReport *report);

#ifdef __cplusplus
}
#endif

#endif
