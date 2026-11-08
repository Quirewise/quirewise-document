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
    "<html><body><h1>Quirewise PDF reader</h1>"
    "<p>Text extraction covers ASCII strings and CJK runs.</p>"
    "<p>\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95\xEF\xBC\x9A"
    "\xE5\x86\x85\xE9\x83\xA8 PDF \xE8\xAF\xBB\xE5\x8F\x96\xE5\x99\xA8\xE3\x80\x82</p>"
    "</body></html>";

static int read_file(const char *path, uint8_t **out_data, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *buffer;
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    buffer = (uint8_t *)malloc((size_t)length + 1u);
    if (!buffer) {
        fclose(file);
        return 0;
    }
    if (fread(buffer, 1u, (size_t)length, file) != (size_t)length) {
        free(buffer);
        fclose(file);
        return 0;
    }
    fclose(file);
    *out_data = buffer;
    *out_size = (size_t)length;
    return 1;
}

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

int main(int argc, char **argv)
{
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    QwConvertReport report;
    QwPdfReader *reader = NULL;
    size_t page_count = 0u;
    uint8_t *text = NULL;
    size_t text_size = 0u;

    memset(&report, 0, sizeof(report));
    CHECK(qw_doc_convert_html(SAMPLE_HTML, strlen(SAMPLE_HTML), &pdf, &pdf_size,
                                &report) == QW_STATUS_OK);
    CHECK(qw_pdf_reader_open(pdf, pdf_size, &reader, &page_count, &report) ==
          QW_STATUS_OK);
    CHECK(reader != NULL && page_count == 1u);
    CHECK(qw_pdf_reader_extract_text(reader, &text, &text_size, &report) ==
          QW_STATUS_OK);
    CHECK(text != NULL && text_size > 10u);
    CHECK(contains((const char *)text, text_size, "Quirewise"));
    CHECK(contains((const char *)text, text_size, "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95"));
    printf("extracted %zu bytes from one page: %.60s\n", text_size, (const char *)text);
    free(text);
    text = NULL;
    qw_pdf_reader_close(reader);

    /* Not a PDF. */
    {
        QwPdfReader *bad = NULL;
        size_t pages = 0u;
        static const uint8_t garbage[] = "not a pdf at all";
        CHECK(qw_pdf_reader_open(garbage, sizeof(garbage) - 1u, &bad, &pages,
                                       NULL) == QW_STATUS_ERR_FORMAT);
        CHECK(bad == NULL);
    }
    /* Encrypted documents are rejected with a stable status. */
    {
        QwPdfReader *encrypted = NULL;
        size_t pages = 0u;
        static const char encrypted_pdf[] =
            "%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Encrypt 9 0 R >>\nendobj\n";
        CHECK(qw_pdf_reader_open((const uint8_t *)encrypted_pdf,
                                       strlen(encrypted_pdf), &encrypted, &pages,
                                       NULL) == QW_STATUS_ERR_UNSUPPORTED);
    }
    /* Rasterisation paints the page geometry and reports content loss. */
    {
        QwPageImage image;
        memset(&image, 0, sizeof(image));
        CHECK(qw_pdf_render_page_memory(pdf, pdf_size, 0u, 144u, &image, NULL) ==
              QW_STATUS_OK);
        CHECK(image.data != NULL && image.size > 0u);
        CHECK(image.kind == QW_PAGE_IMAGE_RGB24);
        free((void *)image.data);
    }
    free(pdf);

    /* Optional external fixture (flate-compressed streams). */
    if (argc > 1) {
        uint8_t *fixture = NULL;
        size_t fixture_size = 0u;
        if (read_file(argv[1], &fixture, &fixture_size)) {
            QwPdfReader *external = NULL;
            size_t external_pages = 0u;
            int rc = qw_pdf_reader_open(fixture, fixture_size, &external,
                                              &external_pages, &report);
            CHECK(rc == QW_STATUS_OK);
            CHECK(external_pages >= 1u);
            {
                uint8_t *external_text = NULL;
                size_t external_text_size = 0u;
                CHECK(qw_pdf_reader_extract_text(external, &external_text,
                                                       &external_text_size, NULL) ==
                      QW_STATUS_OK);
                printf("external fixture: %zu page(s), %zu text bytes\n",
                       external_pages, external_text_size);
                free(external_text);
            }
            qw_pdf_reader_close(external);
            free(fixture);
        }
    }
    puts("PASS: internal PDF reader opens PDFs, extracts UTF-8 text and rejects bad input");
    return 0;
}
