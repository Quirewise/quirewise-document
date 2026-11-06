#include <quirewise/convert.h>
#include <quirewise/document.h>

#include "../src/backends/mupdf_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_JPEG_FIXTURE
#define QUIREWISE_JPEG_FIXTURE "tests/data/tiny.jpg"
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
    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0u;
    char *pdf = NULL;
    size_t pdf_size = 0u;
    size_t capacity;
    int written;
    QwMuPdfReader *reader = NULL;
    size_t pages = 0u;
    QwPageImage page;
    QwConvertReport report;
    int rc;
    size_t non_white = 0u;
    size_t index;
    jpeg = read_file(QUIREWISE_JPEG_FIXTURE, &jpeg_size);
    if (!jpeg) {
        fprintf(stderr, "cannot read JPEG fixture: %s\n", QUIREWISE_JPEG_FIXTURE);
        return 1;
    }
    capacity = jpeg_size + 4096u;
    pdf = (char *)malloc(capacity);
    if (!pdf) { free(jpeg); return 2; }
    written = snprintf(pdf, capacity,
        "%%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 64 64] "
        "/Resources << /XObject << /Im1 5 0 R >> >> /Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 24 >>\nstream\n"
        "q 64 0 0 64 0 0 cm /Im1 Do Q\n"
        "endstream\nendobj\n"
        "5 0 obj\n<< /Type /XObject /Subtype /Image /Width 1 /Height 1 "
        "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode "
        "/Length %zu >>\nstream\n", jpeg_size);
    if (written < 0 || (size_t)written + jpeg_size + 64u > capacity) {
        free(pdf);
        free(jpeg);
        return 3;
    }
    memcpy(pdf + written, jpeg, jpeg_size);
    pdf_size = (size_t)written + jpeg_size;
    written = snprintf(pdf + pdf_size, capacity - pdf_size,
        "\nendstream\nendobj\ntrailer\n<< /Root 1 0 R /Size 6 >>\n%%%%EOF\n");
    pdf_size += (size_t)written;
    free(jpeg);
    memset(&report, 0, sizeof(report));
    rc = qw_mupdf_pdf_reader_open((const uint8_t *)pdf, pdf_size, &reader,
                                  &pages, &report);
    if (rc != 0 || !reader || pages != 1u) {
        fprintf(stderr, "image PDF open failed: rc=%d pages=%zu note=%s\n", rc,
                pages, report.note);
        free(pdf);
        return 4;
    }
    memset(&page, 0, sizeof(page));
    rc = qw_mupdf_pdf_reader_render_page(reader, 0u, 72u, &page, &report);
    if (rc != 0 || !page.data || page.width != 64u || page.height != 64u) {
        fprintf(stderr, "image page render failed: rc=%d %ux%u note=%s\n", rc,
                page.width, page.height, report.note);
        qw_mupdf_pdf_reader_close(reader);
        free(pdf);
        return 5;
    }
    for (index = 0u; index < page.size; index += 3u) {
        if (page.data[index] != 255u || page.data[index + 1u] != 255u ||
            page.data[index + 2u] != 255u)
            ++non_white;
    }
    if (!non_white) {
        fprintf(stderr, "image XObject was not painted: %s\n", report.note);
        free((void *)page.data);
        qw_mupdf_pdf_reader_close(reader);
        free(pdf);
        return 6;
    }
    printf("PDF image XObject painted: %zu non-white pixels, note=%s\n",
           non_white, report.note);
    free((void *)page.data);
    qw_mupdf_pdf_reader_close(reader);
    free(pdf);
    return 0;
}
