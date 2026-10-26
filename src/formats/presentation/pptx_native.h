#ifndef QUIREWISE_PPTX_NATIVE_H
#define QUIREWISE_PPTX_NATIVE_H

#include "../../core/layout_document.h"

int qw_pptx_native_open_layout(const void *data, size_t size,
                               QwInputFormat input_format,
                               const QwConvertOptions *options,
                               QwLayoutDocument *out_document,
                               QwConvertReport *report);

#endif
