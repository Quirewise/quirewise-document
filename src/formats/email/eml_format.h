#ifndef QUIREWISE_EML_FORMAT_H
#define QUIREWISE_EML_FORMAT_H

#include "format_registry.h"

int qw_eml_probe(const void *data, size_t size);
int qw_eml_native_open_layout(const void *data,
                              size_t size,
                              QwInputFormat input_format,
                              const QwConvertOptions *options,
                              QwLayoutDocument *out_document,
                              QwConvertReport *report);

#endif
