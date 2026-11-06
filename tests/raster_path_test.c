#include <quirewise/convert.h>
#include <quirewise/document.h>

#include "../src/backends/mupdf_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char path_pdf[] =
    "%PDF-1.4\n"
    "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 64 64] "
    "/Contents 4 0 R >>\nendobj\n"
    "4 0 obj\n<< /Length 44 >>\nstream\n"
    "0 1 0 rg 0 0 m 64 0 l 32 64 l f\n"
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
    rc = qw_mupdf_pdf_reader_open((const uint8_t *)path_pdf,
                                  sizeof(path_pdf) - 1u, &reader, &pages,
                                  &report);
    if (rc != 0 || !reader || pages != 1u) {
        fprintf(stderr, "path PDF open failed: rc=%d pages=%zu note=%s\n", rc,
                pages, report.note);
        return 1;
    }
    memset(&page, 0, sizeof(page));
    rc = qw_mupdf_pdf_reader_render_page(reader, 0u, 72u, &page, &report);
    if (rc != 0 || !page.data || page.width != 64u || page.height != 64u) {
        fprintf(stderr, "path render failed: rc=%d %ux%u note=%s\n", rc,
                page.width, page.height, report.note);
        qw_mupdf_pdf_reader_close(reader);
        return 2;
    }
    {
        const uint8_t *inside = page.data + ((64u - 1u - 16u) * 64u + 32u) * 3u;
        const uint8_t *outside = page.data + ((64u - 1u - 60u) * 64u + 2u) * 3u;
        if (!(inside[0] == 0u && inside[1] == 255u && inside[2] == 0u)) {
            fprintf(stderr, "triangle interior is not green: %u,%u,%u\n",
                    inside[0], inside[1], inside[2]);
            free((void *)page.data);
            qw_mupdf_pdf_reader_close(reader);
            return 3;
        }
        if (!(outside[0] == 255u && outside[1] == 255u && outside[2] == 255u)) {
            fprintf(stderr, "outside pixel is not white: %u,%u,%u\n", outside[0],
                    outside[1], outside[2]);
            free((void *)page.data);
            qw_mupdf_pdf_reader_close(reader);
            return 4;
        }
    }
    if (!strstr(report.note, "1 painted object")) {
        fprintf(stderr, "painted count missing: %s\n", report.note);
        free((void *)page.data);
        qw_mupdf_pdf_reader_close(reader);
        return 5;
    }
    free((void *)page.data);
    qw_mupdf_pdf_reader_close(reader);
    puts("PDF path painting: green triangle interior, white outside");
    return 0;
}
