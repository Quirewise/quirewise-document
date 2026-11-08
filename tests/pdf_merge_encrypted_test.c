/*
 * Regression: merging an encrypted input must be rejected, not copied.
 *
 * The merger copies object dictionaries and stream payloads verbatim and
 * writes a trailer without /Encrypt. Accepting ciphertext therefore produced a
 * document that no reader could open while the API still reported success.
 */

#include "document_backend.h"

#include <quirewise/convert.h>
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

int main(void)
{
    uint8_t *data = NULL;
    size_t size = 0u;
    uint8_t *merged = NULL;
    size_t merged_size = 0u;
    size_t pages = 0u;
    QwConvertReport report;
    int rc;

    if (!read_file(QUIREWISE_ENCRYPTED_PDF, &data, &size)) {
        fprintf(stderr, "merge encrypted: could not read the fixture\n");
        return 1;
    }
    memset(&report, 0, sizeof(report));
    rc = qw_pdf_merge_documents((const uint8_t *const *)&data, &size, 1u,
                                &merged, &merged_size, &pages, &report);
    free(data);
    if (rc != QW_STATUS_ERR_UNSUPPORTED) {
        fprintf(stderr, "merge encrypted: rc=%d (expected unsupported)\n", rc);
        free(merged);
        return 2;
    }
    if (merged) {
        fprintf(stderr, "merge encrypted: produced output on rejection\n");
        free(merged);
        return 3;
    }
    if (!strstr(report.note, "Encrypted")) {
        fprintf(stderr, "merge encrypted: note does not explain rejection: %s\n",
                report.note);
        return 4;
    }
    return 0;
}
