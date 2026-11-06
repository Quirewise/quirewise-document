#include "quirewise/convert.h"
#include "format_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

/* Minimal flat OpenDocument text: a heading and two paragraphs. */
static const char *FODT_DOCUMENT =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<office:document xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
    "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
    "office:version=\"1.2\" office:mimetype=\"application/vnd.oasis.opendocument.text\">"
    "<office:body><office:text>"
    "<text:h text:outline-level=\"1\">Quirewise end-to-end PDF</text:h>"
    "<text:p>This paragraph comes from a flat ODT document and is converted to PDF "
    "by the internal Quirewise renderer.</text:p>"
    "<text:p>\xE4\xB8\xAD\xE6\x96\x87\xE6\xAE\xB5\xE8\x90\xBD\xE3\x80\x82</text:p>"
    "</office:text></office:body></office:document>";

static int contains(const uint8_t *data, size_t size, const char *text)
{
    size_t length = strlen(text);
    size_t index;
    if (!length) return 0;
    for (index = 0u; index + length <= size; ++index) {
        if (memcmp(data + index, text, length) == 0) return 1;
    }
    return 0;
}

int main(void)
{
    QwConvertOptions options = QW_CONVERT_OPTIONS_INIT;
    QwConvertReport report;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    size_t end_to_end_page_count;

    memset(&report, 0, sizeof(report));
    {
        int rc = qw_format_odt.convert_to_pdf(FODT_DOCUMENT, strlen(FODT_DOCUMENT),
                                              &options, &pdf, &pdf_size, &report);
        if (rc != QW_STATUS_OK) {
            fprintf(stderr, "FODT conversion rc=%d note=%s\n", rc, report.note);
        }
        CHECK(rc == QW_STATUS_OK);
    }
    CHECK(pdf != NULL && pdf_size > 200u);
    CHECK(memcmp(pdf, "%PDF-1.4", 8u) == 0);
    CHECK(contains(pdf, pdf_size, "%%EOF"));
    CHECK(contains(pdf, pdf_size, "/Type /Page "));
    /* Heading text is ASCII and must survive the layout pipeline. */
    CHECK(contains(pdf, pdf_size, "517569726577697365"));
    /* Chinese paragraph: 4E2D 6587 6BB5 843D 3002 = 中文段落。 */
    CHECK(contains(pdf, pdf_size, "4E2D65876BB5843D3002"));

    /* Count pages through the page tree. */
    end_to_end_page_count = 0u;
    {
        size_t index;
        for (index = 0u; index + 12u <= pdf_size; ++index) {
            if (memcmp(pdf + index, "/Type /Page ", 12u) == 0) end_to_end_page_count += 1u;
        }
    }
    CHECK(end_to_end_page_count == 1u);
    printf("end-to-end FODT->PDF: %zu bytes, %zu page(s), note=%s\n",
           pdf_size, end_to_end_page_count, report.note);
    free(pdf);

    /* A document without any text still yields one renderable page. */
    {
        static const char *empty_document =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<office:document xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
            "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
            "office:version=\"1.2\" office:mimetype=\"application/vnd.oasis.opendocument.text\">"
            "<office:body><office:text></office:text></office:body></office:document>";
        uint8_t *empty_pdf = NULL;
        size_t empty_size = 0u;
        CHECK(qw_format_odt.convert_to_pdf(empty_document, strlen(empty_document),
                                           &options, &empty_pdf, &empty_size, NULL) ==
              QW_STATUS_OK);
        CHECK(empty_pdf != NULL && contains(empty_pdf, empty_size, "/Type /Page "));
        free(empty_pdf);
    }
    puts("PASS: FODT end-to-end conversion through the internal PDF renderer");
    return 0;
}
