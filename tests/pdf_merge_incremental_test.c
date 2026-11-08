/*
 * Regression: merging an incrementally updated PDF.
 *
 * The merger used to copy every "N 0 obj" it found in the file, so a document
 * with two revisions of object 4 produced duplicate object numbers, and it
 * trimmed trailing EOLs from stream payloads while leaving /Length unchanged.
 *
 * Both are checked here: exactly one definition of object 4 survives, the
 * content stream keeps its trailing newline, and text extraction returns the
 * newest revision only.
 */

#include "document_backend.h"
#include "pdf_incremental_fixture.h"

#include <quirewise/document.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

static size_t count_occurrences(const uint8_t *data, size_t size,
                                const char *text)
{
    size_t length = strlen(text);
    size_t count = 0u;
    size_t index = 0u;
    while (index + length <= size) {
        if (memcmp(data + index, text, length) == 0) {
            ++count;
            index += length;
        } else {
            ++index;
        }
    }
    return count;
}

static int contains(const uint8_t *data, size_t size, const char *text)
{
    return count_occurrences(data, size, text) > 0u;
}

static int extract_text(const uint8_t *pdf, size_t size, uint8_t **out_text,
                        size_t *out_size)
{
    QwPdfReader *reader = NULL;
    size_t pages = 0u;
    QwConvertReport report;
    int rc;
    memset(&report, 0, sizeof(report));
    rc = qw_pdf_reader_open(pdf, size, &reader, &pages, &report);
    if (rc != 0 || !reader) return rc;
    rc = qw_pdf_reader_extract_text(reader, out_text, out_size, &report);
    qw_pdf_reader_close(reader);
    return rc;
}

int main(void)
{
    static uint8_t pdf[8192];
    size_t size = pdf_build_incremental(pdf, sizeof(pdf));
    const uint8_t *inputs[2];
    size_t input_sizes[2];
    uint8_t *merged = NULL;
    size_t merged_size = 0u;
    size_t merged_pages = 0u;
    QwConvertReport report;
    uint8_t *text = NULL;
    size_t text_size = 0u;
    int rc;

    CHECK(size > 0u);
    inputs[0] = pdf;
    input_sizes[0] = size;
    memset(&report, 0, sizeof(report));
    rc = qw_pdf_merge_documents(inputs, input_sizes, 1u, &merged, &merged_size,
                                &merged_pages, &report);
    CHECK(rc == QW_STATUS_OK && merged && merged_pages == 1u);
    /* One definition per object number: the stale copy must be dropped. */
    CHECK(count_occurrences(merged, merged_size, "4 0 obj") == 1u);
    /* The payload keeps its trailing newline; /Length stays consistent. */
    CHECK(contains(merged, merged_size, "ET\n\nendstream"));
    rc = extract_text(merged, merged_size, &text, &text_size);
    CHECK(rc == 0 && text && text_size > 0u);
    CHECK(contains(text, text_size, "NEW"));
    CHECK(!contains(text, text_size, "OLD"));
    free(text);
    free(merged);

    /* Two inputs: pages from both, each object set renumbered independently. */
    merged = NULL;
    merged_size = 0u;
    merged_pages = 0u;
    inputs[1] = pdf;
    input_sizes[1] = size;
    memset(&report, 0, sizeof(report));
    rc = qw_pdf_merge_documents(inputs, input_sizes, 2u, &merged, &merged_size,
                                &merged_pages, &report);
    CHECK(rc == QW_STATUS_OK && merged && merged_pages == 2u);
    CHECK(count_occurrences(merged, merged_size, "4 0 obj") == 1u);
    rc = extract_text(merged, merged_size, &text, &text_size);
    CHECK(rc == 0 && text && text_size > 0u);
    CHECK(count_occurrences(text, text_size, "NEW") == 2u);
    free(text);
    free(merged);
    return 0;
}
