/*
 * Regression: page order must follow the catalog tree, not file order.
 *
 * The fixture writes page 10 before page 9 but lists them as [9 0 R 10 0 R].
 * A marker scan therefore reports FIRST,SECOND; the tree reports
 * SECOND,FIRST. The same order must survive a merge, and merging must not
 * rewrite the outline item's /Parent to the new page tree.
 */

#include "document_backend.h"

#include <quirewise/document.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

static const char pdf_text[] =
    "%PDF-1.4\n"
    "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Outlines 7 0 R >>\nendobj\n"
    "2 0 obj\n<< /Type /Pages /Kids [9 0 R 10 0 R] /Count 2 >>\nendobj\n"
    "10 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] "
    "/Resources << /Font << /F1 6 0 R >> >> /Contents 5 0 R >>\nendobj\n"
    "9 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] "
    "/Resources << /Font << /F1 6 0 R >> >> /Contents 4 0 R >>\nendobj\n"
    "4 0 obj\n<< /Length 37 >>\nstream\n"
    "BT /F1 12 Tf 10 40 Td (SECOND) Tj ET\n"
    "endstream\nendobj\n"
    "5 0 obj\n<< /Length 36 >>\nstream\n"
    "BT /F1 12 Tf 10 40 Td (FIRST) Tj ET\n"
    "endstream\nendobj\n"
    "6 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\n"
    "endobj\n"
    "7 0 obj\n<< /Type /Outlines /First 8 0 R /Last 8 0 R /Count 1 >>\n"
    "endobj\n"
    "8 0 obj\n<< /Title (Bookmark) /Parent 7 0 R /Dest [9 0 R /Fit] >>\n"
    "endobj\n"
    "trailer\n<< /Root 1 0 R >>\n"
    "%%EOF\n";

static long index_of(const char *data, size_t size, const char *text)
{
    size_t length = strlen(text);
    size_t index;
    if (!length || size < length) return -1;
    for (index = 0u; index + length <= size; ++index) {
        if (memcmp(data + index, text, length) == 0) return (long)index;
    }
    return -1;
}

static int contains(const char *data, size_t size, const char *text)
{
    return index_of(data, size, text) >= 0;
}

int main(void)
{
    const uint8_t *inputs[1];
    size_t input_sizes[1];
    QwPdfReader *reader = NULL;
    size_t pages = 0u;
    QwConvertReport report;
    uint8_t *text = NULL;
    size_t text_size = 0u;
    uint8_t *merged = NULL;
    size_t merged_size = 0u;
    size_t merged_pages = 0u;
    long second;
    long first;
    int rc;

    memset(&report, 0, sizeof(report));
    rc = qw_pdf_reader_open((const uint8_t *)pdf_text, sizeof(pdf_text) - 1u,
                            &reader, &pages, &report);
    CHECK(rc == QW_STATUS_OK && reader && pages == 2u);
    rc = qw_pdf_reader_extract_text(reader, &text, &text_size, &report);
    CHECK(rc == QW_STATUS_OK && text && text_size > 0u);
    second = index_of((const char *)text, text_size, "SECOND");
    first = index_of((const char *)text, text_size, "FIRST");
    CHECK(second >= 0 && first >= 0 && second < first);
    free(text);
    qw_pdf_reader_close(reader);

    inputs[0] = (const uint8_t *)pdf_text;
    input_sizes[0] = sizeof(pdf_text) - 1u;
    memset(&report, 0, sizeof(report));
    rc = qw_pdf_merge_documents(inputs, input_sizes, 1u, &merged, &merged_size,
                                &merged_pages, &report);
    CHECK(rc == QW_STATUS_OK && merged && merged_pages == 2u);
    /* The outline item keeps its own parent (7), not the new page tree. */
    CHECK(contains((const char *)merged, merged_size,
                   "/Title (Bookmark) /Parent 7 0 R"));
    rc = qw_pdf_reader_open(merged, merged_size, &reader, &pages, &report);
    CHECK(rc == QW_STATUS_OK && reader && pages == 2u);
    rc = qw_pdf_reader_extract_text(reader, &text, &text_size, &report);
    CHECK(rc == QW_STATUS_OK && text && text_size > 0u);
    second = index_of((const char *)text, text_size, "SECOND");
    first = index_of((const char *)text, text_size, "FIRST");
    CHECK(second >= 0 && first >= 0 && second < first);
    free(text);
    qw_pdf_reader_close(reader);
    free(merged);
    return 0;
}
