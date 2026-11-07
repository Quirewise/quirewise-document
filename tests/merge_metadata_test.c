#include <quirewise/convert.h>
#include <quirewise/document.h>

#include "../src/backends/document_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char first_pdf[] =
    "%PDF-1.4\n"
    "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Outlines 5 0 R >>\nendobj\n"
    "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] "
    "/Contents 4 0 R >>\nendobj\n"
    "4 0 obj\n<< /Length 27 >>\nstream\nBT /F1 12 Tf 10 40 Td (One) Tj ET\n"
    "endstream\nendobj\n"
    "5 0 obj\n<< /Type /Outlines /First 6 0 R /Last 6 0 R /Count 1 >>\nendobj\n"
    "6 0 obj\n<< /Title (Bookmark One) /Parent 5 0 R /Dest [3 0 R /Fit] >>\n"
    "endobj\n"
    "7 0 obj\n<< /Title (Merged Meta) /Author (Quirewise) >>\nendobj\n"
    "trailer\n<< /Root 1 0 R /Info 7 0 R /Size 8 >>\n%%EOF\n";

static const char second_pdf[] =
    "%PDF-1.4\n"
    "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] "
    "/Contents 4 0 R >>\nendobj\n"
    "4 0 obj\n<< /Length 27 >>\nstream\nBT /F1 12 Tf 10 40 Td (Two) Tj ET\n"
    "endstream\nendobj\n"
    "trailer\n<< /Root 1 0 R /Size 5 >>\n%%EOF\n";

static int contains(const uint8_t *data, size_t size, const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t i;
    if (!needle_size || needle_size > size) return 0;
    for (i = 0u; i + needle_size <= size; ++i) {
        if (memcmp(data + i, needle, needle_size) == 0) return 1;
    }
    return 0;
}

int main(void)
{
    const uint8_t *pdfs[2];
    size_t sizes[2];
    uint8_t *merged = NULL;
    size_t merged_size = 0u;
    size_t pages = 0u;
    QwConvertReport report;
    QwPdfReader *reader = NULL;
    size_t reader_pages = 0u;
    int rc;
    pdfs[0] = (const uint8_t *)first_pdf;
    pdfs[1] = (const uint8_t *)second_pdf;
    sizes[0] = sizeof(first_pdf) - 1u;
    sizes[1] = sizeof(second_pdf) - 1u;
    memset(&report, 0, sizeof(report));
    rc = qw_pdf_merge_documents(pdfs, sizes, 2u, &merged, &merged_size,
                                      &pages, &report);
    if (rc != 0 || !merged || pages != 2u) {
        fprintf(stderr, "merge failed: rc=%d pages=%zu note=%s\n", rc, pages,
                report.note);
        free(merged);
        return 1;
    }
    if (!contains(merged, merged_size, "/Outlines") ||
        !contains(merged, merged_size, "/Info")) {
        fprintf(stderr, "merge output lost outline/info references\n");
        free(merged);
        return 2;
    }
    if (!contains(merged, merged_size, "Bookmark One") ||
        !contains(merged, merged_size, "Merged Meta")) {
        fprintf(stderr, "merge output lost bookmark or metadata payload\n");
        free(merged);
        return 3;
    }
    rc = qw_pdf_reader_open(merged, merged_size, &reader, &reader_pages,
                                  &report);
    if (rc != 0 || reader_pages != 2u) {
        fprintf(stderr, "merged PDF does not open: rc=%d pages=%zu\n", rc,
                reader_pages);
        qw_pdf_reader_close(reader);
        free(merged);
        return 4;
    }
    qw_pdf_reader_close(reader);
    free(merged);
    puts("PDF merge preserves /Outlines and /Info");
    return 0;
}
