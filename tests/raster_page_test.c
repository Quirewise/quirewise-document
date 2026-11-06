#include <quirewise/convert.h>
#include <quirewise/document.h>

#include "../src/backends/mupdf_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int all_white(const QwPageImage *page)
{
    size_t index;
    if (!page->data || !page->width || !page->height) return 0;
    for (index = 0u; index < page->width * (size_t)page->height * 3u; ++index) {
        if (page->data[index] != 0xffu) return 0;
    }
    return 1;
}

int main(void)
{
    static const char html[] = "<html><body><p>Raster test</p></body></html>";
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    QwConvertReport report;
    QwMuPdfReader *reader = NULL;
    size_t pages = 0u;
    QwPageImage page;
    int rc;
    memset(&report, 0, sizeof(report));
    rc = qw_mupdf_convert_html_with_page_size(html, sizeof(html) - 1u, 144.0f,
                                              72.0f, &pdf, &pdf_size, &report);
    if (rc != 0 || !pdf) {
        fprintf(stderr, "HTML->PDF failed: rc=%d\n", rc);
        return 1;
    }
    rc = qw_mupdf_pdf_reader_open(pdf, pdf_size, &reader, &pages, &report);
    if (rc != 0 || !reader || pages != 1u) {
        fprintf(stderr, "PDF reader open failed: rc=%d pages=%zu\n", rc, pages);
        free(pdf);
        return 2;
    }
    memset(&page, 0, sizeof(page));
    rc = qw_mupdf_pdf_reader_render_page(reader, 0u, 72u, &page, &report);
    if (rc != 0) {
        fprintf(stderr, "72dpi render failed: rc=%d note=%s\n", rc, report.note);
        qw_mupdf_pdf_reader_close(reader);
        free(pdf);
        return 3;
    }
    if (page.kind != QW_PAGE_IMAGE_RGB24 || page.width != 144u ||
        page.height != 72u || page.size != 144u * 72u * 3u || !all_white(&page)) {
        fprintf(stderr, "unexpected 72dpi raster: kind=%d %ux%u size=%zu\n",
                page.kind, page.width, page.height, page.size);
        free((void *)page.data);
        qw_mupdf_pdf_reader_close(reader);
        free(pdf);
        return 4;
    }
    if (!strstr(report.note, "QW_ROUTE_LOSS_RASTERIZATION")) {
        fprintf(stderr, "rasterisation loss note missing: %s\n", report.note);
        free((void *)page.data);
        qw_mupdf_pdf_reader_close(reader);
        free(pdf);
        return 5;
    }
    free((void *)page.data);
    memset(&page, 0, sizeof(page));
    rc = qw_mupdf_pdf_reader_render_page(reader, 0u, 144u, &page, &report);
    if (rc != 0 || page.width != 288u || page.height != 144u) {
        fprintf(stderr, "144dpi raster mismatch: rc=%d %ux%u\n", rc, page.width,
                page.height);
        free((void *)page.data);
        qw_mupdf_pdf_reader_close(reader);
        free(pdf);
        return 6;
    }
    free((void *)page.data);
    memset(&page, 0, sizeof(page));
    rc = qw_mupdf_pdf_reader_render_page(reader, 1u, 72u, &page, &report);
    if (rc == 0) {
        fprintf(stderr, "out-of-range page rendered unexpectedly\n");
        free((void *)page.data);
        qw_mupdf_pdf_reader_close(reader);
        free(pdf);
        return 7;
    }
    rc = qw_mupdf_pdf_reader_render_page(reader, 0u, 2000u, &page, &report);
    if (rc == 0) {
        fprintf(stderr, "excessive dpi accepted unexpectedly\n");
        qw_mupdf_pdf_reader_close(reader);
        free(pdf);
        return 8;
    }
    qw_mupdf_pdf_reader_close(reader);
    free(pdf);
    puts("PDF rasterisation: 144x72 page at 72dpi -> 144x72 RGB24, 144dpi -> 288x144");
    return 0;
}
