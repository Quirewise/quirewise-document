#include <quirewise/convert.h>
#include <quirewise/document.h>

#include "../src/backends/document_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_ENCRYPTED_PDF
#define QUIREWISE_ENCRYPTED_PDF "tests/data/encrypted-rc4.pdf"
#endif

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *data;
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) { fclose(file); return NULL; }
    data = (uint8_t *)malloc((size_t)length);
    if (!data) { fclose(file); return NULL; }
    if (fread(data, 1u, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)length;
    return data;
}

int main(void)
{
    uint8_t *data = NULL;
    size_t size = 0u;
    QwPdfReader *reader = NULL;
    size_t pages = 0u;
    uint8_t *text = NULL;
    size_t text_size = 0u;
    QwConvertReport report;
    int rc;
    data = read_file(QUIREWISE_ENCRYPTED_PDF, &size);
    if (!data) {
        fprintf(stderr, "cannot read encrypted fixture: %s\n",
                QUIREWISE_ENCRYPTED_PDF);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    rc = qw_pdf_reader_open(data, size, &reader, &pages, &report);
    if (rc != 0 || !reader || pages != 1u) {
        fprintf(stderr, "encrypted PDF open failed: rc=%d pages=%zu note=%s\n",
                rc, pages, report.note);
        free(data);
        return 2;
    }
    rc = qw_pdf_reader_extract_text(reader, &text, &text_size, &report);
    if (rc != 0 || !text) {
        fprintf(stderr, "encrypted text extraction failed: rc=%d\n", rc);
        qw_pdf_reader_close(reader);
        free(data);
        return 3;
    }
    {
        size_t i;
        int found = 0;
        for (i = 0u; i + 15u <= text_size; ++i) {
            if (memcmp(text + i, "Encrypted hello", 15u) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "unexpected decrypted text: %.*s\n", (int)text_size,
                    text);
            free(text);
            qw_pdf_reader_close(reader);
            free(data);
            return 4;
        }
    }
    free(text);
    qw_pdf_reader_close(reader);
    /* Corrupt /U so the empty user password no longer verifies. */
    {
        uint8_t *bad = (uint8_t *)malloc(size);
        QwPdfReader *bad_reader = NULL;
        size_t bad_pages = 0u;
        if (!bad) { free(data); return 5; }
        memcpy(bad, data, size);
        {
            uint8_t *found = (uint8_t *)strstr((const char *)bad, "/U <");
            if (found && (size_t)(found - bad) + 10u < size)
                found[6] = (found[6] == '0') ? '1' : '0';
        }
        rc = qw_pdf_reader_open(bad, size, &bad_reader, &bad_pages,
                                      &report);
        if (rc == 0 || bad_reader) {
            fprintf(stderr, "corrupted key was accepted\n");
            qw_pdf_reader_close(bad_reader);
            free(bad);
            free(data);
            return 6;
        }
        free(bad);
    }
    free(data);
    puts("Encrypted RC4 PDF: empty user password verifies and text decrypts");
    return 0;
}
