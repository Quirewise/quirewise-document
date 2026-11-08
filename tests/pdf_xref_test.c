/*
 * Regression: object lookup must honour the cross-reference table.
 *
 * 1) Incremental update: object 4 is redefined in an appended revision. A
 *    first-occurrence text scan returns the stale body and extracts "OLD";
 *    the reader must use the newest xref section and extract "NEW".
 * 2) Cross-reference stream: a /Type /XRef stream with W [1 2 2] replaces the
 *    classic table. The reader must decode it (Flate) and resolve objects.
 */

#include "document_backend.h"
#include "pdf_incremental_fixture.h"

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

/* ------------------------------------------------------------------ */
/* 1. Incremental update                                               */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 2. Cross-reference stream                                           */
/* ------------------------------------------------------------------ */

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

static size_t build_xref_stream(uint8_t *out, size_t capacity)
{
    size_t position = 0u;
    size_t offsets[6];
    size_t off5;
    size_t xref_bytes = 6u * 5u;
    uint8_t table[30];
    uint8_t compressed[64];
    size_t compressed_size;
    int written;
    size_t index;

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
                       "4 0 obj\n<< /Length 37 >>\nstream\n"
                       "BT /F1 12 Tf 10 40 Td (STREAM) Tj ET\n"
                       "endstream\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[5] = position; /* font */
    written = snprintf((char *)out + position, capacity - position,
                       "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont "
                       "/Helvetica >>\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    off5 = position;
    /* Entry 0: free. Entries 1..4: in use. Entry 5: the xref stream itself. */
    memset(table, 0, sizeof(table));
    put_be(table + 0u * 5u, 1u, 0u);
    put_be(table + 0u * 5u + 1u, 2u, 0u);
    put_be(table + 0u * 5u + 3u, 2u, 65535u);
    for (index = 1u; index <= 4u; ++index) {
        put_be(table + index * 5u, 1u, 1u);
        put_be(table + index * 5u + 1u, 2u, offsets[index]);
        put_be(table + index * 5u + 3u, 2u, 0u);
    }
    put_be(table + 5u * 5u, 1u, 1u);
    put_be(table + 5u * 5u + 1u, 2u, off5);
    put_be(table + 5u * 5u + 3u, 2u, 0u);
    compressed_size = zlib_store(table, xref_bytes, compressed,
                                 sizeof(compressed));
    if (!compressed_size) return 0u;

    written = snprintf((char *)out + position, capacity - position,
                       "6 0 obj\n<< /Type /XRef /Size 7 /W [1 2 2] "
                       "/Index [0 6] /Root 1 0 R /Length %zu "
                       "/Filter /FlateDecode >>\nstream\n",
                       compressed_size);
    if (written <= 0) return 0u;
    position += (size_t)written;
    if (position + compressed_size + 64u >= capacity) return 0u;
    memcpy(out + position, compressed, compressed_size);
    position += compressed_size;
    written = snprintf((char *)out + position, capacity - position,
                       "\nendstream\nendobj\nstartxref\n%zu\n%%%%EOF\n", off5);
    if (written <= 0) return 0u;
    position += (size_t)written;
    return position;
}

static int extract_text(const uint8_t *pdf, size_t size, uint8_t **out_text,
                        size_t *out_size)
{
    QwPdfReader *reader = NULL;
    size_t pages = 0u;
    QwConvertReport report;
    int rc;
    memset(&report, 0, sizeof(report));
    rc = qw_pdf_reader_open(pdf, size, &reader, &pages, &report);
    if (rc != 0 || !reader) return rc;
    rc = qw_pdf_reader_extract_text(reader, out_text, out_size, &report);
    qw_pdf_reader_close(reader);
    return rc;
}

int main(void)
{
    static uint8_t pdf[8192];
    size_t size;
    uint8_t *text = NULL;
    size_t text_size = 0u;
    int rc;

    size = pdf_build_incremental(pdf, sizeof(pdf));
    CHECK(size > 0u);
    rc = extract_text(pdf, size, &text, &text_size);
    CHECK(rc == 0 && text && text_size > 0u);
    CHECK(contains((const char *)text, text_size, "NEW"));
    CHECK(!contains((const char *)text, text_size, "OLD"));
    free(text);

    text = NULL;
    text_size = 0u;
    size = build_xref_stream(pdf, sizeof(pdf));
    CHECK(size > 0u);
    rc = extract_text(pdf, size, &text, &text_size);
    CHECK(rc == 0 && text && text_size > 0u);
    CHECK(contains((const char *)text, text_size, "STREAM"));
    free(text);
    return 0;
}
