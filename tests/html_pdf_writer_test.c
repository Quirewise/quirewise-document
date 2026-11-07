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

static const char *SAMPLE_HTML =
    "<html><head><title>x</title><style>p{color:red}</style></head><body>"
    "<h1>Quirewise internal PDF writer</h1>"
    "<p>The renderer flattens block structure and wraps ASCII text onto "
    "multiple lines so pagination is exercised by the test fixture.</p>"
    "<p>\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95\xEF\xBC\x9A"
    "\xE5\x86\x85\xE9\x83\xA8 HTML \xE8\xBD\xAC PDF "
    "\xE6\xB8\xB2\xE6\x9F\x93\xE5\x99\xA8\xE3\x80\x82</p>"
    "<ul><li>First item</li><li>Second item &amp; more</li></ul>"
    "</body></html>";

static size_t count_occurrences(const uint8_t *data, size_t size,
                                const char *needle)
{
    size_t needle_length = strlen(needle);
    size_t count = 0u;
    size_t index;
    if (!data || !needle_length || size < needle_length) return 0u;
    for (index = 0u; index + needle_length <= size; ++index) {
        if (memcmp(data + index, needle, needle_length) == 0) count += 1u;
    }
    return count;
}

static int contains_ascii(const uint8_t *data, size_t size, const char *text)
{
    size_t text_length = strlen(text);
    size_t index;
    if (!text_length) return 0;
    for (index = 0u; index + text_length <= size; ++index) {
        if (memcmp(data + index, text, text_length) == 0) return 1;
    }
    return 0;
}

int main(void)
{
    uint8_t *pdf = NULL;
    uint8_t *again = NULL;
    uint8_t *empty = NULL;
    size_t size = 0u;
    size_t again_size = 0u;
    size_t empty_size = 0u;
    QwConvertReport report;
    size_t startxref_offset;
    char trailer[64];

    memset(&report, 0, sizeof(report));
    CHECK(qw_doc_convert_html(SAMPLE_HTML, strlen(SAMPLE_HTML), &pdf, &size,
                                &report) == QW_STATUS_OK);
    CHECK(pdf != NULL && size > 200u);
    CHECK(memcmp(pdf, "%PDF-1.4", 8u) == 0);
    CHECK(count_occurrences(pdf, size, "%%EOF") == 1u);
    CHECK(count_occurrences(pdf, size, "/Type /Page ") == 1u);
    CHECK(count_occurrences(pdf, size, "/Type /Font ") == 3u);
    CHECK(count_occurrences(pdf, size, "/Type /FontDescriptor") == 1u);
    CHECK(count_occurrences(pdf, size, "/BaseFont /STSong-Light") == 2u);
    /* ASCII text is emitted as a hex string: "Quirewise" = 5175697265776973652E */
    CHECK(contains_ascii(pdf, size, "517569726577697365"));
    /* Chinese text is UTF-16BE: 4E2D 6587 6D4B 8BD5 = 中文测试 */
    CHECK(contains_ascii(pdf, size, "4E2D65876D4B8BD5"));
    /* The style block must not leak into the output. */
    CHECK(!contains_ascii(pdf, size, "6D6F76696E672078")); /* "m" "x" guard */
    CHECK(!contains_ascii(pdf, size, "636F6C6F723A726564"));

    /* Deterministic output: the same input renders byte-identically. */
    CHECK(qw_doc_convert_html(SAMPLE_HTML, strlen(SAMPLE_HTML), &again,
                                &again_size, NULL) == QW_STATUS_OK);
    CHECK(again_size == size && memcmp(again, pdf, size) == 0);

    /* Empty input still produces one renderable page. */
    CHECK(qw_doc_convert_html("", 0u, &empty, &empty_size, NULL) == QW_STATUS_OK);
    CHECK(empty_size > 100u && count_occurrences(empty, empty_size, "/Type /Page ") == 1u);

    /* startxref must point at the cross-reference table. */
    startxref_offset = 0u;
    {
        size_t index = size;
        while (index > 0u) {
            --index;
            if (index + 9u <= size && memcmp(pdf + index, "startxref", 9u) == 0) {
                if (sscanf((const char *)pdf + index + 9u, "%zu", &startxref_offset) != 1) {
                    CHECK(0);
                }
                break;
            }
        }
    }
    CHECK(startxref_offset > 0u && startxref_offset < size);
    CHECK(memcmp(pdf + startxref_offset, "xref", 4u) == 0);
    snprintf(trailer, sizeof(trailer), "startxref\n%zu\n", startxref_offset);
    CHECK(contains_ascii(pdf, size, trailer));

    /* Invalid arguments are rejected. */
    CHECK(qw_doc_convert_html(SAMPLE_HTML, strlen(SAMPLE_HTML), NULL, &size,
                                NULL) == QW_STATUS_ERR_INVALID_ARGUMENT);

    free(pdf);
    free(again);
    free(empty);
    puts("PASS: internal HTML/PDF writer structure, text, determinism and argument checks");
    return 0;
}
