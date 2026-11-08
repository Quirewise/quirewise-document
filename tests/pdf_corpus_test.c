/*
 * Real-world PDF corpus check.
 *
 * The workspace keeps a small document corpus used by the pre-migration
 * application (app/testfile). Copying those binaries into an independent
 * library repository would duplicate license/provenance material, so the test
 * reads them in place: the default directory is a workspace-relative path and
 * QUIREWISE_PDF_CORPUS_DIR overrides it. When the directory is absent the test
 * reports a skip and succeeds, which keeps standalone package builds valid.
 *
 * Covered: a normal 2-page PDF and its PDF/A variant. For each file the test
 * opens the document, checks the page count, extracts non-empty text and
 * rasterises the first page.
 */

#include <quirewise/document.h>
#include <quirewise/pdf.h>
#include <quirewise/status.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_PDF_CORPUS_DIR
#define QUIREWISE_PDF_CORPUS_DIR "app/testfile/pdf"
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

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
    buffer = (uint8_t *)malloc((size_t)length);
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

static int check_document(const char *label, const char *path,
                          size_t expected_pages)
{
    uint8_t *data = NULL;
    size_t size = 0u;
    QwPdfDocument *document = NULL;
    size_t pages = 0u;
    QwConvertReport report;
    uint8_t *text = NULL;
    size_t text_size = 0u;
    QwPageImage page;
    int rc;

    if (!read_file(path, &data, &size)) {
        fprintf(stderr, "%s: cannot read %s\n", label, path);
        return 2;
    }
    memset(&report, 0, sizeof(report));
    rc = qw_pdf_open(data, size, &document, &pages, &report);
    if (rc != QW_STATUS_OK || !document) {
        fprintf(stderr, "%s: open rc=%d note=%s\n", label, rc, report.note);
        free(data);
        return 3;
    }
    if (pages != expected_pages) {
        fprintf(stderr, "%s: expected %zu page(s), got %zu\n", label,
                expected_pages, pages);
        qw_pdf_close(document);
        free(data);
        return 4;
    }
    rc = qw_pdf_extract_text(document, &text, &text_size, &report);
    if (rc != QW_STATUS_OK || !text || text_size == 0u) {
        fprintf(stderr, "%s: text extraction rc=%d size=%zu\n", label, rc,
                text_size);
        qw_pdf_free(text);
        qw_pdf_close(document);
        free(data);
        return 5;
    }
    qw_pdf_free(text);
    memset(&page, 0, sizeof(page));
    rc = qw_pdf_render_page(document, 0u, 72u, &page, &report);
    if (rc != QW_STATUS_OK || !page.data || page.width == 0u ||
        page.height == 0u) {
        fprintf(stderr, "%s: render rc=%d note=%s\n", label, rc, report.note);
        qw_pdf_page_free(&page);
        qw_pdf_close(document);
        free(data);
        return 6;
    }
    qw_pdf_page_free(&page);
    qw_pdf_close(document);
    free(data);
    return 0;
}

int main(void)
{
    const char *directory = getenv("QUIREWISE_PDF_CORPUS_DIR");
    char probe[1024];
    char basic[1024];
    char pdfa[1024];
    FILE *file;

    if (!directory || !directory[0]) directory = QUIREWISE_PDF_CORPUS_DIR;
    snprintf(probe, sizeof(probe), "%s/basic/lorem-ipsum.pdf", directory);
    file = fopen(probe, "rb");
    if (!file) {
        printf("PDF corpus skipped: %s not found\n", directory);
        return 0;
    }
    fclose(file);
    snprintf(basic, sizeof(basic), "%s/basic/lorem-ipsum.pdf", directory);
    snprintf(pdfa, sizeof(pdfa), "%s/pdfa/lorem-ipsum-pdfa.pdf", directory);
    CHECK(check_document("corpus/basic", basic, 2u) == 0);
    CHECK(check_document("corpus/pdfa", pdfa, 1u) == 0);
    printf("PDF corpus: 2 documents passed\n");
    return 0;
}
