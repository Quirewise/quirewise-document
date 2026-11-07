#include <quirewise/convert.h>
#include <quirewise/document.h>

#include "../src/backends/document_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char prefix[] =
    "%PDF-1.5\n"
    "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    "2 0 obj\n<< /Type /Pages /Kids [6 0 R] /Count 1 >>\nendobj\n"
    "4 0 obj\n<< /Length 25 >>\nstream\nBT /F1 12 Tf 10 40 Td (S) Tj ET\n"
    "endstream\nendobj\n"
    "5 0 obj\n<< /Type /ObjStm /N 1 /First 4 /Length %d >>\nstream\n";
static const char objstm_body[] =
    "6 0 << /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] "
    "/Contents 4 0 R >>";
static const char suffix[] =
    "\nendstream\nendobj\nstartxref\n0\n%%EOF\n";

int main(void)
{
    char *pdf;
    size_t pdf_size;
    const uint8_t *pdfs[2];
    size_t sizes[2];
    uint8_t *merged = NULL;
    size_t merged_size = 0u;
    size_t pages = 0u;
    QwConvertReport report;
    QwPdfReader *reader = NULL;
    size_t reader_pages = 0u;
    int written;
    int rc;
    pdf_size = sizeof(prefix) + sizeof(objstm_body) - 1u + sizeof(suffix) + 32u;
    pdf = (char *)malloc(pdf_size);
    if (!pdf) return 1;
    written = snprintf(pdf, pdf_size, prefix, (int)(sizeof(objstm_body) - 1u));
    if (written <= 0) { free(pdf); return 2; }
    memcpy(pdf + written, objstm_body, sizeof(objstm_body) - 1u);
    memcpy(pdf + written + sizeof(objstm_body) - 1u, suffix,
           sizeof(suffix) - 1u);
    pdf_size = (size_t)written + sizeof(objstm_body) - 1u + sizeof(suffix) - 1u;
    pdfs[0] = (const uint8_t *)pdf;
    pdfs[1] = (const uint8_t *)pdf;
    sizes[0] = pdf_size;
    sizes[1] = pdf_size;
    memset(&report, 0, sizeof(report));
    rc = qw_pdf_merge_documents(pdfs, sizes, 2u, &merged, &merged_size,
                                      &pages, &report);
    if (rc != 0 || !merged || pages != 2u) {
        fprintf(stderr, "ObjStm merge failed: rc=%d pages=%zu note=%s\n", rc,
                pages, report.note);
        free(merged);
        free(pdf);
        return 3;
    }
    rc = qw_pdf_reader_open(merged, merged_size, &reader, &reader_pages,
                                  &report);
    if (rc != 0 || reader_pages != 2u) {
        fprintf(stderr, "merged ObjStm PDF does not open: rc=%d pages=%zu\n",
                rc, reader_pages);
        qw_pdf_reader_close(reader);
        free(merged);
        free(pdf);
        return 4;
    }
    qw_pdf_reader_close(reader);
    free(merged);
    free(pdf);
    puts("PDF merge expands compressed object streams before copying");
    return 0;
}
