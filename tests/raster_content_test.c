#include <quirewise/convert.h>
#include <quirewise/document.h>

#include "../src/backends/mupdf_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char rect_pdf[] =
    "%PDF-1.4\n"
    "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 144 72] "
    "/Contents 4 0 R >>\nendobj\n"
    "4 0 obj\n<< /Length 24 >>\nstream\n"
    "1 0 0 rg 0 0 72 72 re f\n"
    "endstream\nendobj\n"
    "trailer\n<< /Root 1 0 R /Size 5 >>\n%%EOF\n";

int main(void)
{
    QwMuPdfReader *reader = NULL;
    size_t pages = 0u;
    QwPageImage page;
    QwConvertReport report;
    int rc;
    memset(&report, 0, sizeof(report));
    rc = qw_mupdf_pdf_reader_open((const uint8_t *)rect_pdf,
                                  sizeof(rect_pdf) - 1u, &reader, &pages,
                                  &report);
    if (rc != 0 || !reader || pages != 1u) {
        fprintf(stderr, "rect PDF open failed: rc=%d pages=%zu note=%s\n", rc,
                pages, report.note);
        return 1;
    }
    memset(&page, 0, sizeof(page));
    rc = qw_mupdf_pdf_reader_render_page(reader, 0u, 72u, &page, &report);
    if (rc != 0 || !page.data || page.width != 144u || page.height != 72u) {
        fprintf(stderr, "rect render failed: rc=%d %ux%u note=%s\n", rc,
                page.width, page.height, report.note);
        qw_mupdf_pdf_reader_close(reader);
        return 2;
    }
    {
        const uint8_t *left = page.data + ((72u - 1u - 10u) * 144u + 10u) * 3u;
        const uint8_t *right = page.data + ((72u - 1u - 10u) * 144u + 100u) * 3u;
        if (!(left[0] == 255u && left[1] == 0u && left[2] == 0u)) {
            fprintf(stderr, "left half is not red: %u,%u,%u\n", left[0],
                    left[1], left[2]);
            free((void *)page.data);
            qw_mupdf_pdf_reader_close(reader);
            return 3;
        }
        if (!(right[0] == 255u && right[1] == 255u && right[2] == 255u)) {
            fprintf(stderr, "right half is not white: %u,%u,%u\n", right[0],
                    right[1], right[2]);
            free((void *)page.data);
            qw_mupdf_pdf_reader_close(reader);
            return 4;
        }
    }
    if (!strstr(report.note, "1 painted object")) {
        fprintf(stderr, "rectangle count missing: %s\n", report.note);
        free((void *)page.data);
        qw_mupdf_pdf_reader_close(reader);
        return 5;
    }
    free((void *)page.data);
    qw_mupdf_pdf_reader_close(reader);
    puts("PDF content painting: red left half, white right half at 72dpi");
    return 0;
}
