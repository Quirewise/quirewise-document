#include <quirewise/convert.h>
#include <quirewise/document.h>

#include "../src/backends/mupdf_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_JP2_FIXTURE
#define QUIREWISE_JP2_FIXTURE "tests/data/jp2/gray53_13x17.jp2"
#endif

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *data;
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) { fclose(file); return NULL; }
    data = (uint8_t *)malloc((size_t)length);
    if (!data) { fclose(file); return NULL; }
    if (fread(data, 1u, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)length;
    return data;
}

int main(void)
{
    size_t size = 0u;
    uint8_t *data = read_file(QUIREWISE_JP2_FIXTURE, &size);
    QwPageImage page;
    QwConvertReport report;
    int rc;
    if (!data) {
        fprintf(stderr, "cannot read JP2 fixture: %s\n", QUIREWISE_JP2_FIXTURE);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    rc = qw_mupdf_decode_caj_jp2(data, size, &page, &report);
    free(data);
    if (rc != 0) {
        fprintf(stderr, "CAJ JP2 decode failed: rc=%d note=%s\n", rc,
                report.note);
        return 2;
    }
    if (page.width != 13u || page.height != 17u || !page.data || !page.size ||
        (page.kind != QW_PAGE_IMAGE_GRAY8 && page.kind != QW_PAGE_IMAGE_RGB24)) {
        fprintf(stderr, "unexpected CAJ JP2 result: kind=%d %ux%u size=%zu\n",
                page.kind, page.width, page.height, page.size);
        free((void *)page.data);
        return 3;
    }
    free((void *)page.data);
    puts("CAJ JP2 payload decoded to a page image");
    return 0;
}
