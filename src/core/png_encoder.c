#include "png_encoder.h"

#include <quirewise/compression.h>
#include <quirewise/status.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define QW_PNG_MAX_RAW_BYTES ((size_t)64u * 1024u * 1024u)
#define QW_PNG_HEADER_BYTES (8u + 25u + 12u + 12u)

static void png_write_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void png_write_chunk(uint8_t *out, size_t *position, const char type[4],
                            const uint8_t *data, size_t size)
{
    size_t type_position;
    png_write_u32(out + *position, (uint32_t)size);
    *position += 4u;
    type_position = *position;
    memcpy(out + *position, type, 4u);
    *position += 4u;
    if (size) memcpy(out + *position, data, size);
    *position += size;
    png_write_u32(out + *position,
                  qw_compression_crc32(out + type_position, 4u + size));
    *position += 4u;
}

int qw_png_encode_page(const QwPageImage *page, uint8_t **out_data,
                       size_t *out_size)
{
    static const uint8_t signature[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au,
                                         0x1Au, 0x0Au};
    size_t channels;
    unsigned color_type;
    size_t row_bytes;
    size_t raw_size;
    size_t zlib_capacity;
    size_t zlib_size = 0u;
    size_t total;
    size_t position = 0u;
    size_t row;
    uint8_t *raw = NULL;
    uint8_t *zlib_data = NULL;
    uint8_t *png = NULL;
    uint8_t header[13];

    if (!out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    if (!page || !page->data || !page->width || !page->height) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (page->kind == QW_PAGE_IMAGE_RGB24) {
        channels = 3u;
        color_type = 2u;
    } else if (page->kind == QW_PAGE_IMAGE_GRAY8) {
        channels = 1u;
        color_type = 0u;
    } else {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if ((size_t)page->width > SIZE_MAX / channels) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    row_bytes = (size_t)page->width * channels;
    if (row_bytes == SIZE_MAX || (size_t)page->height > SIZE_MAX / (row_bytes + 1u)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    raw_size = (size_t)page->height * (row_bytes + 1u);
    if (raw_size > QW_PNG_MAX_RAW_BYTES) return QW_STATUS_ERR_RESOURCE_LIMIT;

    raw = (uint8_t *)malloc(raw_size);
    if (!raw) return QW_STATUS_ERR_OUT_OF_MEMORY;
    for (row = 0u; row < page->height; ++row) {
        uint8_t *target = raw + row * (row_bytes + 1u);
        target[0] = 0u; /* filter type None */
        memcpy(target + 1u, page->data + row * row_bytes, row_bytes);
    }

    zlib_capacity = qw_compression_zlib_stored_bound(raw_size);
    if (zlib_capacity == SIZE_MAX ||
        zlib_capacity > raw_size + raw_size / 1024u + 1024u) {
        free(raw);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    zlib_data = (uint8_t *)malloc(zlib_capacity);
    if (!zlib_data) {
        free(raw);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    if (!qw_compression_deflate_zlib_stored(raw, raw_size, zlib_data,
                                            zlib_capacity, &zlib_size)) {
        free(raw);
        free(zlib_data);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    free(raw);

    if (zlib_size > SIZE_MAX - QW_PNG_HEADER_BYTES) {
        free(zlib_data);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    total = QW_PNG_HEADER_BYTES + zlib_size;
    png = (uint8_t *)malloc(total);
    if (!png) {
        free(zlib_data);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    memcpy(png, signature, sizeof(signature));
    position = sizeof(signature);

    png_write_u32(header, page->width);
    png_write_u32(header + 4u, page->height);
    header[8] = 8u;          /* bit depth */
    header[9] = color_type;  /* 2 = truecolour, 0 = greyscale */
    header[10] = 0u;         /* compression */
    header[11] = 0u;         /* filter method */
    header[12] = 0u;         /* interlace: none */
    png_write_chunk(png, &position, "IHDR", header, sizeof(header));
    png_write_chunk(png, &position, "IDAT", zlib_data, zlib_size);
    png_write_chunk(png, &position, "IEND", NULL, 0u);
    free(zlib_data);

    *out_data = png;
    *out_size = position;
    return QW_STATUS_OK;
}
