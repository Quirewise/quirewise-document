/*
 * Conversion routes: PDF input to TXT/PNG/PDF through the public
 * qw_convert_to_format_memory entry point, plus the documented refusals.
 */

#include <quirewise/convert.h>
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

static const char sample_html[] =
    "<html><body><h1>Route</h1><p>Route text body.</p></body></html>";

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

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

int main(void)
{
    QwConvertReport report;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    uint8_t *out = NULL;
    size_t out_size = 0u;
    int rc;

    memset(&report, 0, sizeof(report));
    rc = qw_convert_to_pdf_memory(sample_html, sizeof(sample_html) - 1u, NULL,
                                  &pdf, &pdf_size, &report);
    CHECK(rc == QW_STATUS_OK && pdf && pdf_size > 8u);

    /* PDF -> PDF is a byte-identical passthrough. */
    out = NULL;
    out_size = 0u;
    rc = qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_PDF, NULL, &out,
                                     &out_size, &report);
    CHECK(rc == QW_STATUS_OK && out && out_size == pdf_size);
    CHECK(memcmp(out, pdf, pdf_size) == 0);
    qw_buffer_free(out);

    /* PDF -> TXT extracts the text written by the layout writer. */
    out = NULL;
    out_size = 0u;
    rc = qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_TXT, NULL, &out,
                                     &out_size, &report);
    CHECK(rc == QW_STATUS_OK && out && out_size > 0u);
    CHECK(contains((const char *)out, out_size, "Route"));
    qw_buffer_free(out);

    /* PDF -> PNG renders page 1 and encodes a real PNG. */
    out = NULL;
    out_size = 0u;
    rc = qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_PNG, NULL, &out,
                                     &out_size, &report);
    CHECK(rc == QW_STATUS_OK && out && out_size > 33u);
    CHECK(out[0] == 0x89u && memcmp(out + 1, "PNG", 3u) == 0);
    CHECK(memcmp(out + 12, "IHDR", 4u) == 0);
    CHECK(read_be32(out + 16) > 0u && read_be32(out + 20) > 0u);
    qw_buffer_free(out);

    /* JPEG needs an encoder this package does not ship: refuse with a note. */
    out = NULL;
    out_size = 0u;
    rc = qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_JPG, NULL, &out,
                                     &out_size, &report);
    CHECK(rc == QW_STATUS_ERR_UNSUPPORTED && out == NULL);

    /* Non-PDF input: PDF output still works through the layout converter. */
    out = NULL;
    out_size = 0u;
    rc = qw_convert_to_format_memory(sample_html, sizeof(sample_html) - 1u,
                                     QW_FORMAT_PDF, NULL, &out, &out_size,
                                     &report);
    CHECK(rc == QW_STATUS_OK && out && out_size > 8u);
    CHECK(memcmp(out, "%PDF-", 5u) == 0);
    qw_buffer_free(out);

    /* Non-PDF input to PNG is not implemented yet. */
    out = NULL;
    out_size = 0u;
    rc = qw_convert_to_format_memory(sample_html, sizeof(sample_html) - 1u,
                                     QW_FORMAT_PNG, NULL, &out, &out_size,
                                     &report);
    CHECK(rc == QW_STATUS_ERR_UNSUPPORTED && out == NULL);

    qw_buffer_free(pdf);
    return 0;
}
