/* JBIG2 symbol dictionaries and text regions. The stream below builds eight
 * arithmetic-coded glyphs (template 2), exports all of them and places nine
 * instances on a 132x14 strip; the expected bitmap comes from jbig2dec. */

#include <quirewise/document.h>
#include <quirewise/pdf.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

#define SYM_WIDTH 132u
#define SYM_HEIGHT 14u
#define SYM_STRIDE ((SYM_WIDTH + 7u) / 8u)

/* page information, symbol dictionary and text region segments */
static const uint8_t kJbig2Stream[] = {
    0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x01, 0x00, 0x00, 0x00, 0x13, 0x00, 
    0x00, 0x00, 0x84, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 
    0x01, 0x00, 0x00, 0x00, 0x64, 0x08, 0x00, 0x02, 0xff, 0x00, 0x00, 0x00, 
    0x08, 0x00, 0x00, 0x00, 0x08, 0x3a, 0x48, 0x37, 0x8a, 0xac, 0xb4, 0x06, 
    0x30, 0xe0, 0x91, 0xea, 0x7b, 0xe5, 0x64, 0x1d, 0xfe, 0x6d, 0xbf, 0xf0, 
    0x45, 0x8d, 0xbd, 0xdb, 0x20, 0x91, 0xce, 0x6d, 0x70, 0x74, 0xe6, 0xf3, 
    0x91, 0x4f, 0x6e, 0x2f, 0x68, 0xbb, 0xe0, 0x2f, 0x6c, 0xeb, 0x6b, 0x85, 
    0xe2, 0x28, 0xd2, 0x54, 0x53, 0x12, 0x1e, 0x91, 0xd1, 0xce, 0x4a, 0x44, 
    0x21, 0x81, 0x3c, 0xd2, 0x8b, 0x9d, 0x43, 0xff, 0x2a, 0xd0, 0x18, 0xb3, 
    0x31, 0xc9, 0xc1, 0xf3, 0x4f, 0x5f, 0xbb, 0x37, 0x5f, 0x60, 0x37, 0xa2, 
    0xef, 0x8a, 0xb9, 0x53, 0xdc, 0x34, 0x1b, 0xff, 0xac, 0x00, 0x00, 0x00, 
    0x02, 0x07, 0x20, 0x01, 0x01, 0x00, 0x00, 0x00, 0x26, 0x00, 0x00, 0x00, 
    0x84, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x09, 0xe8, 0xd0, 0x9e, 0xf9, 
    0xda, 0xf0, 0x87, 0x70, 0x61, 0xc5, 0x39, 0xfa, 0xbf, 0xff, 0xac, 
};

/* reference decode of the stream above, packed 1 bit per pixel (1 = black) */
static const uint8_t kExpectedBits[] = {
    0x0f, 0xf3, 0xfe, 0x07, 0xfc, 0x07, 0xe8, 0x1f, 0x00, 0x01, 0xff, 0xcf, 
    0xfe, 0x1e, 0x3c, 0x7f, 0xf0, 0x00, 0x80, 0x81, 0x00, 0x40, 0x08, 0x18, 
    0x20, 0x80, 0x01, 0x08, 0x42, 0x02, 0x04, 0x10, 0x42, 0x10, 0x00, 0x80, 
    0x80, 0x80, 0x40, 0x10, 0x08, 0x40, 0x40, 0x01, 0x08, 0x42, 0x02, 0x02, 
    0x20, 0x42, 0x10, 0x00, 0x80, 0x80, 0x80, 0x40, 0x10, 0x00, 0x40, 0x20, 
    0x01, 0x08, 0x42, 0x00, 0x02, 0x20, 0x42, 0x10, 0x00, 0x80, 0x80, 0x80, 
    0x40, 0x20, 0x00, 0x00, 0x20, 0x01, 0x08, 0x42, 0x10, 0x01, 0x40, 0x42, 
    0x10, 0x00, 0x80, 0x81, 0x00, 0x40, 0x20, 0x00, 0x00, 0x20, 0x00, 0x08, 
    0x02, 0x10, 0x00, 0x80, 0x02, 0x00, 0x00, 0x80, 0xfe, 0x00, 0x40, 0x20, 
    0x00, 0x00, 0x40, 0x00, 0x08, 0x03, 0xf0, 0x00, 0x80, 0x02, 0x00, 0x00, 
    0x80, 0x81, 0x80, 0x40, 0x20, 0x00, 0x01, 0x80, 0x00, 0x08, 0x02, 0x10, 
    0x01, 0x40, 0x02, 0x00, 0xc0, 0x80, 0x80, 0x40, 0x40, 0x20, 0xfc, 0x06, 
    0x00, 0x00, 0x08, 0x02, 0x10, 0x02, 0x20, 0x02, 0x00, 0x80, 0x80, 0x80, 
    0x40, 0x40, 0x20, 0x08, 0x08, 0x00, 0x00, 0x08, 0x02, 0x01, 0x02, 0x20, 
    0x02, 0x00, 0x80, 0x80, 0x80, 0x40, 0x40, 0x30, 0x08, 0x10, 0x00, 0x00, 
    0x08, 0x02, 0x01, 0x04, 0x10, 0x02, 0x00, 0x80, 0x80, 0x80, 0x40, 0x40, 
    0x10, 0x08, 0x20, 0x00, 0x00, 0x08, 0x02, 0x01, 0x08, 0x08, 0x02, 0x00, 
    0x41, 0x00, 0x80, 0x80, 0x40, 0x08, 0x18, 0x40, 0x20, 0x00, 0x08, 0x02, 
    0x01, 0x08, 0x08, 0x02, 0x00, 0x3e, 0x03, 0xff, 0x07, 0xfc, 0x07, 0xe0, 
    0x7f, 0xe0, 0x00, 0x3e, 0x0f, 0xff, 0x1e, 0x1e, 0x0f, 0x80, 
};

typedef struct Buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
} Buffer;

static int buffer_append(Buffer *buffer, const void *data, size_t size)
{
    if (buffer->size + size > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 4096u;
        uint8_t *grown;
        while (capacity < buffer->size + size) capacity *= 2u;
        grown = (uint8_t *)realloc(buffer->data, capacity);
        if (!grown) return 0;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 1;
}

static int buffer_appendf(Buffer *buffer, const char *format, ...)
{
    char text[512];
    va_list args;
    int written;
    va_start(args, format);
    written = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= sizeof(text)) return 0;
    return buffer_append(buffer, text, (size_t)written);
}

static int build_pdf(Buffer *out)
{
    size_t offsets[5];
    size_t index;
    size_t xref;
    static const char content[] = "q 132 0 0 14 0 0 cm /Im0 Do Q";
    memset(out, 0, sizeof(*out));
    if (!buffer_append(out, "%PDF-1.4\n", 9u)) return 0;
    offsets[0] = out->size;
    if (!buffer_appendf(out, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) return 0;
    offsets[1] = out->size;
    if (!buffer_appendf(out, "2 0 obj\n<< /Type /Pages /Count 1 /Kids [3 0 R] >>\nendobj\n")) return 0;
    offsets[2] = out->size;
    if (!buffer_appendf(out,
                        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 132 14] "
                        "/Resources << /XObject << /Im0 5 0 R >> >> /Contents 4 0 R >>\nendobj\n")) {
        return 0;
    }
    offsets[3] = out->size;
    if (!buffer_appendf(out, "4 0 obj\n<< /Length %zu >>\nstream\n", sizeof(content) - 1u)) return 0;
    if (!buffer_append(out, content, sizeof(content) - 1u)) return 0;
    if (!buffer_append(out, "\nendstream\nendobj\n", 18u)) return 0;
    offsets[4] = out->size;
    if (!buffer_appendf(out,
                        "5 0 obj\n<< /Type /XObject /Subtype /Image /Width 132 /Height 14 "
                        "/ColorSpace /DeviceGray /BitsPerComponent 1 /Filter /JBIG2Decode "
                        "/Length %zu >>\nstream\n", sizeof(kJbig2Stream))) {
        return 0;
    }
    if (!buffer_append(out, kJbig2Stream, sizeof(kJbig2Stream))) return 0;
    if (!buffer_append(out, "\nendstream\nendobj\n", 18u)) return 0;
    xref = out->size;
    if (!buffer_appendf(out, "xref\n0 6\n0000000000 65535 f \n")) return 0;
    for (index = 0u; index < 5u; ++index) {
        if (!buffer_appendf(out, "%010zu 00000 n \n", offsets[index])) return 0;
    }
    if (!buffer_appendf(out, "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n", xref)) {
        return 0;
    }
    return 1;
}

int main(void)
{
    Buffer pdf;
    QwPdfDocument *document = NULL;
    size_t pages = 0u;
    QwConvertReport report;
    QwPageImage page;
    uint32_t x;
    uint32_t y;
    int mismatches = 0;

    CHECK(build_pdf(&pdf) == 1);
    memset(&report, 0, sizeof(report));
    memset(&page, 0, sizeof(page));
    if (qw_pdf_open(pdf.data, pdf.size, &document, &pages, &report) != QW_STATUS_OK ||
        !document || pages != 1u) {
        fputs("symbol dictionary: open failed\n", stderr);
        ++g_failures;
        free(pdf.data);
        return 1;
    }
    if (qw_pdf_render_page(document, 0u, 72u, &page, &report) != QW_STATUS_OK ||
        page.width != SYM_WIDTH || page.height != SYM_HEIGHT ||
        page.kind != QW_PAGE_IMAGE_RGB24) {
        fprintf(stderr, "symbol dictionary: render failed (%ux%u kind=%d)\n",
                page.width, page.height, (int)page.kind);
        ++g_failures;
        qw_pdf_page_free(&page);
        qw_pdf_close(document);
        free(pdf.data);
        return 1;
    }
    for (y = 0u; y < SYM_HEIGHT; ++y) {
        for (x = 0u; x < SYM_WIDTH; ++x) {
            const uint8_t *pixel = page.data + ((size_t)y * page.width + x) * 3u;
            int dark = pixel[0] < 128;
            int want = (kExpectedBits[(size_t)y * SYM_STRIDE + (x >> 3)] >>
                        (7u - (x & 7u))) & 1;
            if (dark != want) {
                if (mismatches < 6) {
                    fprintf(stderr, "symbol dictionary: pixel %u,%u dark=%d\n", x, y, dark);
                }
                ++mismatches;
            }
        }
    }
    if (mismatches) {
        fprintf(stderr, "symbol dictionary: %d mismatched pixels\n", mismatches);
        ++g_failures;
    }
    qw_pdf_page_free(&page);
    qw_pdf_close(document);
    free(pdf.data);
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    puts("PASS: JBIG2 symbol dictionaries and text regions");
    return 0;
}
