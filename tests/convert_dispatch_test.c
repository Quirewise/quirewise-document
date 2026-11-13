#include "quirewise/convert.h"
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

static const char *FODT =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<office:document xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
    "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" office:version=\"1.2\" "
    "office:mimetype=\"application/vnd.oasis.opendocument.text\">"
    "<office:body><office:text><text:h text:outline-level=\"1\">Dispatch</text:h>"
    "<text:p>Flat ODT through the public conversion API.</text:p>"
    "</office:text></office:body></office:document>";

static const char *HTML =
    "<html><body><h1>Dispatch HTML</h1><p>Rendered by the internal writer.</p></body></html>";

static int is_pdf(const uint8_t *data, size_t size)
{
    return data && size > 8u && memcmp(data, "%PDF-", 5u) == 0;
}

int main(void)
{
    QwConvertOptions options = QW_CONVERT_OPTIONS_INIT;
    QwConvertReport report;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;

    /* RTF is routed through the text reader and the layout writer. */
    {
        static const char rtf[] =
            "{\\rtf1\\ansi\\deff0 {\\fonttbl {\\f0 Times New Roman;}}"
            "\\f0\\fs24 Hello from the RTF dispatch route.\\par}";
        uint8_t *text = NULL;
        size_t text_size = 0u;
        int rc = qw_convert_to_pdf_memory(rtf, sizeof(rtf) - 1u, &options, &pdf,
                                          &pdf_size, &report);
        CHECK(rc == QW_STATUS_OK);
        CHECK(is_pdf(pdf, pdf_size));
        rc = qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_TXT, &options,
                                         &text, &text_size, &report);
        CHECK(rc == QW_STATUS_OK);
        CHECK(text_size > 0u);
        CHECK(strstr((const char *)text, "RTF dispatch route") != NULL);
        qw_buffer_free(text);
        free(pdf);
        pdf = NULL;
        pdf_size = 0u;
    }

    /* EML is routed through the email reader and the layout writer. */
    {
        static const char eml[] =
            "From: sender@example.com\r\n"
            "To: receiver@example.com\r\n"
            "Subject: Dispatch EML\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "\r\n"
            "Hello from the EML dispatch route.\r\n";
        uint8_t *text = NULL;
        size_t text_size = 0u;
        int rc = qw_convert_to_pdf_memory(eml, sizeof(eml) - 1u, &options, &pdf,
                                          &pdf_size, &report);
        CHECK(rc == QW_STATUS_OK);
        CHECK(is_pdf(pdf, pdf_size));
        rc = qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_TXT, &options,
                                         &text, &text_size, &report);
        CHECK(rc == QW_STATUS_OK);
        CHECK(text_size > 0u);
        CHECK(strstr((const char *)text, "EML dispatch route") != NULL);
        qw_buffer_free(text);
        free(pdf);
        pdf = NULL;
        pdf_size = 0u;
    }

    memset(&report, 0, sizeof(report));
    CHECK(qw_convert_to_pdf_memory(FODT, strlen(FODT), &options, &pdf, &pdf_size,
                                   &report) == QW_STATUS_OK);
    CHECK(is_pdf(pdf, pdf_size));
    free(pdf);
    pdf = NULL;

    CHECK(qw_convert_to_pdf_memory(HTML, strlen(HTML), &options, &pdf, &pdf_size,
                                   NULL) == QW_STATUS_OK);
    CHECK(is_pdf(pdf, pdf_size));
    free(pdf);
    pdf = NULL;

    /* A PDF is passed through unchanged. */
    {
        static const char small_pdf[] = "%PDF-1.4\n%%EOF\n";
        size_t expected = sizeof(small_pdf) - 1u;
        CHECK(qw_convert_to_pdf_memory(small_pdf, expected, &options, &pdf,
                                       &pdf_size, NULL) == QW_STATUS_OK);
        CHECK(pdf_size == expected && memcmp(pdf, small_pdf, expected) == 0);
        free(pdf);
        pdf = NULL;
    }

    /* Unknown binary input is rejected with a defined status. */
    {
        static const uint8_t junk[] = {0x01u, 0x02u, 0x03u, 0x04u};
        QwConvertReport junk_report;
        memset(&junk_report, 0, sizeof(junk_report));
        CHECK(qw_convert_to_pdf_memory(junk, sizeof(junk), &options, &pdf, &pdf_size,
                                       &junk_report) == QW_STATUS_ERR_UNSUPPORTED);
        CHECK(pdf == NULL && pdf_size == 0u);
        CHECK(junk_report.note[0] != '\0');
    }

    /* Non-OpenDocument XML is rejected, not silently rendered. */
    {
        static const char other_xml[] =
            "<?xml version=\"1.0\"?><root><child/></root>";
        CHECK(qw_convert_to_pdf_memory(other_xml, sizeof(other_xml) - 1u, &options,
                                       &pdf, &pdf_size, NULL) ==
              QW_STATUS_ERR_FORMAT);
        CHECK(pdf == NULL);
    }

    /* A ZIP container that is neither DOCX nor ODT is reported as unsupported,
     * not as a broken OpenDocument package. */
    {
        static const uint8_t zip_like[] = {'P', 'K', 0x03u, 0x04u, 0u, 0u, 0u, 0u};
        QwConvertReport zip_report;
        memset(&zip_report, 0, sizeof(zip_report));
        CHECK(qw_convert_to_pdf_memory(zip_like, sizeof(zip_like), &options, &pdf,
                                       &pdf_size, &zip_report) ==
              QW_STATUS_ERR_UNSUPPORTED);
        CHECK(pdf == NULL && pdf_size == 0u);
        CHECK(zip_report.note[0] != '\0');
    }

    /* The historical convert-memory entry renders ODT/FODT/HTML internally. */
    CHECK(qw_doc_convert_memory((const uint8_t *)FODT, strlen(FODT), ".fodt", 0,
                                  &pdf, &pdf_size, &report) == QW_STATUS_OK);
    CHECK(is_pdf(pdf, pdf_size));
    free(pdf);
    pdf = NULL;
    CHECK(qw_doc_convert_memory((const uint8_t *)FODT, strlen(FODT), NULL, 0,
                                  &pdf, &pdf_size, NULL) == QW_STATUS_OK);
    CHECK(is_pdf(pdf, pdf_size));
    free(pdf);
    pdf = NULL;
    CHECK(qw_doc_convert_memory((const uint8_t *)HTML, strlen(HTML), ".html", 0,
                                  &pdf, &pdf_size, NULL) == QW_STATUS_OK);
    CHECK(is_pdf(pdf, pdf_size));
    free(pdf);
    pdf = NULL;
    {
        static const char small_pdf[] = "%PDF-1.4\n%%EOF\n";
        size_t expected = sizeof(small_pdf) - 1u;
        CHECK(qw_doc_convert_memory((const uint8_t *)small_pdf, expected, ".pdf",
                                      0, &pdf, &pdf_size, NULL) == QW_STATUS_OK);
        CHECK(pdf_size == expected);
        free(pdf);
        pdf = NULL;
    }
    {
        static const uint8_t junk[] = {0x01u, 0x02u, 0x03u, 0x04u};
        CHECK(qw_doc_convert_memory(junk, sizeof(junk), ".bin", 0, &pdf,
                                      &pdf_size, NULL) == QW_STATUS_ERR_UNSUPPORTED);
        pdf = NULL;
        CHECK(qw_doc_convert_memory((const uint8_t *)FODT, strlen(FODT), ".fodt",
                                      1, &pdf, &pdf_size, NULL) == QW_STATUS_OK);
        CHECK(is_pdf(pdf, pdf_size));
        free(pdf);
        pdf = NULL;
    }

    CHECK(qw_convert_to_pdf_memory(NULL, 0u, &options, &pdf, &pdf_size, NULL) ==
          QW_STATUS_ERR_INVALID_ARGUMENT);
    puts("PASS: public PDF conversion dispatch (FODT, HTML, PDF passthrough, rejections)");
    return 0;
}
