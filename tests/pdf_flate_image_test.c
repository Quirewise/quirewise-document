/*
 * Regression: raw FlateDecode image XObjects with predictors.
 *
 * PDF images are usually raw samples declared with /Width, /Height,
 * /ColorSpace and /BitsPerComponent, compressed with FlateDecode and
 * optionally filtered with a PNG (/Predictor 10-15) or TIFF (/Predictor 2)
 * predictor. Such images are not self-describing files, so the image-package
 * decoder cannot sniff them; the rasteriser has to build the pixels itself.
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

enum { IMAGE_PLAIN = 0, IMAGE_PNG_UP = 12, IMAGE_TIFF = 2 };

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

/* Encode 4 rows x 12 bytes of solid red with the requested predictor. */
static size_t encode_samples(int mode, uint8_t *out, size_t capacity)
{
    uint8_t samples[4u * 12u];
    size_t row;
    size_t column;
    size_t position = 0u;
    for (row = 0u; row < 4u; ++row) {
        for (column = 0u; column < 4u; ++column) {
            samples[row * 12u + column * 3u + 0u] = 0xffu;
            samples[row * 12u + column * 3u + 1u] = 0x00u;
            samples[row * 12u + column * 3u + 2u] = 0x00u;
        }
    }
    if (mode == IMAGE_PNG_UP) {
        uint8_t previous[12] = {0};
        for (row = 0u; row < 4u; ++row) {
            out[position++] = 2u;
            for (column = 0u; column < 12u; ++column) {
                uint8_t value = samples[row * 12u + column];
                out[position++] = (uint8_t)(value - previous[column]);
                previous[column] = value;
            }
        }
        return position;
    }
    if (mode == IMAGE_TIFF) {
        uint8_t encoded[4u * 12u];
        memcpy(encoded, samples, sizeof(encoded));
        for (row = 0u; row < 4u; ++row) {
            for (column = 12u; column-- > 3u;) {
                encoded[row * 12u + column] =
                    (uint8_t)(encoded[row * 12u + column] -
                              encoded[row * 12u + column - 3u]);
            }
        }
        memcpy(out, encoded, sizeof(encoded));
        return sizeof(encoded);
    }
    memcpy(out, samples, sizeof(samples));
    return sizeof(samples);
}

static size_t build_pdf(uint8_t *out, size_t capacity, int mode)
{
    uint8_t samples[256];
    uint8_t compressed[300];
    size_t sample_size = encode_samples(mode, samples, sizeof(samples));
    size_t compressed_size = zlib_store(samples, sample_size, compressed,
                                        sizeof(compressed));
    static const char content[] = "q 100 0 0 100 0 0 cm /Im1 Do Q\n";
    size_t position = 0u;
    size_t offsets[6];
    size_t xref;
    int written;

    CHECK(compressed_size > 0u);
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
                       "[0 0 100 100] /Resources << /XObject << /Im1 5 0 R "
                       ">> >> /Contents 4 0 R >>\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[4] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "4 0 obj\n<< /Length %zu >>\nstream\n%s"
                       "endstream\nendobj\n",
                       strlen(content), content);
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[5] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "5 0 obj\n<< /Type /XObject /Subtype /Image /Width 4 "
                       "/Height 4 /ColorSpace /DeviceRGB "
                       "/BitsPerComponent 8 /Length %zu /Filter /FlateDecode",
                       compressed_size);
    if (written <= 0) return 0u;
    position += (size_t)written;
    if (mode == IMAGE_PNG_UP) {
        written = snprintf((char *)out + position, capacity - position,
                           " /DecodeParms << /Predictor 12 /Columns 12 >>");
        if (written <= 0) return 0u;
        position += (size_t)written;
    } else if (mode == IMAGE_TIFF) {
        written = snprintf((char *)out + position, capacity - position,
                           " /DecodeParms << /Predictor 2 /Columns 12 >>");
        if (written <= 0) return 0u;
        position += (size_t)written;
    }
    written = snprintf((char *)out + position, capacity - position,
                       " >>\nstream\n");
    if (written <= 0) return 0u;
    position += (size_t)written;
    if (position + compressed_size + 64u >= capacity) return 0u;
    memcpy(out + position, compressed, compressed_size);
    position += compressed_size;
    written = snprintf((char *)out + position, capacity - position,
                       "\nendstream\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    xref = position;
    written = snprintf((char *)out + position, capacity - position,
                       "xref\n0 6\n0000000000 65535 f \n");
    if (written <= 0) return 0u;
    position += (size_t)written;
    {
        size_t index;
        for (index = 1u; index < 6u; ++index) {
            written = snprintf((char *)out + position, capacity - position,
                               "%010zu 00000 n \n", offsets[index]);
            if (written <= 0) return 0u;
            position += (size_t)written;
        }
    }
    written = snprintf((char *)out + position, capacity - position,
                       "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%zu\n"
                       "%%%%EOF\n",
                       xref);
    if (written <= 0) return 0u;
    position += (size_t)written;
    return position;
}

static int render_center_is_red(const uint8_t *pdf, size_t size,
                                const char *label)
{
    QwPdfReader *reader = NULL;
    size_t pages = 0u;
    QwConvertReport report;
    QwPageImage page;
    size_t offset;
    int rc;
    memset(&report, 0, sizeof(report));
    memset(&page, 0, sizeof(page));
    rc = qw_pdf_reader_open(pdf, size, &reader, &pages, &report);
    if (rc != 0 || !reader || pages != 1u) {
        fprintf(stderr, "%s: open rc=%d note=%s\n", label, rc, report.note);
        if (reader) qw_pdf_reader_close(reader);
        return 0;
    }
    rc = qw_pdf_reader_render_page(reader, 0u, 72u, &page, &report);
    qw_pdf_reader_close(reader);
    if (rc != 0 || page.width != 100u || page.height != 100u || !page.data) {
        fprintf(stderr, "%s: render rc=%d %ux%u\n", label, rc, page.width,
                page.height);
        free((void *)page.data);
        return 0;
    }
    offset = (50u * page.width + 50u) * 3u;
    if (page.data[offset] != 0xffu || page.data[offset + 1u] != 0x00u ||
        page.data[offset + 2u] != 0x00u) {
        fprintf(stderr, "%s: center pixel %02x%02x%02x\n", label,
                page.data[offset], page.data[offset + 1u],
                page.data[offset + 2u]);
        free((void *)page.data);
        return 0;
    }
    free((void *)page.data);
    return 1;
}

int main(void)
{
    static uint8_t pdf[8192];
    size_t size;

    size = build_pdf(pdf, sizeof(pdf), IMAGE_PLAIN);
    CHECK(size > 0u);
    CHECK(render_center_is_red(pdf, size, "plain-flate"));

    size = build_pdf(pdf, sizeof(pdf), IMAGE_PNG_UP);
    CHECK(size > 0u);
    CHECK(render_center_is_red(pdf, size, "png-up-predictor"));

    size = build_pdf(pdf, sizeof(pdf), IMAGE_TIFF);
    CHECK(size > 0u);
    CHECK(render_center_is_red(pdf, size, "tiff-predictor"));
    return 0;
}
