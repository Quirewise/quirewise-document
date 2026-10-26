#ifndef QUIREWISE_SVG_BACKEND_H
#define QUIREWISE_SVG_BACKEND_H

#include "quirewise/convert.h"
#include "../formats/svg/svg_limits.h"

#include <stddef.h>
#include <stdint.h>

int qw_svg_to_png(const uint8_t *svg,
                  size_t svg_size,
                  uint8_t **out_png,
                  size_t *out_size,
                  QwConvertReport *report);

int qw_svg_to_rgb24(const uint8_t *svg,
                    size_t svg_size,
                    uint8_t **out_pixels,
                    size_t *out_size,
                    uint32_t *out_width,
                    uint32_t *out_height,
                    QwConvertReport *report);

#endif
