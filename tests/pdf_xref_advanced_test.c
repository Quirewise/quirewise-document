/*
 * Regression: two xref paths that real producers use but the basic tests did
 * not cover.
 *
 * 1) Cross-reference stream compressed with a PNG Up predictor (/Predictor 12)
 *    and /Columns. Modern writers (Acrobat, pdfTeX) routinely emit this; the
 *    predictor decoder is otherwise untested.
 * 2) Hybrid-reference file: a classic xref table plus a trailer /XRefStm that
 *    supplies additional entries. The page content object only exists in the
 *    stream, so text extraction proves the entry was merged.
 */

#include "document_backend.h"

#include <quirewise/document.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

static uint32_t adler32_bytes(const uint8_t *data, size_t size)
{
    uint32_t low = 1u;
    uint32_t high = 0u;
    size_t index;
    for (index = 0u; index < size; ++index) {
        low = (low + data[index]) % 65521u;
        high = (high + low) % 65521u;
    }
    return (high << 16) | low;
}

static size_t zlib_store(const uint8_t *source, size_t size, uint8_t *dest,
                         size_t capacity)
{
    uint32_t checksum;
    uint16_t length = (uint16_t)size;
    size_t needed = size + 11u;
    if (size > 65535u || needed > capacity) return 0u;
    dest[0] = 0x78u;
    dest[1] = 0x01u;
    dest[2] = 0x01u;
    dest[3] = (uint8_t)(length & 0xffu);
    dest[4] = (uint8_t)((length >> 8) & 0xffu);
    dest[5] = (uint8_t)(~length & 0xffu);
    dest[6] = (uint8_t)((~length >> 8) & 0xffu);
    memcpy(dest + 7u, source, size);
    checksum = adler32_bytes(source, size);
    dest[7u + size] = (uint8_t)(checksum >> 24);
    dest[8u + size] = (uint8_t)(checksum >> 16);
    dest[9u + size] = (uint8_t)(checksum >> 8);
    dest[10u + size] = (uint8_t)checksum;
    return needed;
}

static void put_be(uint8_t *out, size_t width, size_t value)
{
    size_t index;
    for (index = 0u; index < width; ++index) {
        out[width - 1u - index] = (uint8_t)(value & 0xffu);
        value >>= 8;
    }
}

static int contains(const uint8_t *data, size_t size, const char *text)
{
    size_t length = strlen(text);
    size_t index;
    if (!length || size < length) return 0;
    for (index = 0u; index + length <= size; ++index) {
        if (memcmp(data + index, text, length) == 0) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 1. Xref stream with PNG Up predictor                                */
/* ------------------------------------------------------------------ */

static size_t build_predictor(uint8_t *out, size_t capacity)
{
    static const char content[] =
        "BT /F1 12 Tf 10 40 Td (PREDICTOR hello) Tj ET\n";
    size_t position = 0u;
    size_t offsets[6];
    size_t off6;
    uint8_t table[30];
    uint8_t encoded[36];
    uint8_t compressed[64];
    size_t compressed_size;
    size_t index;
    int written;

    written = snprintf((char *)out + position, capacity - position,
                       "%%PDF-1.5\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[1] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[2] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
                       "endobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[3] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox "
                       "[0 0 200 100] /Resources << /Font << /F1 5 0 R >> >> "
                       "/Contents 4 0 R >>\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[4] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "4 5 obj\n<< /Length %zu >>\nstream\n%s"
                       "endstream\nendobj\n",
                       strlen(content), content);
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[5] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont "
                       "/Helvetica >>\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    off6 = position;
    memset(table, 0, sizeof(table));
    put_be(table + 0u * 5u, 1u, 0u);
    put_be(table + 0u * 5u + 1u, 2u, 0u);
    put_be(table + 0u * 5u + 3u, 2u, 65535u);
    for (index = 1u; index <= 5u; ++index) {
        put_be(table + index * 5u, 1u, 1u);
        put_be(table + index * 5u + 1u, 2u, offsets[index]);
        put_be(table + index * 5u + 3u, 2u, index == 4u ? 5u : 0u);
    }
    /* Encode with the PNG Up filter: each row is preceded by filter byte 2
     * and holds (row - previous_row) mod 256. */
    {
        uint8_t previous[5] = {0, 0, 0, 0, 0};
        size_t row;
        for (row = 0u; row < 6u; ++row) {
            size_t column;
            encoded[row * 6u] = 2u;
            for (column = 0u; column < 5u; ++column) {
                uint8_t value = table[row * 5u + column];
                encoded[row * 6u + 1u + column] =
                    (uint8_t)(value - previous[column]);
                previous[column] = value;
            }
        }
    }
    compressed_size = zlib_store(encoded, sizeof(encoded), compressed,
                                 sizeof(compressed));
    if (!compressed_size) return 0u;
    written = snprintf((char *)out + position, capacity - position,
                       "6 0 obj\n<< /Type /XRef /Size 7 /W [1 2 2] "
                       "/Index [0 6] /Root 1 0 R /Length %zu "
                       "/Filter /FlateDecode "
                       "/DecodeParms << /Predictor 12 /Columns 5 >> >>\n"
                       "stream\n",
                       compressed_size);
    if (written <= 0) return 0u;
    position += (size_t)written;
    if (position + compressed_size + 64u >= capacity) return 0u;
    memcpy(out + position, compressed, compressed_size);
    position += compressed_size;
    written = snprintf((char *)out + position, capacity - position,
                       "\nendstream\nendobj\nstartxref\n%zu\n%%%%EOF\n", off6);
    if (written <= 0) return 0u;
    position += (size_t)written;
    return position;
}

/* ------------------------------------------------------------------ */
/* 2. Hybrid-reference file                                            */
/* ------------------------------------------------------------------ */

static size_t build_hybrid(uint8_t *out, size_t capacity)
{
    static const char content[] = "BT /F1 12 Tf 10 40 Td (Hybrid hello) Tj ET\n";
    size_t position = 0u;
    size_t offsets[7];
    size_t xref;
    uint8_t table[15];
    uint8_t compressed[64];
    size_t compressed_size;
    size_t off7;
    int written;

    written = snprintf((char *)out + position, capacity - position,
                       "%%PDF-1.4\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[1] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[2] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
                       "endobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[3] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox "
                       "[0 0 200 100] /Contents 6 0 R >>\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[4] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "4 0 obj\n<< >>\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[5] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "5 0 obj\n<< >>\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[6] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "6 5 obj\n<< /Length %zu >>\nstream\n%s"
                       "endstream\nendobj\n",
                       strlen(content), content);
    if (written <= 0) return 0u;
    position += (size_t)written;

    /* Stream entries for objects 4..6; 4 and 5 are unused, 6 holds content. */
    off7 = position;
    memset(table, 0, sizeof(table));
    put_be(table + 0u * 5u, 1u, 0u);
    put_be(table + 1u * 5u, 1u, 0u);
    put_be(table + 2u * 5u, 1u, 1u);
    put_be(table + 2u * 5u + 1u, 2u, offsets[6]);
    put_be(table + 2u * 5u + 3u, 2u, 5u);
    compressed_size = zlib_store(table, sizeof(table), compressed,
                                 sizeof(compressed));
    if (!compressed_size) return 0u;
    written = snprintf((char *)out + position, capacity - position,
                       "7 0 obj\n<< /Type /XRef /Size 8 /W [1 2 2] "
                       "/Index [4 3] /Root 1 0 R /Length %zu "
                       "/Filter /FlateDecode >>\nstream\n",
                       compressed_size);
    if (written <= 0) return 0u;
    position += (size_t)written;
    if (position + compressed_size + 8u >= capacity) return 0u;
    memcpy(out + position, compressed, compressed_size);
    position += compressed_size;
    written = snprintf((char *)out + position, capacity - position,
                       "\nendstream\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    xref = position;
    written = snprintf((char *)out + position, capacity - position,
                       "xref\n0 4\n0000000000 65535 f \n");
    if (written <= 0) return 0u;
    position += (size_t)written;
    {
        size_t index;
        for (index = 1u; index <= 3u; ++index) {
            written = snprintf((char *)out + position, capacity - position,
                               "%010zu 00000 n \n", offsets[index]);
            if (written <= 0) return 0u;
            position += (size_t)written;
        }
    }
    written = snprintf((char *)out + position, capacity - position,
                       "trailer\n<< /Size 8 /Root 1 0 R /XRefStm %zu >>\n"
                       "startxref\n%zu\n%%%%EOF\n",
                       off7, xref);
    if (written <= 0) return 0u;
    position += (size_t)written;
    return position;
}

static int check_text(const uint8_t *pdf, size_t size, const char *expected,
                      const char *label)
{
    QwPdfReader *reader = NULL;
    size_t pages = 0u;
    QwConvertReport report;
    uint8_t *text = NULL;
    size_t text_size = 0u;
    int rc;
    memset(&report, 0, sizeof(report));
    rc = qw_pdf_reader_open(pdf, size, &reader, &pages, &report);
    if (rc != 0 || !reader || pages != 1u) {
        fprintf(stderr, "%s: open rc=%d pages=%zu note=%s\n", label, rc, pages,
                report.note);
        if (reader) qw_pdf_reader_close(reader);
        return 0;
    }
    rc = qw_pdf_reader_extract_text(reader, &text, &text_size, &report);
    qw_pdf_reader_close(reader);
    if (rc != 0 || !text) {
        fprintf(stderr, "%s: extract rc=%d\n", label, rc);
        free(text);
        return 0;
    }
    if (!contains(text, text_size, expected)) {
        fprintf(stderr, "%s: missing '%s' (%.*s)\n", label, expected,
                (int)(text_size > 120u ? 120u : text_size), text);
        free(text);
        return 0;
    }
    free(text);
    return 1;
}

int main(void)
{
    static uint8_t pdf[8192];
    size_t size;

    size = build_predictor(pdf, sizeof(pdf));
    CHECK(size > 0u);
    CHECK(check_text(pdf, size, "PREDICTOR hello", "predictor-12"));

    size = build_hybrid(pdf, sizeof(pdf));
    CHECK(size > 0u);
    CHECK(check_text(pdf, size, "Hybrid hello", "hybrid-xrefstm"));
    return 0;
}
