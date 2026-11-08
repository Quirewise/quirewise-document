/*
 * Regression: Standard security handler V4/R4 with /AESV2 crypt filters
 * (AES-128-CBC, per-object keys salted with "sAlT", PKCS#5 padding).
 *
 * The fixtures are produced by tests/data/make_encrypted_r3.py with the
 * `cryptography` package, independently from the reader. The object-stream
 * variant also checks that the AES decryptor reports the unpadded length
 * before the payload is inflated.
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
    int rc = check_document("aes", QUIREWISE_AES_PDF, "AES encrypted hello");
    if (rc != 0) return rc;
    rc = check_document("aes objstm", QUIREWISE_AES_OBJSTM_PDF,
                        "AES objstm hello");
    if (rc != 0) return rc;

    /* Corrupt one ciphertext byte of the encrypted object stream: AES output is
     * garbage, inflation fails and the document must be rejected instead of
     * being reported as a valid PDF with missing pages. */
    {
        uint8_t *data = NULL;
        size_t size = 0u;
        size_t position = 0u;
        QwPdfReader *reader = NULL;
        size_t pages = 0u;
        QwConvertReport report;
        int tampered = 0;
        if (!read_file(QUIREWISE_AES_OBJSTM_PDF, &data, &size)) return 1;
        while (position + 9u <= size) {
            if (memcmp(data + position, "20 0 obj", 8u) == 0) {
                size_t stream = position;
                while (stream + 7u <= size &&
                       memcmp(data + stream, "stream", 6u) != 0) {
                    ++stream;
                }
                if (stream + 24u < size) {
                    data[stream + 20u] ^= 0x5au;
                    tampered = 1;
                }
                break;
            }
            ++position;
        }
        if (!tampered) {
            free(data);
            return 1;
        }
        memset(&report, 0, sizeof(report));
        rc = qw_pdf_reader_open(data, size, &reader, &pages, &report);
        free(data);
        if (reader) qw_pdf_reader_close(reader);
        if (rc == 0) {
            fprintf(stderr, "tampered AES ObjStm was accepted\n");
            return 5;
        }
    }
    return 0;
}
