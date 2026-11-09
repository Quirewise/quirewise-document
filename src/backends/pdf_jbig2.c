/* JBIG2 decoding for PDF image XObjects.
 *
 * PDF carries JBIG2 as a segment stream: page information, region segments and
 * end markers. This decoder implements the path scanned documents use:
 *
 *  - generic regions coded with MMR (ITU-T T.6) or with the T.88 arithmetic
 *    coder, generic templates 0-3;
 *  - symbol dictionaries that build glyphs from arithmetic-coded generic
 *    bitmaps and export a subset of them;
 *  - text regions that place those glyphs on a strip.
 *
 * Huffman-coded dictionaries, refinement, halftones and pattern dictionaries
 * report QW_STATUS_ERR_UNSUPPORTED rather than producing a partial page. */

#include "document_backend.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define JB2_MAX_PIXELS ((size_t)100u * 1000u * 1000u)
#define JB2_MAX_DIMENSION 20000u
#define JB2_MAX_SEGMENTS 4096u
#define JB2_MAX_SYMBOLS 65536u
#define JB2_MAX_DICTIONARIES 64u
#define JB2_MAX_INSTANCES 1000000u

typedef struct Jb2Bitmap {
    uint8_t *bits; /* packed 1-bit rows, 1 = black */
    uint32_t width;
    uint32_t height;
    size_t stride;
} Jb2Bitmap;

typedef struct Jb2Symbols {
    Jb2Bitmap *glyphs;
    size_t count;
} Jb2Symbols;

typedef struct Jb2Dictionary {
    uint32_t segment;
    Jb2Symbols symbols;
} Jb2Dictionary;

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

/* Compose a region bitmap onto the page with a combination operator
 * (T.88 7.4.1: 0 OR, 1 AND, 2 XOR, 3 XNOR, 4 REPLACE). */
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

static void jb2_arith_init(Jb2Arith *arith, const uint8_t *data, size_t size)
{
    int first;
    memset(arith, 0, sizeof(*arith));
    arith->data = data;
    arith->size = size;
    first = (size > 0u) ? data[0] : 0;
    arith->previous = first;
    arith->position = 1u;
    arith->interval = 0x8000u;
    arith->code = ((uint32_t)(~first) & 0xFFu) << 16;
    jb2_arith_byte_in(arith);
    arith->code <<= 7;
    arith->bits_left -= 7;
    arith->interval = 0x8000u;
}

/* T.88 Annex A arithmetic integer decoder. Returns 0 for a value, 1 for the
 * out-of-band code that terminates a run, or -1 on a malformed stream. */
typedef struct Jb2IntCtx {
    uint8_t contexts[512];
} Jb2IntCtx;

static int jb2_arith_int_decode(Jb2Arith *arith, Jb2IntCtx *ctx, int32_t *value)
{
    int previous = 1;
    int s;
    int bit;
    int32_t result = 0;
    int n_tail;
    int offset;
    int index;
    s = jb2_arith_decode(arith, &ctx->contexts[previous]);
    previous = (previous << 1) | s;
    bit = jb2_arith_decode(arith, &ctx->contexts[previous]);
    previous = (previous << 1) | bit;
    if (bit) {
        bit = jb2_arith_decode(arith, &ctx->contexts[previous]);
        previous = (previous << 1) | bit;
        if (bit) {
            bit = jb2_arith_decode(arith, &ctx->contexts[previous]);
            previous = (previous << 1) | bit;
            if (bit) {
                bit = jb2_arith_decode(arith, &ctx->contexts[previous]);
                previous = (previous << 1) | bit;
                if (bit) {
                    bit = jb2_arith_decode(arith, &ctx->contexts[previous]);
                    previous = (previous << 1) | bit;
                    if (bit) {
                        n_tail = 32;
                        offset = 4436;
                    } else {
                        n_tail = 12;
                        offset = 340;
                    }
                } else {
                    n_tail = 8;
                    offset = 84;
                }
            } else {
                n_tail = 6;
                offset = 20;
            }
        } else {
            n_tail = 4;
            offset = 4;
        }
    } else {
        n_tail = 2;
        offset = 0;
    }
    for (index = 0; index < n_tail; ++index) {
        bit = jb2_arith_decode(arith, &ctx->contexts[previous]);
        previous = ((previous << 1) & 511) | (previous & 256) | bit;
        if (result > (INT32_MAX >> 1)) {
            result = INT32_MAX;
        } else {
            result = (result << 1) | bit;
        }
    }
    if (result > INT32_MAX - offset) {
        result = INT32_MAX;
    } else {
        result += offset;
    }
    *value = s ? -result : result;
    return (s && result == 0) ? 1 : 0;
}

/* T.88 Annex A symbol ID decoder: a fixed-width arithmetic integer. */
typedef struct Jb2IaidCtx {
    uint8_t *contexts;
    uint32_t length;
} Jb2IaidCtx;

static int jb2_iaid_decode(Jb2Arith *arith, Jb2IaidCtx *ctx, uint32_t *value)
{
    uint32_t previous = 1u;
    uint32_t index;
    for (index = 0u; index < ctx->length; ++index) {
        int bit = jb2_arith_decode(arith, &ctx->contexts[previous]);
        previous = (previous << 1) | (uint32_t)bit;
    }
    *value = previous - (1u << ctx->length);
    return 0;
}

/* Decode an arithmetic-coded generic region into `bitmap` with `template`.
 * `contexts` must hold 65536 (template 0), 8192 (template 1) or 1024
 * (templates 2 and 3) entries. */
static int jb2_decode_generic_bits(Jb2Arith *arith, uint8_t *contexts, unsigned template_id,
                                   const int at[4][2], uint32_t width, uint32_t height,
                                   Jb2Bitmap *bitmap)
{
    uint32_t y;
    uint32_t x;
    for (y = 0u; y < height; ++y) {
        unsigned out_byte = 0u;
        uint32_t previous_row = 0u;
        uint32_t previous_row2 = 0u;
        size_t row_index = 0u;
        size_t row_index2 = 0u;
        if (y >= 1u) {
            previous_row = (uint32_t)bitmap->bits[(size_t)(y - 1u) * bitmap->stride] << 8;
            if (bitmap->stride > 1u) {
                previous_row |= bitmap->bits[(size_t)(y - 1u) * bitmap->stride + 1u];
            }
            row_index = 2u;
        }
        if (y >= 2u) {
            previous_row2 = ((uint32_t)bitmap->bits[(size_t)(y - 2u) * bitmap->stride] << 8) |
                            (uint32_t)((bitmap->stride > 1u) ?
                                       bitmap->bits[(size_t)(y - 2u) * bitmap->stride + 1u] : 0u);
            row_index2 = 2u;
        }
        for (x = 0u; x < width; ++x) {
            unsigned context;
            int bit;
            switch (template_id) {
            case 0u:
                context = out_byte & 0x0Fu;
                context |= (unsigned)jb2_get(bitmap, (int32_t)x + at[0][0], (int32_t)y + at[0][1]) << 4;
                context |= (previous_row >> 8) & 0x03E0u;
                context |= (unsigned)jb2_get(bitmap, (int32_t)x + at[1][0], (int32_t)y + at[1][1]) << 10;
                context |= (unsigned)jb2_get(bitmap, (int32_t)x + at[2][0], (int32_t)y + at[2][1]) << 11;
                context |= (previous_row2 >> 2) & 0x7000u;
                context |= (unsigned)jb2_get(bitmap, (int32_t)x + at[3][0], (int32_t)y + at[3][1]) << 15;
                break;
            case 1u:
                context = out_byte & 0x07u;
                context |= (unsigned)jb2_get(bitmap, (int32_t)x + at[0][0], (int32_t)y + at[0][1]) << 3;
                context |= (previous_row >> 9) & 0x01F0u;
                context |= (previous_row2 >> 4) & 0x1E00u;
                break;
            case 2u:
                context = out_byte & 0x03u;
                context |= (unsigned)jb2_get(bitmap, (int32_t)x + at[0][0], (int32_t)y + at[0][1]) << 2;
                context |= (previous_row >> 11) & 0x0078u;
                context |= (previous_row2 >> 7) & 0x0380u;
                break;
            default:
                context = out_byte & 0x0Fu;
                context |= (unsigned)jb2_get(bitmap, (int32_t)x + at[0][0], (int32_t)y + at[0][1]) << 4;
                context |= (previous_row >> 9) & 0x03E0u;
                break;
            }
            bit = jb2_arith_decode(arith, &contexts[context]);
            if (bit) jb2_set(bitmap, x, y, 1);
            previous_row <<= 1;
            previous_row2 <<= 1;
            out_byte = ((out_byte << 1) | (unsigned)bit) & 0xFFu;
            if ((x & 7u) == 7u) {
                if (y >= 1u && x + 9u < width) {
                    if (row_index < bitmap->stride) {
                        previous_row |= bitmap->bits[(size_t)(y - 1u) * bitmap->stride + row_index];
                    }
                    row_index++;
                    if (y >= 2u && row_index2 < bitmap->stride) {
                        previous_row2 |= bitmap->bits[(size_t)(y - 2u) * bitmap->stride + row_index2];
                    }
                    row_index2++;
                }
            }
        }
    }
    return 1;
}

static size_t jb2_context_count(unsigned template_id)
{
    if (template_id == 0u) return 65536u;
    if (template_id == 1u) return 8192u;
    return 1024u;
}

/* Decode one arithmetic-coded generic region segment body (region information,
 * generic flags, adaptive template pixels, coded data). */
static int jb2_decode_arithmetic_region(const uint8_t *data, size_t size,
                                        uint32_t width, uint32_t height,
                                        Jb2Bitmap *out)
{
    Jb2Arith arith;
    uint8_t *contexts;
    int at[4][2];
    int index;
    unsigned template_id = 0u;
    unsigned flags;
    if (size < 18u) return 0;
    flags = data[17u];
    template_id = (flags >> 1) & 0x03u;
    for (index = 0; index < 4; ++index) {
        at[index][0] = 0;
        at[index][1] = 0;
    }
    if (template_id == 0u) {
        if (size < 26u) return 0;
        for (index = 0; index < 4; ++index) {
            at[index][0] = (int)(int8_t)data[18u + (unsigned)index * 2u];
            at[index][1] = (int)(int8_t)data[19u + (unsigned)index * 2u];
        }
    } else {
        if (size < 20u) return 0;
        at[0][0] = (int)(int8_t)data[18u];
        at[0][1] = (int)(int8_t)data[19u];
    }
    if (!jb2_bitmap_init(out, width, height, 0)) return 0;
    contexts = (uint8_t *)calloc(jb2_context_count(template_id), 1u);
    if (!contexts) {
        jb2_bitmap_free(out);
        return 0;
    }
    jb2_arith_init(&arith, data + (template_id == 0u ? 26u : 20u),
                   size - (template_id == 0u ? 26u : 20u));
    jb2_decode_generic_bits(&arith, contexts, template_id, at, width, height, out);
    free(contexts);
    return 1;
}

/* Decode one generic region segment body (MMR or arithmetic). */
static int jb2_decode_generic_region(const uint8_t *data, size_t size, Jb2Bitmap *out)
{
    uint32_t width;
    uint32_t height;
    unsigned flags;
    int mmr;
    size_t offset = 17u;
    if (!data || size < 18u || !out) return 0;
    width = jb2_read_u32(data);
    height = jb2_read_u32(data + 4u);
    if (!width || !height || width > JB2_MAX_DIMENSION || height > JB2_MAX_DIMENSION) {
        return 0;
    }
    flags = data[17u];
    mmr = (flags & 0x01u) != 0u;
    offset = 18u;
    if (!mmr) {
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

/* Decode a symbol dictionary segment body into its exported glyphs. */
static int jb2_decode_symbol_dict(const uint8_t *data, size_t size, Jb2Symbols *out)
{
    uint16_t flags;
    unsigned sd_huff;
    unsigned sd_refagg;
    unsigned template_id;
    unsigned sd_rtemplate;
    size_t offset;
    uint32_t export_count;
    uint32_t new_count;
    Jb2Bitmap *new_symbols = NULL;
    uint32_t decoded = 0u;
    Jb2Arith arith;
    uint8_t *contexts = NULL;
    Jb2IntCtx *iadh = NULL;
    Jb2IntCtx *iadw = NULL;
    Jb2IntCtx *iaex = NULL;
    int32_t height_class = 0;
    int status = 0;
    int at[4][2];
    int index;
    if (!data || size < 10u || !out) return 0;
    flags = (uint16_t)((data[0] << 8) | data[1]);
    sd_huff = flags & 0x01u;
    sd_refagg = (flags >> 1) & 0x01u;
    template_id = (flags >> 10) & 0x03u;
    sd_rtemplate = (flags >> 12) & 0x01u;
    if (sd_huff || sd_refagg || sd_rtemplate) return QW_STATUS_ERR_UNSUPPORTED;
    if (template_id > 3u) return QW_STATUS_ERR_UNSUPPORTED;
    offset = 2u;
    {
        size_t at_bytes = (template_id == 0u) ? 8u : 2u;
        if (size - offset < at_bytes) return 0;
        for (index = 0; index < 4; ++index) {
            at[index][0] = 0;
            at[index][1] = 0;
        }
        if (template_id == 0u) {
            for (index = 0; index < 4; ++index) {
                at[index][0] = (int)(int8_t)data[offset + (size_t)index * 2u];
                at[index][1] = (int)(int8_t)data[offset + (size_t)index * 2u + 1u];
            }
        } else {
            at[0][0] = (int)(int8_t)data[offset];
            at[0][1] = (int)(int8_t)data[offset + 1u];
        }
        offset += at_bytes;
    }
    if (size - offset < 8u) return 0;
    export_count = jb2_read_u32(data + offset);
    new_count = jb2_read_u32(data + offset + 4u);
    offset += 8u;
    if (new_count > JB2_MAX_SYMBOLS || export_count > JB2_MAX_SYMBOLS) return 0;
    new_symbols = (Jb2Bitmap *)calloc(new_count ? new_count : 1u, sizeof(Jb2Bitmap));
    contexts = (uint8_t *)calloc(jb2_context_count(template_id), 1u);
    iadh = (Jb2IntCtx *)calloc(1u, sizeof(Jb2IntCtx));
    iadw = (Jb2IntCtx *)calloc(1u, sizeof(Jb2IntCtx));
    iaex = (Jb2IntCtx *)calloc(1u, sizeof(Jb2IntCtx));
    if (!contexts || !iadh || !iadw || !iaex) goto cleanup;
    jb2_arith_init(&arith, data + offset, size - offset);
    while (decoded < new_count) {
        int32_t delta;
        int code = jb2_arith_int_decode(&arith, iadh, &delta);
        uint32_t symbol_width = 0u;
        if (code != 0) {
            status = 0;
            goto cleanup;
        }
        height_class += delta;
        if (height_class <= 0) {
            status = 0;
            goto cleanup;
        }
        for (;;) {
            int32_t dw;
            code = jb2_arith_int_decode(&arith, iadw, &dw);
            if (code == 1) break; /* end of height class */
            if (code != 0) {
                status = 0;
                goto cleanup;
            }
            if (dw < 0 && symbol_width < (uint32_t)(-dw)) {
                status = 0;
                goto cleanup;
            }
            symbol_width += (uint32_t)dw;
            if (!symbol_width || symbol_width > JB2_MAX_DIMENSION) {
                status = 0;
                goto cleanup;
            }
            if (decoded >= new_count) break;
            if (!jb2_bitmap_init(&new_symbols[decoded], symbol_width,
                                 (uint32_t)height_class, 0)) {
                status = 0;
                goto cleanup;
            }
            jb2_decode_generic_bits(&arith, contexts, template_id, at, symbol_width,
                                    (uint32_t)height_class, &new_symbols[decoded]);
            decoded++;
        }
    }
    /* Export flags are arithmetic-coded run lengths of alternately skipped and
     * exported symbols; see T.88 6.5.10. */
    out->glyphs = (Jb2Bitmap *)calloc(export_count ? export_count : 1u, sizeof(Jb2Bitmap));
    if (!out->glyphs) {
        status = 0;
        goto cleanup;
    }
    {
        uint32_t position = 0u;
        uint32_t exported = 0u;
        int is_export = 0;
        while (position < new_count && exported < export_count) {
            int32_t run;
            uint32_t step;
            int code = jb2_arith_int_decode(&arith, iaex, &run);
            if (code == 1) break;
            if (code != 0) {
                status = 0;
                goto cleanup;
            }
            if (run < 0) {
                step = (uint32_t)(-run);
                if (step > new_count - position) step = new_count - position;
                position += step;
            } else {
                uint32_t index2;
                if ((uint32_t)run > new_count - position) run = (int32_t)(new_count - position);
                for (index2 = 0u; index2 < (uint32_t)run; ++index2) {
                    if (is_export && exported < export_count) {
                        out->glyphs[exported].bits = new_symbols[position].bits;
                        out->glyphs[exported].width = new_symbols[position].width;
                        out->glyphs[exported].height = new_symbols[position].height;
                        out->glyphs[exported].stride = new_symbols[position].stride;
                        new_symbols[position].bits = NULL;
                        exported++;
                    }
                    position++;
                }
            }
            is_export = !is_export;
        }
        out->count = exported;
    }
    status = 1;

cleanup:
    if (new_symbols) {
        uint32_t index3;
        for (index3 = 0u; index3 < new_count; ++index3) {
            jb2_bitmap_free(&new_symbols[index3]);
        }
        free(new_symbols);
    }
    if (status != 1 && out->glyphs) {
        uint32_t index4;
        for (index4 = 0u; index4 < out->count; ++index4) {
            jb2_bitmap_free(&out->glyphs[index4]);
        }
        free(out->glyphs);
        out->glyphs = NULL;
        out->count = 0u;
    }
    free(contexts);
    free(iadh);
    free(iadw);
    free(iaex);
    return status;
}

/* Decode a text region segment body into a region bitmap. */
static int jb2_decode_text_region(const uint8_t *data, size_t size,
                                  const Jb2Symbols *const *dictionaries,
                                  size_t dictionary_count, Jb2Bitmap *out)
{
    uint32_t width;
    uint32_t height;
    uint16_t flags;
    unsigned sb_huff;
    unsigned sb_refine;
    unsigned strips_log;
    unsigned refcorner;
    unsigned transposed;
    unsigned combine_op;
    int32_t strip_offset;
    uint32_t instances;
    size_t offset;
    size_t symbol_count = 0u;
    size_t index;
    uint32_t code_length;
    Jb2Arith arith;
    Jb2IntCtx *iadt = NULL;
    Jb2IntCtx *iafs = NULL;
    Jb2IntCtx *iads = NULL;
    Jb2IntCtx *iait = NULL;
    Jb2IaidCtx iaid;
    int status = 0;
    int32_t strip_t;
    int32_t first_s = 0;
    int32_t cursor_s = 0;
    uint32_t placed = 0u;
    if (!data || size < 19u || !out) return 0;
    width = jb2_read_u32(data);
    height = jb2_read_u32(data + 4u);
    if (!width || !height || width > JB2_MAX_DIMENSION || height > JB2_MAX_DIMENSION) {
        return 0;
    }
    flags = (uint16_t)((data[17u] << 8) | data[18u]);
    sb_huff = flags & 0x01u;
    sb_refine = (flags >> 1) & 0x01u;
    strips_log = (flags >> 2) & 0x03u;
    refcorner = (flags >> 4) & 0x03u;
    transposed = (flags >> 6) & 0x01u;
    combine_op = (flags >> 7) & 0x03u;
    strip_offset = (int32_t)((flags >> 10) & 0x1Fu);
    if (strip_offset > 0x0F) strip_offset -= 0x20;
    if (sb_huff || sb_refine || transposed) return QW_STATUS_ERR_UNSUPPORTED;
    offset = 19u;
    if (size - offset < 4u) return 0;
    instances = jb2_read_u32(data + offset);
    offset += 4u;
    if (instances > JB2_MAX_INSTANCES) return 0;
    for (index = 0u; index < dictionary_count; ++index) {
        symbol_count += dictionaries[index]->count;
    }
    code_length = 0u;
    while ((1u << code_length) < symbol_count) ++code_length;
    if (code_length > 16u) return 0;
    if (!jb2_bitmap_init(out, width, height, 0)) return 0;
    iadt = (Jb2IntCtx *)calloc(1u, sizeof(Jb2IntCtx));
    iafs = (Jb2IntCtx *)calloc(1u, sizeof(Jb2IntCtx));
    iads = (Jb2IntCtx *)calloc(1u, sizeof(Jb2IntCtx));
    iait = (Jb2IntCtx *)calloc(1u, sizeof(Jb2IntCtx));
    memset(&iaid, 0, sizeof(iaid));
    iaid.length = code_length;
    iaid.contexts = (uint8_t *)calloc((size_t)1u << (code_length + 1u), 1u);
    if (!iadt || !iafs || !iads || !iait || !iaid.contexts) goto cleanup;
    jb2_arith_init(&arith, data + offset, size - offset);
    {
        int32_t strip_value;
        int code = jb2_arith_int_decode(&arith, iadt, &strip_value);
        if (code != 0) goto cleanup;
        strip_t = -strip_value * (int32_t)(1u << strips_log);
    }
    while (placed < instances) {
        int32_t delta_t;
        int code = jb2_arith_int_decode(&arith, iadt, &delta_t);
        int first_symbol = 1;
        if (code != 0) goto cleanup;
        strip_t += delta_t * (int32_t)(1u << strips_log);
        for (;;) {
            int32_t cur_t = 0;
            uint32_t symbol_id;
            const Jb2Bitmap *glyph = NULL;
            int32_t x;
            int32_t y;
            if (first_symbol) {
                int32_t dfs;
                code = jb2_arith_int_decode(&arith, iafs, &dfs);
                if (code != 0) goto cleanup;
                first_s += dfs;
                cursor_s = first_s;
                first_symbol = 0;
            } else {
                int32_t ids;
                code = jb2_arith_int_decode(&arith, iads, &ids);
                if (code == 1) break; /* end of strip */
                if (code != 0) goto cleanup;
                cursor_s += ids + strip_offset;
            }
            if (strips_log > 0u) {
                int32_t value;
                code = jb2_arith_int_decode(&arith, iait, &value);
                if (code != 0) goto cleanup;
                cur_t = value;
            }
            if (jb2_iaid_decode(&arith, &iaid, &symbol_id) != 0) goto cleanup;
            {
                size_t remaining = symbol_id;
                size_t dictionary_index = 0u;
                while (dictionary_index < dictionary_count &&
                       remaining >= dictionaries[dictionary_index]->count) {
                    remaining -= dictionaries[dictionary_index]->count;
                    dictionary_index++;
                }
                if (dictionary_index < dictionary_count &&
                    remaining < dictionaries[dictionary_index]->count) {
                    glyph = &dictionaries[dictionary_index]->glyphs[remaining];
                }
            }
            if (!glyph) {
                placed++;
                if (placed >= instances) break;
                continue;
            }
            /* T.88 6.4.10: the reference corner anchors the glyph; the strip
             * coordinate counts from the bottom of the region. */
            if (refcorner < 2u) {
                y = strip_t + cur_t - (int32_t)glyph->height + 1;
                x = (refcorner == 0u) ? cursor_s
                                      : cursor_s - (int32_t)glyph->width + 1;
            } else {
                y = strip_t + cur_t;
                x = (refcorner == 2u) ? cursor_s
                                      : cursor_s - (int32_t)glyph->width + 1;
            }
            jb2_compose(out, glyph, x, y, combine_op);
            if (refcorner < 2u) {
                cursor_s += (int32_t)glyph->width - 1;
            }
            placed++;
            if (placed >= instances) break;
        }
    }
    status = 1;

cleanup:
    free(iadt);
    free(iafs);
    free(iads);
    free(iait);
    free(iaid.contexts);
    if (status != 1) jb2_bitmap_free(out);
    return status;
}

/* Parse the header of the next segment. Returns 1 with the fields filled in,
 * 0 at end of data, or -1 on a malformed header. */
static int jb2_next_segment(const uint8_t *data, size_t size, size_t *offset,
                            uint32_t *number, unsigned *type, uint32_t *page,
                            size_t *body_offset, size_t *body_size,
                            uint32_t *referred, size_t referred_capacity,
                            size_t *referred_count)
{
    size_t position = *offset;
    uint8_t flags;
    uint32_t count = 0u;
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
            count = long_form & 0x1fffffffu;
            position += 3u;
            position += ((size_t)count + 1u) / 8u;
        } else {
            count = (uint32_t)(rtscarf >> 5);
        }
    }
    referred_size = (*number <= 256u) ? 1u : (*number <= 65536u) ? 2u : 4u;
    if (count > 0u && (size_t)count > size) return -1;
    for (index = 0u; index < count; ++index) {
        uint32_t value;
        if (size - position < referred_size) return -1;
        if (referred && index < referred_capacity) {
            if (referred_size == 1u) {
                value = data[position];
            } else if (referred_size == 2u) {
                value = ((uint32_t)data[position] << 8) | data[position + 1u];
            } else {
                value = jb2_read_u32(data + position);
            }
            referred[index] = value;
        }
        position += referred_size;
    }
    if (referred_count) {
        *referred_count = (count < referred_capacity) ? count : referred_capacity;
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
    Jb2Dictionary dictionaries[JB2_MAX_DICTIONARIES];
    size_t dictionary_count = 0u;
    size_t pass;
    (void)width_hint;
    (void)height_hint;

    if (!data || !size || !out_gray8) return 0;
    memset(&page, 0, sizeof(page));
    memset(dictionaries, 0, sizeof(dictionaries));
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

    /* Pass 0 reads the global segments (symbol dictionaries); pass 1 decodes
     * the page segments, which may refer to those dictionaries. */
    for (pass = 0u; pass < ((globals && globals_size) ? 2u : 1u); ++pass) {
        const uint8_t *stream;
        size_t stream_size;
        int globals_pass = (globals && globals_size && pass == 0u) ? 1 : 0;
        size_t position = 0u;
        if (globals && globals_size) {
            stream = globals_pass ? globals : data;
            stream_size = globals_pass ? globals_size : size;
        } else {
            stream = data;
            stream_size = size;
        }
        (void)page_number;
        for (;;) {
            uint32_t referred[JB2_MAX_DICTIONARIES];
            size_t referred_count = 0u;
            int next = jb2_next_segment(stream, stream_size, &position, &number, &type,
                                        &page_number, &body_offset, &body_size,
                                        referred, JB2_MAX_DICTIONARIES, &referred_count);
            const uint8_t *body = stream + body_offset;
            if (next == 0) break;
            if (next < 0) {
                status = 0;
                goto done;
            }
            if (globals_pass) {
                if (type == 0u && dictionary_count < JB2_MAX_DICTIONARIES) {
                    Jb2Symbols symbols;
                    memset(&symbols, 0, sizeof(symbols));
                    {
                        int decoded = jb2_decode_symbol_dict(body, body_size, &symbols);
                        if (decoded == QW_STATUS_ERR_UNSUPPORTED) {
                            status = QW_STATUS_ERR_UNSUPPORTED;
                            goto done;
                        }
                        if (decoded != 1) {
                            status = 0;
                            goto done;
                        }
                    }
                    dictionaries[dictionary_count].segment = number;
                    dictionaries[dictionary_count].symbols = symbols;
                    dictionary_count++;
                }
                continue;
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
            if (type == 52u || type == 53u || type == 54u || type == 62u) {
                continue; /* profiles, custom tables, palettes, comments */
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
                if (decoded != 1) { status = 0; goto done; }
                if (!jb2_compose(&page, &region, (int32_t)jb2_read_u32(body + 8u),
                                 (int32_t)jb2_read_u32(body + 12u), body[16u] & 0x07u)) {
                    jb2_bitmap_free(&region);
                    status = 0;
                    goto done;
                }
                jb2_bitmap_free(&region);
                continue;
            }
            if (type == 0u) { /* symbol dictionary without a page */
                if (dictionary_count < JB2_MAX_DICTIONARIES) {
                    Jb2Symbols symbols;
                    memset(&symbols, 0, sizeof(symbols));
                    {
                        int decoded = jb2_decode_symbol_dict(body, body_size, &symbols);
                        if (decoded == QW_STATUS_ERR_UNSUPPORTED) {
                            status = QW_STATUS_ERR_UNSUPPORTED;
                            goto done;
                        }
                        if (decoded != 1) {
                            status = 0;
                            goto done;
                        }
                    }
                    dictionaries[dictionary_count].segment = number;
                    dictionaries[dictionary_count].symbols = symbols;
                    dictionary_count++;
                }
                continue;
            }
            if (type == 4u || type == 6u || type == 7u) {
                Jb2Bitmap region;
                const Jb2Symbols *active[JB2_MAX_DICTIONARIES];
                size_t active_count = 0u;
                size_t index;
                int decoded;
                memset(&region, 0, sizeof(region));
                for (index = 0u; index < referred_count && active_count < JB2_MAX_DICTIONARIES; ++index) {
                    size_t dictionary_index;
                    for (dictionary_index = 0u; dictionary_index < dictionary_count; ++dictionary_index) {
                        if (dictionaries[dictionary_index].segment == referred[index]) {
                            active[active_count++] = &dictionaries[dictionary_index].symbols;
                            break;
                        }
                    }
                }
                if (!active_count) {
                    for (index = 0u; index < dictionary_count && active_count < JB2_MAX_DICTIONARIES; ++index) {
                        active[active_count++] = &dictionaries[index].symbols;
                    }
                }
                decoded = jb2_decode_text_region(body, body_size, active, active_count, &region);
                if (decoded == QW_STATUS_ERR_UNSUPPORTED) {
                    status = QW_STATUS_ERR_UNSUPPORTED;
                    goto done;
                }
                if (decoded != 1) { status = 0; goto done; }
                if (!jb2_compose(&page, &region, (int32_t)jb2_read_u32(body + 8u),
                                 (int32_t)jb2_read_u32(body + 12u), body[16u] & 0x07u)) {
                    jb2_bitmap_free(&region);
                    status = 0;
                    goto done;
                }
                jb2_bitmap_free(&region);
                continue;
            }
            /* Halftones, pattern dictionaries and refinement produce content
             * this decoder cannot compose. */
            status = QW_STATUS_ERR_UNSUPPORTED;
            goto done;
        }
    }

    if (!have_page) {
        status = 0;
        goto done;
    }
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
    {
        size_t index;
        for (index = 0u; index < dictionary_count; ++index) {
            size_t glyph;
            for (glyph = 0u; glyph < dictionaries[index].symbols.count; ++glyph) {
                jb2_bitmap_free(&dictionaries[index].symbols.glyphs[glyph]);
            }
            free(dictionaries[index].symbols.glyphs);
        }
    }
    jb2_bitmap_free(&page);
    return status;
}
