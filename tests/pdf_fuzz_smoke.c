/* Bounded mutation smoke test for the internal PDF reader.
 *
 * Deterministic: the same seed and corpus produce the same mutations, so a
 * failure reproduces. The harness asserts only that every parsed document either
 * opens, extracts and closes cleanly, or is rejected with a defined status; any
 * memory error aborts under ASan/UBSan. */

#include "mupdf_backend.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INPUT (1u << 20)

static uint32_t rng_state = 0x9E3779B9u;

static uint32_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int load(const char *path, uint8_t **out_data, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *buffer;
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        (size_t)length > MAX_INPUT || fseek(file, 0, SEEK_SET) != 0) {
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

static void exercise(const uint8_t *data, size_t size)
{
    QwMuPdfReader *reader = NULL;
    size_t pages = 0u;
    int rc = qw_mupdf_pdf_reader_open(data, size, &reader, &pages, NULL);
    if (rc == QW_STATUS_OK) {
        uint8_t *text = NULL;
        size_t text_size = 0u;
        if (qw_mupdf_pdf_reader_extract_text(reader, &text, &text_size, NULL) ==
            QW_STATUS_OK) {
            free(text);
        }
        qw_mupdf_pdf_reader_close(reader);
    } else {
        /* Every rejection path must leave the reader unset. */
        if (reader != NULL) abort();
    }
}

int main(int argc, char **argv)
{
    int iterations = 400;
    int argument;
    size_t corpus_total = 0u;
    if (argc > 1) {
        int parsed = atoi(argv[1]);
        if (parsed > 0) iterations = parsed;
    }
    for (argument = 2; argument < argc; ++argument) {
        uint8_t *data = NULL;
        size_t size = 0u;
        int round;
        if (!load(argv[argument], &data, &size)) {
            fprintf(stderr, "skip unreadable corpus file: %s\n", argv[argument]);
            continue;
        }
        corpus_total += 1u;
        exercise(data, size);
        for (round = 0; round < iterations; ++round) {
            uint8_t *mutated = (uint8_t *)malloc(size ? size : 1u);
            size_t mutated_size = size;
            int changes = 1 + (int)(rng_next() % 8u);
            int change;
            if (!mutated) return 1;
            if (size) memcpy(mutated, data, size);
            for (change = 0; change < changes; ++change) {
                uint32_t operation = rng_next() % 4u;
                size_t position = size ? rng_next() % size : 0u;
                if (operation == 0u && size) {
                    mutated[position] ^= (uint8_t)(1u << (rng_next() % 8u));
                } else if (operation == 1u && size) {
                    mutated[position] = (uint8_t)rng_next();
                } else if (operation == 2u && size > 16u) {
                    mutated_size = size - (rng_next() % 16u);
                } else if (operation == 3u && size) {
                    size_t copy_length = size - position;
                    if (copy_length > 64u) copy_length = 64u;
                    memmove(mutated + position, mutated + position + copy_length,
                            size - position - copy_length);
                    mutated_size = size - copy_length;
                }
            }
            exercise(mutated, mutated_size);
            free(mutated);
        }
        free(data);
    }
    printf("pdf fuzz smoke: %zu corpus file(s) x %d mutation round(s), no crash\n",
           corpus_total, iterations);
    return corpus_total ? 0 : 2;
}
