#ifndef QUIREWISE_DOCX_FORMAT_INTERNAL_H
#define QUIREWISE_DOCX_FORMAT_INTERNAL_H

#include <quirewise/docx.h>

#ifndef QUIREWISE_STANDALONE_DOCX_PARSER
#include "../../core/format_registry.h"
#include "../../core/layout_document.h"

extern const QwFormatDescriptor qw_format_docx;
extern const QwFormatDescriptor qw_format_docm;
extern const QwFormatDescriptor qw_format_dotx;
extern const QwFormatDescriptor qw_format_dotm;

int qw_docx_native_open_layout(const void *data, size_t size,
                              QwInputFormat input_format,
                              const QwConvertOptions *options,
                              QwLayoutDocument *out_document,
                              QwConvertReport *report);
#else
int qw_docx_parse_native_html(const void *data, size_t size,
                              char **out_html, size_t *out_html_size,
                              QwDocxPageLayout *out_page_layout);
#endif

#endif
