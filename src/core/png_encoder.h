#ifndef QUIREWISE_PNG_ENCODER_H
#define QUIREWISE_PNG_ENCODER_H

#include "quirewise/document.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Encode one page raster as a PNG image.
 *
 * GRAY8 and RGB24 inputs are stored with filter type 0 (None) and a
 * zlib-stored IDAT, so the encoder needs no compressor and stays independent
 * of the image package. Other kinds return QW_STATUS_ERR_UNSUPPORTED.
 * The returned buffer is malloc-owned by the caller.
 */
int qw_png_encode_page(const QwPageImage *page, uint8_t **out_data,
                       size_t *out_size);

#endif
