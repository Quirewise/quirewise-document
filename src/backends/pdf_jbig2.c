/* JBIG2 decoding for PDF image XObjects.
 *
 * PDF carries JBIG2 as a segment stream: page information, region segments and
 * end markers. The decoder here supports the path that scanned documents use
 * for entire pages, generic regions coded with MMR (ITU-T T.6), and composes
 * them onto the page bitmap. Arithmetic-coded generic regions, symbol
 * dictionaries, text regions and halftones report QW_STATUS_ERR_UNSUPPORTED
 * rather than producing a partial page. */

#include "document_backend.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define JB2_MAX_PIXELS ((size_t)100u * 1000u * 1000u)
#define JB2_MAX_DIMENSION 20000u
#define JB2_MAX_SEGMENTS 4096u

typedef struct Jb2Bitmap {
    uint8_t *bits; /* packed 1-bit rows, 1 = black */
    uint32_t width;
    uint32_t height;
    size_t stride;
} Jb2Bitmap;

static uint32_t jb2_read_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void jb2_bitmap_free(Jb2Bitmap *bitmap)
{
    if (!bitmap) return;
    free(bitmap->bits);
    bitmap->bits = NULL;
    bitmap->width = 0u;
    bitmap->height = 0u;
    bitmap->stride = 0u;
}

static int jb2_bitmap_init(Jb2Bitmap *bitmap, uint32_t width, uint32_t height, int fill_black)
{
    size_t stride;
    size_t size;
    if (!bitmap || !width || !height || width > JB2_MAX_DIMENSION ||
        height > JB2_MAX_DIMENSION) {
        return 0;
    }
    if ((size_t)width > JB2_MAX_PIXELS / (size_t)height) return 0;
    stride = ((size_t)width + 7u) / 8u;
    if (stride > SIZE_MAX / (size_t)height) return 0;
    size = stride * (size_t)height;
    bitmap->bits = (uint8_t *)malloc(size);
    if (!bitmap->bits) return 0;
    memset(bitmap->bits, fill_black ? 0xff : 0x00, size);
    bitmap->width = width;
    bitmap->height = height;
    bitmap->stride = stride;
    return 1;
}

static int jb2_get(const Jb2Bitmap *bitmap, int32_t x, int32_t y)
{
    if (x < 0 || y < 0 || x >= (int32_t)bitmap->width || y >= (int32_t)bitmap->height) {
        return 0;
    }
    return (bitmap->bits[(size_t)y * bitmap->stride + ((size_t)x >> 3)] >>
            (7u - ((uint32_t)x & 7u))) & 1;
}

static void jb2_set(Jb2Bitmap *bitmap, uint32_t x, uint32_t y, int black)
{
    uint8_t *byte = &bitmap->bits[(size_t)y * bitmap->stride + (x >> 3)];
    uint8_t mask = (uint8_t)(0x80u >> (x & 7u));
    if (black) {
        *byte |= mask;
    } else {
        *byte &= (uint8_t)~mask;
    }
}

/* Compose a region bitmap onto the page with the segment's combination
 * operator (T.88 7.4.1: 0 OR, 1 AND, 2 XOR, 3 XNOR, 4 REPLACE). */
static int jb2_compose(Jb2Bitmap *page, const Jb2Bitmap *region, int32_t x0, int32_t y0,
                       unsigned op)
{
    uint32_t y;
    uint32_t x;
    if (!page || !region) return 0;
    if (op > 4u) return 0;
    for (y = 0u; y < region->height; ++y) {
        int32_t page_y = y0 + (int32_t)y;
        if (page_y < 0 || page_y >= (int32_t)page->height) continue;
        for (x = 0u; x < region->width; ++x) {
            int32_t page_x = x0 + (int32_t)x;
            int source;
            int target;
            if (page_x < 0 || page_x >= (int32_t)page->width) continue;
            source = jb2_get(region, (int32_t)x, (int32_t)y);
            target = jb2_get(page, page_x, page_y);
            switch (op) {
            case 0u: target = target | source; break;
            case 1u: target = target & source; break;
            case 2u: target = target ^ source; break;
            case 3u: target = (target ^ source) ^ 1; break;
            default: target = source; break;
            }
            if (target != jb2_get(page, page_x, page_y)) {
                jb2_set(page, (uint32_t)page_x, (uint32_t)page_y, target);
            }
        }
    }
    return 1;
}

/* Decode one generic region segment body. The body starts with the region
 * segment information, then the generic region flags, optional adaptive
 * template pixels and the coded data. */
static int jb2_decode_generic_region(const uint8_t *data, size_t size, Jb2Bitmap *out)
{
    uint32_t width;
    uint32_t height;
    unsigned flags;
    int mmr;
    int template_id;
    size_t offset = 17u;
    if (!data || size < 18u || !out) return 0;
    width = jb2_read_u32(data);
    height = jb2_read_u32(data + 4u);
    if (!width || !height || width > JB2_MAX_DIMENSION || height > JB2_MAX_DIMENSION) {
        return 0;
    }
    flags = data[17u];
    mmr = (flags & 0x01u) != 0u;
    template_id = (int)((flags >> 1) & 0x03u);
    (void)template_id;
    offset = 18u;
    if (!mmr) {
        /* Arithmetic coding is not implemented yet. */
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    /* MMR coding carries no adaptive template pixels; the region data is a
     * plain ITU-T T.6 stream with one row per region row. */
    {
        uint8_t *gray = NULL;
        uint32_t y;
        uint32_t x;
        if (!jb2_bitmap_init(out, width, height, 0)) return 0;
        /* black_is1 = 0 keeps the T.6 decoder's natural polarity: a zero sample
         * is a black run, which is exactly the JBIG2 bitmap convention. */
        if (!pdf_ccitt_decode_image(data + offset, size - offset, width, height, 0, &gray)) {
            jb2_bitmap_free(out);
            return 0;
        }
        for (y = 0u; y < height; ++y) {
            for (x = 0u; x < width; ++x) {
                if (gray[(size_t)y * width + x] == 0u) {
                    jb2_set(out, x, y, 1);
                }
            }
        }
        free(gray);
    }
    return 1;
}

/* Parse the header of the next segment. Returns 1 with the fields filled in,
 * 0 at end of data, or -1 on a malformed header. */
static int jb2_next_segment(const uint8_t *data, size_t size, size_t *offset,
                            uint32_t *number, unsigned *type, uint32_t *page,
                            size_t *body_offset, size_t *body_size)
{
    size_t position = *offset;
    uint8_t flags;
    uint32_t referred_count = 0u;
    uint32_t referred_size;
    size_t index;
    if (position >= size) return 0;
    if (size - position < 11u) return -1;
    *number = jb2_read_u32(data + position);
    position += 4u;
    flags = data[position];
    position += 1u;
    *type = flags & 0x3fu;
    {
        uint8_t rtscarf = data[position];
        position += 1u;
        if ((rtscarf & 0xe0u) == 0xe0u) {
            uint32_t long_form;
            if (size - position < 3u) return -1;
            long_form = jb2_read_u32(data + position - 1u);
            referred_count = long_form & 0x1fffffffu;
            position += 3u;
            position += ((size_t)referred_count + 1u) / 8u;
        } else {
            referred_count = (uint32_t)(rtscarf >> 5);
        }
    }
    referred_size = (*number <= 256u) ? 1u : (*number <= 65536u) ? 2u : 4u;
    if (referred_count > 0u && (size_t)referred_count > size) return -1;
    for (index = 0u; index < referred_count; ++index) {
        if (size - position < referred_size) return -1;
        position += referred_size;
    }
    if (flags & 0x40u) {
        if (size - position < 4u) return -1;
        *page = jb2_read_u32(data + position);
        position += 4u;
    } else {
        if (size - position < 1u) return -1;
        *page = data[position];
        position += 1u;
    }
    if (size - position < 4u) return -1;
    *body_size = (size_t)jb2_read_u32(data + position);
    position += 4u;
    if (*body_size > size - position) return -1;
    *body_offset = position;
    *offset = position + *body_size;
    return 1;
}

int pdf_jbig2_decode_image(const uint8_t *globals, size_t globals_size,
                           const uint8_t *data, size_t size,
                           uint32_t width_hint, uint32_t height_hint,
                           uint8_t **out_gray8)
{
    Jb2Bitmap page;
    size_t offset = 0u;
    uint32_t number;
    unsigned type;
    uint32_t page_number;
    size_t body_offset;
    size_t body_size;
    int have_page = 0;
    int status = 0;
    (void)globals;
    (void)globals_size;

    if (!data || !size || !out_gray8) return 0;
    memset(&page, 0, sizeof(page));

    for (;;) {
        int next = jb2_next_segment(data, size, &offset, &number, &type, &page_number,
                                    &body_offset, &body_size);
        const uint8_t *body = data + body_offset;
        if (next == 0) break;
        if (next < 0) {
            status = 0;
            goto done;
        }
        if (type == 48u) { /* page information */
            uint32_t width;
            uint32_t height;
            unsigned flags;
            if (body_size < 19u) { status = 0; goto done; }
            width = jb2_read_u32(body);
            height = jb2_read_u32(body + 4u);
            flags = body[16u];
            if (!width || !height) { status = 0; goto done; }
            jb2_bitmap_free(&page);
            if (!jb2_bitmap_init(&page, width, height, (flags & 0x04u) != 0u)) {
                status = 0;
                goto done;
            }
            have_page = 1;
            continue;
        }
        if (type == 49u || type == 50u || type == 51u) continue; /* end markers */
        if (!have_page) { status = 0; goto done; }
        if (type == 38u || type == 39u) {
            Jb2Bitmap region;
            int decoded;
            memset(&region, 0, sizeof(region));
            decoded = jb2_decode_generic_region(body, body_size, &region);
            if (decoded == QW_STATUS_ERR_UNSUPPORTED) {
                status = QW_STATUS_ERR_UNSUPPORTED;
                goto done;
            }
            if (!decoded) { status = 0; goto done; }
            if (!jb2_compose(&page, &region, (int32_t)jb2_read_u32(body + 8u),
                             (int32_t)jb2_read_u32(body + 12u), body[16u] & 0x07u)) {
                jb2_bitmap_free(&region);
                status = 0;
                goto done;
            }
            jb2_bitmap_free(&region);
            continue;
        }
        if (type == 36u) { /* intermediate generic region: not composed */
            status = QW_STATUS_ERR_UNSUPPORTED;
            goto done;
        }
        /* Symbol dictionaries, text regions, halftones and refinement produce
         * content this decoder cannot compose, so fail instead of returning a
         * partially drawn page. */
        status = QW_STATUS_ERR_UNSUPPORTED;
        goto done;
    }

    if (!have_page) {
        status = 0;
        goto done;
    }
    if (width_hint && page.width != width_hint && page.width == 0u) {
        status = 0;
        goto done;
    }
    (void)height_hint;
    {
        uint8_t *gray = (uint8_t *)malloc((size_t)page.width * (size_t)page.height);
        uint32_t y;
        uint32_t x;
        if (!gray) {
            status = 0;
            goto done;
        }
        for (y = 0u; y < page.height; ++y) {
            for (x = 0u; x < page.width; ++x) {
                gray[(size_t)y * page.width + x] = jb2_get(&page, (int32_t)x, (int32_t)y) ? 0u : 255u;
            }
        }
        *out_gray8 = gray;
        status = 1;
    }

done:
    jb2_bitmap_free(&page);
    return status;
}
