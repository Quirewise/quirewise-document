#ifndef QUIREWISE_ODT_FORMAT_INTERNAL_H
#define QUIREWISE_ODT_FORMAT_INTERNAL_H

#include "../../core/format_registry.h"
#include "../../core/layout_document.h"

extern const QwFormatDescriptor qw_format_odt;
extern const QwFormatDescriptor qw_format_ott;
extern const QwFormatDescriptor qw_format_fodt;

int qw_odt_native_open_layout(const void *data, size_t size,
                             QwInputFormat input_format,
                             const QwConvertOptions *options,
                             QwLayoutDocument *out_document,
                             QwConvertReport *report);

#endif
