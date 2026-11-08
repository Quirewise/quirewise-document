/*
 * Public PDF API: open, render, extract text, merge, release.
 *
 * Everything here goes through the installed header (quirewise/pdf.h); the
 * private backend header is not included on purpose.
 */

#include <quirewise/convert.h>
#include <quirewise/document.h>
#include <quirewise/pdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

static const char sample_html[] =
    "<html><body><h1>Public API</h1>"
    "<p>PDF round trip through the installed header.</p></body></html>";

static int contains(const char *data, size_t size, const char *text)
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
    QwConvertReport report;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    QwPdfDocument *document = NULL;
    size_t pages = 0u;
    QwPageImage page;
    uint8_t *text = NULL;
    size_t text_size = 0u;
    uint8_t *merged = NULL;
    size_t merged_size = 0u;
    size_t merged_pages = 0u;
    const uint8_t *inputs[2];
    size_t input_sizes[2];
    int rc;

    memset(&report, 0, sizeof(report));
    rc = qw_convert_to_pdf_memory(sample_html, sizeof(sample_html) - 1u, NULL,
                                  &pdf, &pdf_size, &report);
    CHECK(rc == QW_STATUS_OK && pdf && pdf_size > 8u);
    CHECK(memcmp(pdf, "%PDF-", 5u) == 0);

    rc = qw_pdf_open(pdf, pdf_size, &document, &pages, &report);
    CHECK(rc == QW_STATUS_OK && document && pages == 1u);

    {
        double media[4];
        double crop[4];
        int rotation = -1;
        rc = qw_pdf_page_geometry(document, 0u, media, crop, &rotation);
        CHECK(rc == QW_STATUS_OK);
        CHECK(media[2] > media[0] && media[3] > media[1]);
        CHECK(crop[2] > crop[0] && crop[3] > crop[1]);
        CHECK(rotation == 0);
        CHECK(qw_pdf_page_geometry(document, 1u, media, crop, &rotation) ==
              QW_STATUS_ERR_INVALID_ARGUMENT);
    }

    memset(&page, 0, sizeof(page));
    rc = qw_pdf_render_page(document, 0u, 72u, &page, &report);
    CHECK(rc == QW_STATUS_OK && page.data && page.width > 0u && page.height > 0u);
    qw_pdf_page_free(&page);
    CHECK(page.data == NULL && page.size == 0u);

    rc = qw_pdf_extract_text(document, &text, &text_size, &report);
    CHECK(rc == QW_STATUS_OK && text && text_size > 0u);
    CHECK(contains((const char *)text, text_size, "Public API"));
    qw_pdf_free(text);

    inputs[0] = pdf;
    inputs[1] = pdf;
    input_sizes[0] = pdf_size;
    input_sizes[1] = pdf_size;
    rc = qw_pdf_merge(inputs, input_sizes, 2u, &merged, &merged_size,
                      &merged_pages, &report);
    CHECK(rc == QW_STATUS_OK && merged && merged_size > 8u);
    CHECK(merged_pages == 2u && memcmp(merged, "%PDF-", 5u) == 0);
    qw_pdf_free(merged);

    qw_pdf_close(document);
    qw_pdf_free(pdf);

    /* Argument validation: every entry point rejects what it cannot own. */
    CHECK(qw_pdf_open(NULL, 0u, &document, &pages, NULL) ==
          QW_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(qw_pdf_open((const uint8_t *)"not a pdf", 9u, &document, &pages,
                      &report) != QW_STATUS_OK);
    CHECK(document == NULL);
    CHECK(qw_pdf_render_page(NULL, 0u, 72u, &page, NULL) ==
          QW_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(qw_pdf_open_structure(NULL, 0u, &document, &pages, NULL) ==
          QW_STATUS_ERR_INVALID_ARGUMENT);
    {
        double media[4];
        double crop[4];
        int rotation = 0;
        CHECK(qw_pdf_page_geometry(NULL, 0u, media, crop, &rotation) ==
              QW_STATUS_ERR_INVALID_ARGUMENT);
    }
    CHECK(qw_pdf_extract_text(NULL, &text, &text_size, NULL) ==
          QW_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(qw_pdf_merge(NULL, NULL, 0u, &merged, &merged_size, NULL, NULL) ==
          QW_STATUS_ERR_INVALID_ARGUMENT);
    qw_pdf_close(NULL);
    qw_pdf_page_free(NULL);
    qw_pdf_free(NULL);
    return 0;
}
