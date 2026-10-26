#ifndef QUIREWISE_HWPX_FORMAT_H
#define QUIREWISE_HWPX_FORMAT_H

#include "../../core/format_registry.h"

int qw_hwpx_native_open_layout(const void *data,
                               size_t size,
                               QwInputFormat input_format,
                               const QwConvertOptions *options,
                               QwLayoutDocument *out_document,
                               QwConvertReport *report);

#endif
