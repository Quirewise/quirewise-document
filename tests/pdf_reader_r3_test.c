/*
 * Regression: Standard security handler V2/R3 (128-bit RC4).
 *
 * The /U verification used the file key for all twenty RC4 rounds instead of
 * key XOR round index (ISO 32000-1 algorithm 5), so every compliant R3
 * document was rejected. The second fixture stores the page object inside an
 * encrypted /Type /ObjStm stream, which also pins the ordering rule: the
 * security handler must be ready before an object stream is decrypted and
 * inflated.
 *
 * Both fixtures are produced by tests/data/make_encrypted_r3.py, which
 * implements the algorithms independently from the reader.
 */

#include "document_backend.h"

#include <quirewise/document.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int check_document(const char *label, const char *path,
                          const char *expected_text)
{
    uint8_t *data = NULL;
    size_t size = 0u;
    QwPdfReader *reader = NULL;
    size_t pages = 0u;
    QwConvertReport report;
    uint8_t *text = NULL;
    size_t text_size = 0u;
    int rc;

    if (!read_file(path, &data, &size)) {
        fprintf(stderr, "%s: could not read %s\n", label, path);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    rc = qw_pdf_reader_open(data, size, &reader, &pages, &report);
    if (rc != 0 || !reader || pages != 1u) {
        fprintf(stderr, "%s: open rc=%d pages=%zu note=%s\n", label, rc, pages,
                report.note);
        free(data);
        if (reader) qw_pdf_reader_close(reader);
        return 2;
    }
    rc = qw_pdf_reader_extract_text(reader, &text, &text_size, &report);
    qw_pdf_reader_close(reader);
    free(data);
    if (rc != 0 || !text) {
        fprintf(stderr, "%s: extract rc=%d note=%s\n", label, rc, report.note);
        free(text);
        return 3;
    }
    if (!contains((const char *)text, text_size, expected_text)) {
        fprintf(stderr, "%s: decrypted text missing (%.*s)\n", label,
                (int)(text_size > 120u ? 120u : text_size), text);
        free(text);
        return 4;
    }
    free(text);
    return 0;
}

int main(void)
{
    int rc = check_document("r3", QUIREWISE_R3_PDF, "R3 encrypted hello");
    if (rc != 0) return rc;
    rc = check_document("r3 objstm", QUIREWISE_R3_OBJSTM_PDF,
                        "R3 objstm hello");
    return rc;
}
