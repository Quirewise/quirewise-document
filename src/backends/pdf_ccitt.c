/* ITU-T T.6 (CCITT Group 4) decoding for PDF image XObjects.
 *
 * The PDF writer emits bitonal pages with /CCITTFaxDecode, and a document
 * reader that cannot decode them renders those pages blank. The decoder here
 * works on the whole stream at once (no strip bookkeeping) and reports pixels
 * as GRAY8 so the caller can blit them through the regular image path.
 *
 * The run-length tables below are the standard ITU-T T.4 white/black code
 * tables; they are shared verbatim with the PDG package, which keeps its own
 * copy because the two packages do not depend on each other. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "document_backend.h"

typedef struct PdfCcittCode {
    uint16_t bits;
    uint8_t length;
    int16_t value; /* run length for T.4 tables */
} PdfCcittCode;

static const PdfCcittCode kPdfCcittWhiteCodes[] = {
    { 0x0007, 4, 2 },
    { 0x0008, 4, 3 },
    { 0x000b, 4, 4 },
    { 0x000c, 4, 5 },
    { 0x000e, 4, 6 },
    { 0x000f, 4, 7 },
    { 0x0007, 5, 10 },
    { 0x0008, 5, 11 },
    { 0x0012, 5, 128 },
    { 0x0013, 5, 8 },
    { 0x0014, 5, 9 },
    { 0x001b, 5, 64 },
    { 0x0003, 6, 13 },
    { 0x0007, 6, 1 },
    { 0x0008, 6, 12 },
    { 0x0017, 6, 192 },
    { 0x0018, 6, 1664 },
    { 0x002a, 6, 16 },
    { 0x002b, 6, 17 },
    { 0x0034, 6, 14 },
    { 0x0035, 6, 15 },
    { 0x0003, 7, 22 },
    { 0x0004, 7, 23 },
    { 0x0008, 7, 20 },
    { 0x000c, 7, 19 },
    { 0x0013, 7, 26 },
    { 0x0017, 7, 21 },
    { 0x0018, 7, 28 },
    { 0x0024, 7, 27 },
    { 0x0027, 7, 18 },
    { 0x0028, 7, 24 },
    { 0x002b, 7, 25 },
    { 0x0037, 7, 256 },
    { 0x0002, 8, 29 },
    { 0x0003, 8, 30 },
    { 0x0004, 8, 45 },
    { 0x0005, 8, 46 },
    { 0x000a, 8, 47 },
    { 0x000b, 8, 48 },
    { 0x0012, 8, 33 },
    { 0x0013, 8, 34 },
    { 0x0014, 8, 35 },
    { 0x0015, 8, 36 },
    { 0x0016, 8, 37 },
    { 0x0017, 8, 38 },
    { 0x001a, 8, 31 },
    { 0x001b, 8, 32 },
    { 0x0024, 8, 53 },
    { 0x0025, 8, 54 },
    { 0x0028, 8, 39 },
    { 0x0029, 8, 40 },
    { 0x002a, 8, 41 },
    { 0x002b, 8, 42 },
    { 0x002c, 8, 43 },
    { 0x002d, 8, 44 },
    { 0x0032, 8, 61 },
    { 0x0033, 8, 62 },
    { 0x0034, 8, 63 },
    { 0x0035, 8, 0 },
    { 0x0036, 8, 320 },
    { 0x0037, 8, 384 },
    { 0x004a, 8, 59 },
    { 0x004b, 8, 60 },
    { 0x0052, 8, 49 },
    { 0x0053, 8, 50 },
    { 0x0054, 8, 51 },
    { 0x0055, 8, 52 },
    { 0x0058, 8, 55 },
    { 0x0059, 8, 56 },
    { 0x005a, 8, 57 },
    { 0x005b, 8, 58 },
    { 0x0064, 8, 448 },
    { 0x0065, 8, 512 },
    { 0x0067, 8, 640 },
    { 0x0068, 8, 576 },
    { 0x0098, 9, 1472 },
    { 0x0099, 9, 1536 },
    { 0x009a, 9, 1600 },
    { 0x009b, 9, 1728 },
    { 0x00cc, 9, 704 },
    { 0x00cd, 9, 768 },
    { 0x00d2, 9, 832 },
    { 0x00d3, 9, 896 },
    { 0x00d4, 9, 960 },
    { 0x00d5, 9, 1024 },
    { 0x00d6, 9, 1088 },
    { 0x00d7, 9, 1152 },
    { 0x00d8, 9, 1216 },
    { 0x00d9, 9, 1280 },
    { 0x00da, 9, 1344 },
    { 0x00db, 9, 1408 },
    { 0x0008, 11, 1792 },
    { 0x000c, 11, 1856 },
    { 0x000d, 11, 1920 },
    { 0x0012, 12, 1984 },
    { 0x0013, 12, 2048 },
    { 0x0014, 12, 2112 },
    { 0x0015, 12, 2176 },
    { 0x0016, 12, 2240 },
    { 0x0017, 12, 2304 },
    { 0x001c, 12, 2368 },
    { 0x001d, 12, 2432 },
    { 0x001e, 12, 2496 },
    { 0x001f, 12, 2560 },
};

static const PdfCcittCode kPdfCcittBlackCodes[] = {
    { 0x0002, 2, 3 },
    { 0x0003, 2, 2 },
    { 0x0002, 3, 1 },
    { 0x0003, 3, 4 },
    { 0x0002, 4, 6 },
    { 0x0003, 4, 5 },
    { 0x0003, 5, 7 },
    { 0x0004, 6, 9 },
    { 0x0005, 6, 8 },
    { 0x0004, 7, 10 },
    { 0x0005, 7, 11 },
    { 0x0007, 7, 12 },
    { 0x0004, 8, 13 },
    { 0x0007, 8, 14 },
    { 0x0018, 9, 15 },
    { 0x0008, 10, 18 },
    { 0x000f, 10, 64 },
    { 0x0017, 10, 16 },
    { 0x0018, 10, 17 },
    { 0x0037, 10, 0 },
    { 0x0008, 11, 1792 },
    { 0x000c, 11, 1856 },
    { 0x000d, 11, 1920 },
    { 0x0017, 11, 24 },
    { 0x0018, 11, 25 },
    { 0x0028, 11, 23 },
    { 0x0037, 11, 22 },
    { 0x0067, 11, 19 },
    { 0x0068, 11, 20 },
    { 0x006c, 11, 21 },
    { 0x0012, 12, 1984 },
    { 0x0013, 12, 2048 },
    { 0x0014, 12, 2112 },
    { 0x0015, 12, 2176 },
    { 0x0016, 12, 2240 },
    { 0x0017, 12, 2304 },
    { 0x001c, 12, 2368 },
    { 0x001d, 12, 2432 },
    { 0x001e, 12, 2496 },
    { 0x001f, 12, 2560 },
    { 0x0024, 12, 52 },
    { 0x0027, 12, 55 },
    { 0x0028, 12, 56 },
    { 0x002b, 12, 59 },
    { 0x002c, 12, 60 },
    { 0x0033, 12, 320 },
    { 0x0034, 12, 384 },
    { 0x0035, 12, 448 },
    { 0x0037, 12, 53 },
    { 0x0038, 12, 54 },
    { 0x0052, 12, 50 },
    { 0x0053, 12, 51 },
    { 0x0054, 12, 44 },
    { 0x0055, 12, 45 },
    { 0x0056, 12, 46 },
    { 0x0057, 12, 47 },
    { 0x0058, 12, 57 },
    { 0x0059, 12, 58 },
    { 0x005a, 12, 61 },
    { 0x005b, 12, 256 },
    { 0x0064, 12, 48 },
    { 0x0065, 12, 49 },
    { 0x0066, 12, 62 },
    { 0x0067, 12, 63 },
    { 0x0068, 12, 30 },
    { 0x0069, 12, 31 },
    { 0x006a, 12, 32 },
    { 0x006b, 12, 33 },
    { 0x006c, 12, 40 },
    { 0x006d, 12, 41 },
    { 0x00c8, 12, 128 },
    { 0x00c9, 12, 192 },
    { 0x00ca, 12, 26 },
    { 0x00cb, 12, 27 },
    { 0x00cc, 12, 28 },
    { 0x00cd, 12, 29 },
    { 0x00d2, 12, 34 },
    { 0x00d3, 12, 35 },
    { 0x00d4, 12, 36 },
    { 0x00d5, 12, 37 },
    { 0x00d6, 12, 38 },
    { 0x00d7, 12, 39 },
    { 0x00da, 12, 42 },
    { 0x00db, 12, 43 },
    { 0x004a, 13, 640 },
    { 0x004b, 13, 704 },
    { 0x004c, 13, 768 },
    { 0x004d, 13, 832 },
    { 0x0052, 13, 1280 },
    { 0x0053, 13, 1344 },
    { 0x0054, 13, 1408 },
    { 0x0055, 13, 1472 },
    { 0x005a, 13, 1536 },
    { 0x005b, 13, 1600 },
    { 0x0064, 13, 1664 },
    { 0x0065, 13, 1728 },
    { 0x006c, 13, 512 },
    { 0x006d, 13, 576 },
    { 0x0072, 13, 896 },
    { 0x0073, 13, 960 },
    { 0x0074, 13, 1024 },
    { 0x0075, 13, 1088 },
    { 0x0076, 13, 1152 },
    { 0x0077, 13, 1216 },
};

typedef struct PdfCcittLookup {
    int16_t value;
    uint8_t length;
} PdfCcittLookup;

#define PDF_CCITT_LOOKUP_BITS 16u
#define PDF_CCITT_LOOKUP_SIZE (1u << PDF_CCITT_LOOKUP_BITS)

static int pdf_ccitt_build_lookup(PdfCcittLookup *lookup,
                                  const PdfCcittCode *codes,
                                  size_t count)
{
    size_t index;
    memset(lookup, 0, PDF_CCITT_LOOKUP_SIZE * sizeof(*lookup));
    for (index = 0u; index < count; ++index) {
        unsigned length = codes[index].length;
        unsigned fill;
        unsigned prefix;
        if (!length || length > PDF_CCITT_LOOKUP_BITS) {
            return 0;
        }
        prefix = (unsigned)codes[index].bits << (PDF_CCITT_LOOKUP_BITS - length);
        for (fill = 0u; fill < (1u << (PDF_CCITT_LOOKUP_BITS - length)); ++fill) {
            lookup[prefix | fill].value = codes[index].value;
            lookup[prefix | fill].length = (uint8_t)length;
        }
    }
    return 1;
}

typedef struct PdfCcittBits {
    const uint8_t *data;
    size_t size;
    size_t bit;
    int exhausted;
} PdfCcittBits;

static int pdf_ccitt_peek(PdfCcittBits *bits, unsigned count)
{
    unsigned index;
    int value = 0;
    for (index = 0u; index < count; ++index) {
        size_t position = bits->bit + index;
        int bit = 0;
        if (position / 8u < bits->size) {
            bit = (bits->data[position / 8u] >> (7u - (position % 8u))) & 1u;
        } else {
            bits->exhausted = 1;
        }
        value = (value << 1) | bit;
    }
    return value;
}

static int pdf_ccitt_read_run(PdfCcittBits *bits, const PdfCcittLookup *lookup,
                              int *run)
{
    int total = 0;
    int guard = 0;
    for (;;) {
        PdfCcittLookup entry = lookup[pdf_ccitt_peek(bits, PDF_CCITT_LOOKUP_BITS)];
        if (!entry.length || ++guard > 64) {
            return 0;
        }
        bits->bit += entry.length;
        total += entry.value;
        if (entry.value < 64) {
            *run = total;
            return 1;
        }
        if (total > 1000000) {
            return 0;
        }
    }
}

/* Returns the vertical delta for V0..V3/VL3, PDF_CCITT_HORIZONTAL for the
 * horizontal mode, PDF_CCITT_PASS for pass mode, or a negative value when the
 * stream ends (EOFB) or the code is invalid. */
#define PDF_CCITT_PASS (-100)
#define PDF_CCITT_HORIZONTAL (-101)
#define PDF_CCITT_END (-102)
#define PDF_CCITT_INVALID (-103)

static int pdf_ccitt_read_mode(PdfCcittBits *bits)
{
    if (!pdf_ccitt_peek(bits, 1)) {
        /* value 0 -> leading zero; fall through to the longer prefixes */
    }
    if ((pdf_ccitt_peek(bits, 1) & 1) != 0) {
        bits->bit += 1u;
        return 0;
    }
    if ((pdf_ccitt_peek(bits, 3) & 0x7) == 0x3) { /* 011 VR1 */
        bits->bit += 3u;
        return 1;
    }
    if ((pdf_ccitt_peek(bits, 3) & 0x7) == 0x2) { /* 010 VL1 */
        bits->bit += 3u;
        return -1;
    }
    if ((pdf_ccitt_peek(bits, 3) & 0x7) == 0x1) { /* 001 horizontal */
        bits->bit += 3u;
        return PDF_CCITT_HORIZONTAL;
    }
    if ((pdf_ccitt_peek(bits, 4) & 0xf) == 0x1) { /* 0001 pass */
        bits->bit += 4u;
        return PDF_CCITT_PASS;
    }
    if ((pdf_ccitt_peek(bits, 6) & 0x3f) == 0x3) { /* 000011 VR2 */
        bits->bit += 6u;
        return 2;
    }
    if ((pdf_ccitt_peek(bits, 6) & 0x3f) == 0x2) { /* 000010 VL2 */
        bits->bit += 6u;
        return -2;
    }
    if ((pdf_ccitt_peek(bits, 7) & 0x7f) == 0x3) { /* 0000011 VR3 */
        bits->bit += 7u;
        return 3;
    }
    if ((pdf_ccitt_peek(bits, 7) & 0x7f) == 0x2) { /* 0000010 VL3 */
        bits->bit += 7u;
        return -3;
    }
    {
        /* EOL (000000000001) and EOFB (two EOLs) terminate the data. */
        unsigned index;
        int all_zero = 1;
        for (index = 0u; index < 11u; ++index) {
            if (pdf_ccitt_peek(bits, index + 1u) & (1 << index)) {
                all_zero = 0;
            }
        }
        if (all_zero || bits->exhausted) {
            return PDF_CCITT_END;
        }
    }
    return PDF_CCITT_INVALID;
}

static int pdf_ccitt_decode(const uint8_t *data, size_t size, uint32_t width,
                            uint32_t height, uint8_t **out_gray8)
{
    PdfCcittLookup *white = NULL;
    PdfCcittLookup *black = NULL;
    int *reference = NULL;
    int *current = NULL;
    uint8_t *gray = NULL;
    size_t row_capacity;
    uint32_t y;
    int ok = 0;

    if (!data || !size || !width || !height || !out_gray8) {
        return 0;
    }
    if ((size_t)width > SIZE_MAX / (size_t)height) {
        return 0;
    }
    row_capacity = (size_t)width + 2u;
    white = (PdfCcittLookup *)malloc(PDF_CCITT_LOOKUP_SIZE * sizeof(*white));
    black = (PdfCcittLookup *)malloc(PDF_CCITT_LOOKUP_SIZE * sizeof(*black));
    reference = (int *)calloc(row_capacity, sizeof(*reference));
    current = (int *)calloc(row_capacity, sizeof(*current));
    gray = (uint8_t *)malloc((size_t)width * (size_t)height);
    if (!white || !black || !reference || !current || !gray) {
        goto done;
    }
    if (!pdf_ccitt_build_lookup(white, kPdfCcittWhiteCodes,
                                sizeof(kPdfCcittWhiteCodes) / sizeof(kPdfCcittWhiteCodes[0])) ||
        !pdf_ccitt_build_lookup(black, kPdfCcittBlackCodes,
                                sizeof(kPdfCcittBlackCodes) / sizeof(kPdfCcittBlackCodes[0]))) {
        goto done;
    }
    memset(gray, 255, (size_t)width * (size_t)height);

    {
        PdfCcittBits bits;
        size_t reference_count = 0u;
        memset(&bits, 0, sizeof(bits));
        bits.data = data;
        bits.size = size;

        for (y = 0u; y < height; ++y) {
            int a0 = -1;
            int color = 0; /* 0 = white, 1 = black */
            size_t current_count = 0u;
            int guard = 0;

            while (a0 < (int)width) {
                int mode;
                if (++guard > (int)width * 8 + 4096) {
                    goto done;
                }
                mode = pdf_ccitt_read_mode(&bits);
                if (mode == PDF_CCITT_END) {
                    goto rows_done;
                }
                if (mode == PDF_CCITT_INVALID) {
                    goto done;
                }
                if (mode == PDF_CCITT_PASS) {
                    size_t k = 0u;
                    int b2;
                    while (k < reference_count && reference[k] <= a0) ++k;
                    if (k < reference_count && ((k & 1u) ? 1 : 0) != color) ++k;
                    b2 = (k + 1u < reference_count) ? reference[k + 1u] : (int)width;
                    if (b2 <= a0) goto done;
                    a0 = b2;
                    continue;
                }
                if (mode == PDF_CCITT_HORIZONTAL) {
                    int base = (a0 < 0) ? 0 : a0;
                    int run1 = 0;
                    int run2 = 0;
                    int a1;
                    int a2;
                    if (!pdf_ccitt_read_run(&bits, color ? black : white, &run1)) goto done;
                    a1 = base + run1;
                    if (a1 > (int)width) a1 = (int)width;
                    if (current_count < row_capacity) current[current_count++] = a1;
                    if (!pdf_ccitt_read_run(&bits, color ? white : black, &run2)) goto done;
                    a2 = a1 + run2;
                    if (a2 > (int)width) a2 = (int)width;
                    if (current_count < row_capacity) current[current_count++] = a2;
                    if (a2 <= a0) goto done;
                    a0 = a2;
                    continue;
                }
                {
                    size_t k = 0u;
                    int b1;
                    int a1;
                    while (k < reference_count && reference[k] <= a0) ++k;
                    if (k < reference_count && ((k & 1u) ? 1 : 0) != color) ++k;
                    b1 = (k < reference_count) ? reference[k] : (int)width;
                    a1 = b1 + mode;
                    if (a1 < 0) a1 = 0;
                    if (a1 > (int)width) a1 = (int)width;
                    if (current_count < row_capacity) current[current_count++] = a1;
                    a0 = a1;
                    color = !color;
                }
            }

            /* Paint black runs: [current[0], current[1]), [current[2], ...). */
            {
                size_t index;
                uint8_t *row = gray + (size_t)y * (size_t)width;
                for (index = 0u; index + 1u < current_count; index += 2u) {
                    int start = current[index];
                    int end = current[index + 1u];
                    int x;
                    if (start < 0) start = 0;
                    if (end > (int)width) end = (int)width;
                    for (x = start; x < end; ++x) {
                        row[x] = 0u;
                    }
                }
            }
            memcpy(reference, current, current_count * sizeof(*reference));
            reference_count = current_count;
        }
    }

rows_done:
    *out_gray8 = gray;
    gray = NULL;
    ok = 1;

done:
    free(white);
    free(black);
    free(reference);
    free(current);
    free(gray);
    return ok;
}

int pdf_ccitt_decode_image(const uint8_t *data, size_t size, uint32_t width,
                           uint32_t height, int black_is1, uint8_t **out_gray8)
{
    uint8_t *gray = NULL;
    size_t count;
    size_t index;
    if (!pdf_ccitt_decode(data, size, width, height, &gray)) {
        return 0;
    }
    /* /BlackIs1 true means one bits are black, which is the reverse of the
     * 1-bit DeviceGray sample convention this reader blits into (0 = black),
     * so the samples have to be flipped; the default (false) already matches. */
    if (black_is1) {
        count = (size_t)width * (size_t)height;
        for (index = 0u; index < count; ++index) {
            gray[index] = (uint8_t)(255u - gray[index]);
        }
    }
    *out_gray8 = gray;
    return 1;
}

