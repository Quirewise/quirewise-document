#include "document_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

static int contains(const uint8_t *data, size_t size, const char *text)
{
    size_t length = strlen(text);
    size_t index;
    if (!length || size < length) return 0;
    for (index = 0u; index + length <= size; ++index) {
        if (memcmp(data + index, text, length) == 0) return 1;
    }
    return 0;
}

int main(void)
{
    static const char *first =
        "<html><body><h1>Merged document one</h1>"
        "<p>The first input paragraph.</p></body></html>";
    static const char *second =
        "<html><body><h1>Merged document two</h1>"
        "<p>The second input paragraph.</p></body></html>";
    uint8_t *pdf_a = NULL;
    uint8_t *pdf_b = NULL;
    size_t size_a = 0u;
    size_t size_b = 0u;
    const uint8_t *inputs[2];
    size_t sizes[2];
    uint8_t *merged = NULL;
    size_t merged_size = 0u;
    size_t merged_pages = 0u;
    QwConvertReport report;

    memset(&report, 0, sizeof(report));
    CHECK(qw_doc_convert_html(first, strlen(first), &pdf_a, &size_a, NULL) ==
          QW_STATUS_OK);
    CHECK(qw_doc_convert_html(second, strlen(second), &pdf_b, &size_b, NULL) ==
          QW_STATUS_OK);

    inputs[0] = pdf_a;
    inputs[1] = pdf_b;
    sizes[0] = size_a;
    sizes[1] = size_b;
    CHECK(qw_pdf_merge_documents(inputs, sizes, 2u, &merged, &merged_size,
                                       &merged_pages, &report) == QW_STATUS_OK);
    CHECK(merged != NULL && merged_size > 400u);
    CHECK(merged_pages == 2u);
    CHECK(memcmp(merged, "%PDF-1.4", 8u) == 0);
    CHECK(contains(merged, merged_size, "%%EOF"));
    CHECK(contains(merged, merged_size, "/Type /Catalog"));
    CHECK(contains(merged, merged_size, "/Type /Pages /Count 2"));

    /* The merged document must be readable by the internal reader and contain
     * text from both inputs. */
    {
        QwPdfReader *reader = NULL;
        size_t pages = 0u;
        uint8_t *text = NULL;
        size_t text_size = 0u;
        CHECK(qw_pdf_reader_open(merged, merged_size, &reader, &pages, NULL) ==
              QW_STATUS_OK);
        CHECK(pages == 2u);
        CHECK(qw_pdf_reader_extract_text(reader, &text, &text_size, NULL) ==
              QW_STATUS_OK);
        CHECK(contains(text, text_size, "Merged document one"));
        CHECK(contains(text, text_size, "Merged document two"));
        free(text);
        qw_pdf_reader_close(reader);
    }

    /* Merging one input keeps its page count. */
    {
        uint8_t *single = NULL;
        size_t single_size = 0u;
        size_t single_pages = 0u;
        CHECK(qw_pdf_merge_documents(inputs, sizes, 1u, &single, &single_size,
                                           &single_pages, NULL) == QW_STATUS_OK);
        CHECK(single_pages == 1u);
        free(single);
    }

    /* Bad arguments and non-PDF inputs are rejected. These calls clear their
     * output pointers, so the merged buffer must be released first. */
    free(merged);
    merged = NULL;
    {
        uint8_t *scratch = NULL;
        size_t scratch_size = 0u;
        CHECK(qw_pdf_merge_documents(NULL, sizes, 2u, &scratch, &scratch_size,
                                           NULL, NULL) ==
              QW_STATUS_ERR_INVALID_ARGUMENT);
        CHECK(qw_pdf_merge_documents(inputs, sizes, 0u, &scratch, &scratch_size,
                                           NULL, NULL) ==
              QW_STATUS_ERR_INVALID_ARGUMENT);
        CHECK(scratch == NULL && scratch_size == 0u);
    }
    {
        const uint8_t junk[8] = {0u};
        const uint8_t *bad[1] = {junk};
        size_t bad_size[1] = {sizeof(junk)};
        uint8_t *scratch = NULL;
        size_t scratch_size = 0u;
        CHECK(qw_pdf_merge_documents(bad, bad_size, 1u, &scratch, &scratch_size,
                                           NULL, NULL) == QW_STATUS_ERR_FORMAT);
        CHECK(scratch == NULL);
    }

    free(pdf_a);
    free(pdf_b);
    puts("PASS: PDF merge produces a readable two-page document with text from both inputs");
    return 0;
}
