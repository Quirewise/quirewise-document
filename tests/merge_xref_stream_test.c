#include <quirewise/convert.h>
#include <quirewise/document.h>

#include "../src/backends/mupdf_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char xref_prefix[] =
    "%PDF-1.5\n"
    "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] "
    "/Contents 4 0 R >>\nendobj\n"
    "4 0 obj\n<< /Length 25 >>\nstream\nBT /F1 12 Tf 10 40 Td (X) Tj ET\n"
    "endstream\nendobj\n"
    "5 0 obj\n<< /Type /XRef /Size 6 /W [1 2 1] /Root 1 0 R /Length 24 >>\n"
    "stream\n";

static const char xref_suffix[] =
    "\nendstream\nendobj\nstartxref\n0\n%%EOF\n";

int main(void)
{
    static const unsigned char entries[24] = {
        0, 0, 0, 255,
        0, 0, 9, 0,
        0, 0, 55, 0,
        0, 0, 105, 0,
        0, 0, 180, 0,
        0, 0, 230, 0
    };
    char *pdf;
    size_t pdf_size;
    const uint8_t *pdfs[2];
    size_t sizes[2];
    uint8_t *merged = NULL;
    size_t merged_size = 0u;
    size_t pages = 0u;
    QwConvertReport report;
    QwMuPdfReader *reader = NULL;
    size_t reader_pages = 0u;
    int rc;
    pdf_size = sizeof(xref_prefix) - 1u + sizeof(entries) +
               sizeof(xref_suffix) - 1u;
    pdf = (char *)malloc(pdf_size);
    if (!pdf) return 1;
    memcpy(pdf, xref_prefix, sizeof(xref_prefix) - 1u);
    memcpy(pdf + sizeof(xref_prefix) - 1u, entries, sizeof(entries));
    memcpy(pdf + sizeof(xref_prefix) - 1u + sizeof(entries), xref_suffix,
           sizeof(xref_suffix) - 1u);
    pdfs[0] = (const uint8_t *)pdf;
    pdfs[1] = (const uint8_t *)pdf;
    sizes[0] = pdf_size;
    sizes[1] = pdf_size;
    memset(&report, 0, sizeof(report));
    rc = qw_mupdf_merge_pdf_documents(pdfs, sizes, 2u, &merged, &merged_size,
                                      &pages, &report);
    if (rc != 0 || !merged || pages != 2u) {
        fprintf(stderr, "xref-stream merge failed: rc=%d pages=%zu note=%s\n",
                rc, pages, report.note);
        free(merged);
        free(pdf);
        return 2;
    }
    rc = qw_mupdf_pdf_reader_open(merged, merged_size, &reader, &reader_pages,
                                  &report);
    if (rc != 0 || reader_pages != 2u) {
        fprintf(stderr, "merged xref-stream PDF does not open: rc=%d pages=%zu\n",
                rc, reader_pages);
        qw_mupdf_pdf_reader_close(reader);
        free(merged);
        free(pdf);
        return 3;
    }
    qw_mupdf_pdf_reader_close(reader);
    free(merged);
    free(pdf);
    puts("PDF merge accepts xref-stream documents and writes a classic xref");
    return 0;
}
