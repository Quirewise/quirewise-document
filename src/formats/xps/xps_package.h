#ifndef QUIREWISE_XPS_PACKAGE_H
#define QUIREWISE_XPS_PACKAGE_H

#include "quirewise/convert.h"

#include <stddef.h>
#include <stdint.h>

typedef struct QwXpsPackage QwXpsPackage;

int qw_xps_package_open(QwXpsPackage **out_package,
                        const void *data,
                        size_t size,
                        QwConvertReport *report);
size_t qw_xps_package_page_count(const QwXpsPackage *package);
int qw_xps_package_read_page(QwXpsPackage *package,
                             size_t page_index,
                             uint8_t **out_data,
                             size_t *out_size,
                             double *out_width,
                             double *out_height,
                             QwConvertReport *report);
void qw_xps_package_close(QwXpsPackage *package);

#endif
