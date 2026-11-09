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

/* T.88 Annex E arithmetic decoder (the JBIG2 MQ coder). The register C holds
 * complemented bytes and each byte carries seven usable bits when it follows
 * 0xFF, so this is not the JPEG 2000 variant of the MQ coder. */
static const uint16_t jb2_arith_qe[47] = {
    0x5601u, 0x3401u, 0x1801u, 0x0ac1u, 0x0521u, 0x0221u, 0x5601u,
    0x5401u, 0x4801u, 0x3801u, 0x3001u, 0x2401u, 0x1c01u, 0x1601u,
    0x5601u, 0x5401u, 0x5101u, 0x4801u, 0x3801u, 0x3401u, 0x3001u,
    0x2801u, 0x2401u, 0x2201u, 0x1c01u, 0x1801u, 0x1601u, 0x1401u,
    0x1201u, 0x1101u, 0x0ac1u, 0x09c1u, 0x08a1u, 0x0521u, 0x0441u,
    0x02a1u, 0x0221u, 0x0141u, 0x0111u, 0x0085u, 0x0049u, 0x0025u,
    0x0015u, 0x0009u, 0x0005u, 0x0001u, 0x5601u
};

static const uint8_t jb2_arith_nmps[47] = {
    1u, 2u, 3u, 4u, 5u, 38u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 29u, 15u,
    16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u,
    29u, 30u, 31u, 32u, 33u, 34u, 35u, 36u, 37u, 38u, 39u, 40u, 41u,
    42u, 43u, 44u, 45u, 45u, 46u
};

static const uint8_t jb2_arith_nlps[47] = {
    1u, 6u, 9u, 12u, 29u, 33u, 6u, 14u, 14u, 14u, 17u, 18u, 20u, 21u,
    14u, 14u, 15u, 16u, 17u, 18u, 19u, 19u, 20u, 21u, 22u, 23u, 24u,
    25u, 26u, 27u, 28u, 29u, 30u, 31u, 32u, 33u, 34u, 35u, 36u, 37u,
    38u, 39u, 40u, 41u, 42u, 43u, 46u
};

static const uint8_t jb2_arith_switch[47] = {
    1u, 0u, 0u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
};

typedef struct Jb2Arith {
    const uint8_t *data;
    size_t size;
    size_t position;
    uint32_t code;
    uint16_t interval;
    int bits_left;
    int previous;
} Jb2Arith;

static int jb2_arith_read_byte(Jb2Arith *arith)
{
    if (arith->position >= arith->size) return -1;
    return arith->data[arith->position++];
}

static void jb2_arith_byte_in(Jb2Arith *arith)
{
    int next;
    if (arith->previous == 0xFF) {
        next = (arith->position < arith->size) ? arith->data[arith->position] : -1;
        if (next < 0 || next > 0x8F) {
            /* A terminating marker: keep feeding ones without consuming it. */
            arith->bits_left = 8;
            return;
        }
        arith->position++;
        arith->code += 0xFE00u - ((uint32_t)next << 9);
        arith->bits_left = 7;
        arith->previous = next;
        return;
    }
    next = jb2_arith_read_byte(arith);
    if (next < 0) {
        arith->code += 0xFF00u;
        arith->bits_left = 8;
        arith->previous = 0xFF;
        return;
    }
    arith->code += 0xFF00u - ((uint32_t)next << 8);
    arith->bits_left = 8;
    arith->previous = next;
}

static void jb2_arith_renorm(Jb2Arith *arith)
{
    do {
        if (arith->bits_left == 0) jb2_arith_byte_in(arith);
        arith->interval = (uint16_t)(arith->interval << 1);
        arith->code <<= 1;
        arith->bits_left--;
    } while ((arith->interval & 0x8000u) == 0u);
}

static int jb2_arith_decode(Jb2Arith *arith, uint8_t *context)
{
    unsigned index = (unsigned)(*context & 0x7Fu);
    unsigned mps = (unsigned)(*context >> 7);
    uint16_t qe = jb2_arith_qe[index];
    unsigned decision;
    arith->interval = (uint16_t)(arith->interval - qe);
    if ((arith->code >> 16) < arith->interval) {
        if (arith->interval & 0x8000u) return (int)mps;
        if (arith->interval < qe) {
            decision = 1u - mps;
            if (jb2_arith_switch[index]) mps ^= 1u;
            index = jb2_arith_nlps[index];
        } else {
            decision = mps;
            index = jb2_arith_nmps[index];
        }
        jb2_arith_renorm(arith);
        *context = (uint8_t)(index | (mps << 7));
        return (int)decision;
    }
    arith->code -= (uint32_t)arith->interval << 16;
    if (arith->interval < qe) {
        arith->interval = qe;
        decision = mps;
        index = jb2_arith_nmps[index];
    } else {
        arith->interval = qe;
        decision = 1u - mps;
        if (jb2_arith_switch[index]) mps ^= 1u;
        index = jb2_arith_nlps[index];
    }
    jb2_arith_renorm(arith);
    *context = (uint8_t)(index | (mps << 7));
    return (int)decision;
}

/* Decode a generic region whose pixels are arithmetic coded with template 0. */
static int jb2_decode_arithmetic_region(const uint8_t *data, size_t size,
                                        uint32_t width, uint32_t height,
                                        Jb2Bitmap *out)
{
    Jb2Arith arith;
    uint8_t *contexts;
    int at[4][2];
    int index;
    uint32_t y;
    uint32_t x;
    int first;
    if (!jb2_bitmap_init(out, width, height, 0)) return 0;
    contexts = (uint8_t *)calloc(65536u, 1u);
    if (!contexts) {
        jb2_bitmap_free(out);
        return 0;
    }
    for (index = 0; index < 4; ++index) {
        at[index][0] = (int)(int8_t)data[18u + (unsigned)index * 2u];
        at[index][1] = (int)(int8_t)data[19u + (unsigned)index * 2u];
    }
    memset(&arith, 0, sizeof(arith));
    arith.data = data + 26u;
    arith.size = size - 26u;
    first = (arith.size > 0u) ? arith.data[0] : 0;
    arith.previous = first;
    arith.position = 1u;
    arith.interval = 0x8000u;
    arith.code = ((uint32_t)(~first) & 0xFFu) << 16;
    jb2_arith_byte_in(&arith);
    arith.code <<= 7;
    arith.bits_left -= 7;
    arith.interval = 0x8000u;

    for (y = 0u; y < height; ++y) {
        unsigned out_byte = 0u;
        uint32_t previous_row = 0u;
        uint32_t previous_row2 = 0u;
        size_t row_index = 0u;
        size_t row_index2 = 0u;
        if (y >= 1u) {
            previous_row = (uint32_t)out->bits[(size_t)(y - 1u) * out->stride] << 8;
            if (out->stride > 1u) {
                previous_row |= out->bits[(size_t)(y - 1u) * out->stride + 1u];
            }
            row_index = 2u;
        }
        if (y >= 2u) {
            previous_row2 = ((uint32_t)out->bits[(size_t)(y - 2u) * out->stride] << 8) |
                            (uint32_t)((out->stride > 1u) ? out->bits[(size_t)(y - 2u) * out->stride + 1u] : 0u);
            row_index2 = 2u;
        }
        for (x = 0u; x < width; ++x) {
            unsigned context = out_byte & 0x0Fu;
            context |= (unsigned)jb2_get(out, (int32_t)x + at[0][0], (int32_t)y + at[0][1]) << 4;
            context |= (previous_row >> 8) & 0x03E0u;
            context |= (unsigned)jb2_get(out, (int32_t)x + at[1][0], (int32_t)y + at[1][1]) << 10;
            context |= (unsigned)jb2_get(out, (int32_t)x + at[2][0], (int32_t)y + at[2][1]) << 11;
            context |= (previous_row2 >> 2) & 0x7000u;
            context |= (unsigned)jb2_get(out, (int32_t)x + at[3][0], (int32_t)y + at[3][1]) << 15;
            {
                int bit = jb2_arith_decode(&arith, &contexts[context]);
                if (bit) jb2_set(out, x, y, 1);
                previous_row <<= 1;
                previous_row2 <<= 1;
                out_byte = ((out_byte << 1) | (unsigned)bit) & 0xFFu;
            }
            if ((x & 7u) == 7u) {
                if (y >= 1u && x + 9u < width) {
                    if (row_index < out->stride) {
                        previous_row |= out->bits[(size_t)(y - 1u) * out->stride + row_index];
                    }
                    row_index++;
                    if (y >= 2u && row_index2 < out->stride) {
                        previous_row2 |= out->bits[(size_t)(y - 2u) * out->stride + row_index2];
                    }
                    row_index2++;
                }
            }
        }
    }
    free(contexts);
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
        if (size < 26u) return 0;
        if (template_id != 0) return QW_STATUS_ERR_UNSUPPORTED;
        /* Typical prediction would skip decoded rows; ignoring it silently
         * corrupts the region, so refuse it instead. */
        if (flags & 0x08u) return QW_STATUS_ERR_UNSUPPORTED;
        if (!jb2_decode_arithmetic_region(data, size, width, height, out)) return 0;
        return 1;
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
    /* Some producers embed the standalone file header; the segment stream
     * starts after it (D.4). */
    if (size >= 9u && data[0] == 0x97u && data[1] == 0x4Au && data[2] == 0x42u &&
        data[3] == 0x32u && data[4] == 0x0Du && data[5] == 0x0Au && data[6] == 0x1Au &&
        data[7] == 0x0Au) {
        size_t header = (data[8] & 0x02u) ? 9u : 13u;
        if (size < header) return 0;
        data += header;
        size -= header;
    }

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
        if (type == 36u) continue; /* intermediate generic region: never composed */
        if (type == 52u || type == 53u || type == 62u || type == 54u) {
            /* Profiles, custom tables, comments and colour palettes do not
             * change the page bitmap this decoder produces. */
            continue;
        }
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
    /* The PDF dictionary's /Width and /Height only hint at the page size; the
     * page information segment is authoritative. */
    (void)width_hint;
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
