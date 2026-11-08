#ifndef QUIREWISE_PDF_H
#define QUIREWISE_PDF_H

#include "quirewise/convert.h"
#include "quirewise/document.h"
#include "quirewise/status.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Public PDF reading, rendering and merging.
 *
 * The implementation is the package-local PDF engine: cross-reference tables
 * and xref streams (classic, /Prev chains, hybrid /XRefStm), object streams,
 * FlateDecode with TIFF/PNG predictors, the Standard security handler
 * (V1/V2 RC4, V4 /AESV2 AES-128-CBC and V5/R5-R6 /AESV3 AES-256-CBC, empty
 * user password), text extraction (per-font /ToUnicode CMaps with UCS-2 and
 * WinAnsi fallbacks), an RGB24 rasteriser for the supported content subset
 * (paths, text, JPEG XObjects and raw Flate RGB/Gray images with PNG or TIFF
 * predictors), and a classic-xref merger.
 *
 * Input bytes are borrowed and must stay valid until qw_pdf_close(). Outputs
 * are owned by the caller: text and merged documents with qw_pdf_free(), page
 * rasters with qw_pdf_page_free().
 */

/* Opaque reader handle. */
typedef struct QwPdfReader QwPdfDocument;

/*!
 * Open a PDF from memory. On success *out_document owns the parse state and
 * *out_page_count receives the number of pages. On failure *out_document is
 * NULL and the return value is a QwStatus error code.
 */
QW_API int qw_pdf_open(const uint8_t *data, size_t size,
                       QwPdfDocument **out_document,
                       size_t *out_page_count,
                       QwConvertReport *report);

/*!
 * Open a PDF for structure queries only. Page dictionaries are not encrypted,
 * so page count and geometry stay available even when the security handler
 * needs a user password this reader does not support. Text extraction and
 * rendering on such a document fail with a stable error. Pages stored inside
 * an encrypted /ObjStm cannot be enumerated without the password.
 */
QW_API int qw_pdf_open_structure(const uint8_t *data, size_t size,
                                 QwPdfDocument **out_document,
                                 size_t *out_page_count,
                                 QwConvertReport *report);

/*!
 * Media box, crop box (both in points, x0/y0/x1/y1) and rotation (0/90/180/
 * 270) for one page. Missing boxes default to US Letter; malformed boxes fall
 * back to the default instead of failing.
 */
QW_API int qw_pdf_page_geometry(QwPdfDocument *document, size_t page_index,
                                double media_box[4], double crop_box[4],
                                int *rotation);

/*!
 * Rasterise one page to a malloc-owned RGB24 raster at the requested
 * resolution (72 selects page units). Unsupported drawing operators are
 * reported through the route-loss note instead of failing the call, so a
 * successful result may be partial.
 */
QW_API int qw_pdf_render_page(QwPdfDocument *document, size_t page_index,
                              unsigned int dpi, QwPageImage *out_page,
                              QwConvertReport *report);

/*!
 * Extract the text of every page as UTF-8. The result is malloc-owned and
 * released with qw_pdf_free(). The returned text is empty for pages whose
 * content only draws non-text operators.
 */
QW_API int qw_pdf_extract_text(QwPdfDocument *document, uint8_t **out_text,
                               size_t *out_size, QwConvertReport *report);

/*!
 * Merge PDF documents into one classic-xref PDF. Inputs must be unencrypted;
 * encrypted inputs are rejected with QW_STATUS_ERR_UNSUPPORTED.
 */
QW_API int qw_pdf_merge(const uint8_t *const *pdfs, const size_t *sizes,
                        size_t count, uint8_t **out_pdf, size_t *out_size,
                        size_t *out_pages, QwConvertReport *report);

/*! Release a document returned by qw_pdf_open(). NULL is accepted. */
QW_API void qw_pdf_close(QwPdfDocument *document);

/*! Release text or a merged PDF returned by this header. */
QW_API void qw_pdf_free(void *buffer);

/*! Release a page raster returned by qw_pdf_render_page(). */
QW_API void qw_pdf_page_free(QwPageImage *page);

#ifdef __cplusplus
}
#endif

#endif
