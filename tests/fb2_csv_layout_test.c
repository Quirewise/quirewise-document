#include <quirewise/convert.h>
#include <quirewise/document.h>

#include "../src/backends/mupdf_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int contains(const uint8_t *data, size_t size, const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t i;
    if (!needle_size || needle_size > size) return 0;
    for (i = 0u; i + needle_size <= size; ++i) {
        if (memcmp(data + i, needle, needle_size) == 0) return 1;
    }
    return 0;
}

static int convert_and_check(const char *text, const char *extension,
                             const char *needle, const char *note_needle,
                             const char *label)
{
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    QwConvertReport report;
    QwMuPdfReader *reader = NULL;
    size_t pages = 0u;
    uint8_t *extracted = NULL;
    size_t extracted_size = 0u;
    int rc;
    memset(&report, 0, sizeof(report));
    rc = qw_mupdf_convert_memory((const uint8_t *)text, strlen(text), extension,
                                 0, &pdf, &pdf_size, &report);
    if (rc != 0 || !pdf) {
        fprintf(stderr, "%s conversion failed: rc=%d note=%s\n", label, rc,
                report.note);
        free(pdf);
        return 1;
    }
    if (!strstr(report.note, note_needle)) {
        fprintf(stderr, "%s loss note missing: %s\n", label, report.note);
        free(pdf);
        return 2;
    }
    rc = qw_mupdf_pdf_reader_open(pdf, pdf_size, &reader, &pages, &report);
    if (rc != 0 || pages < 1u) {
        fprintf(stderr, "%s PDF does not open: rc=%d pages=%zu\n", label, rc,
                pages);
        qw_mupdf_pdf_reader_close(reader);
        free(pdf);
        return 3;
    }
    rc = qw_mupdf_pdf_reader_extract_text(reader, &extracted, &extracted_size,
                                          &report);
    qw_mupdf_pdf_reader_close(reader);
    if (rc != 0 || !extracted || !contains(extracted, extracted_size, needle)) {
        fprintf(stderr, "%s text missing %s: %.*s\n", label, needle,
                (int)(extracted ? extracted_size : 0u),
                extracted ? (const char *)extracted : "");
        free(extracted);
        free(pdf);
        return 4;
    }
    free(extracted);
    free(pdf);
    return 0;
}

int main(void)
{
    static const char fb2[] =
        "<?xml version=\"1.0\"?><FictionBook "
        "xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\"><body><section>"
        "<title><p>FB2 Title</p></title><p>FB2 layout text</p>"
        "</section></body></FictionBook>";
    static const char csv[] = "name,value\nalpha,1\nbeta,2\n";
    int rc;
    rc = convert_and_check(fb2, "fb2", "FB2 layout text", "FB2 layout", "FB2");
    if (rc) return rc;
    rc = convert_and_check(csv, "csv", "alpha", "CSV layout", "CSV");
    if (rc) return rc;
    puts("FB2 and CSV layout outputs render to PDF with text");
    return 0;
}
