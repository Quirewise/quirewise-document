/* PDF carries JBIG2 image XObjects with /JBIG2Decode. The stream below is a
 * 32x24 generic region coded with MMR; it was cross-checked against jbig2dec
 * and Ghostscript before being pinned here. */

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

#define PAGE_WIDTH 32u
#define PAGE_HEIGHT 24u

/* page information + MMR generic region + end of page/file */
static const uint8_t kJbig2Stream[] = {
    0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x01, 0x00, 0x00, 0x00, 0x13, 0x00,
    0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x01, 0x2c, 0x00,
    0x00, 0x01, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x26, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x73, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
    0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x23,
    0x61, 0xb9, 0x1b, 0x0d, 0xc8, 0xd8, 0x6e, 0x46, 0xc3, 0x73, 0x62, 0x11,
    0x81, 0xb9, 0xb1, 0x08, 0xc0, 0xdc, 0xd8, 0x84, 0x60, 0x6e, 0x6c, 0x42,
    0x30, 0x37, 0x36, 0x21, 0x18, 0x1b, 0x9b, 0x10, 0x8c, 0x0d, 0xcd, 0x88,
    0x46, 0x06, 0xe4, 0x6c, 0x37, 0x23, 0x61, 0xb9, 0x1b, 0x0d, 0xcb, 0xa3,
    0xa8, 0x53, 0x01, 0xb9, 0x84, 0x74, 0x0a, 0x60, 0x37, 0x36, 0x88, 0x61,
    0x4c, 0x06, 0xe7, 0x11, 0x20, 0x29, 0x80, 0xdc, 0xf2, 0x28, 0x0a, 0x60,
    0x37, 0x3e, 0x89, 0xc2, 0x98, 0x0d, 0xcc, 0xd1, 0xa0, 0x53, 0x01, 0xb9,
    0xa2, 0x20, 0xc3, 0x72, 0x36, 0x1b, 0x91, 0xb0, 0xdc, 0x00, 0x40, 0x04,
    0x00, 0x00, 0x00, 0x02, 0x31, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x03, 0x33, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
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

static int build_pdf(Buffer *out, int invert)
{
    size_t offsets[5];
    size_t index;
    size_t xref;
    static const char content[] = "q 32 0 0 24 0 0 cm /Im0 Do Q";

    memset(out, 0, sizeof(*out));
    if (!buffer_append(out, "%PDF-1.4\n", 9u)) return 0;
    offsets[0] = out->size;
    if (!buffer_appendf(out, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) return 0;
    offsets[1] = out->size;
    if (!buffer_appendf(out, "2 0 obj\n<< /Type /Pages /Count 1 /Kids [3 0 R] >>\nendobj\n")) return 0;
    offsets[2] = out->size;
    if (!buffer_appendf(out,
                        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 32 24] "
                        "/Resources << /XObject << /Im0 5 0 R >> >> /Contents 4 0 R >>\nendobj\n")) {
        return 0;
    }
    offsets[3] = out->size;
    if (!buffer_appendf(out, "4 0 obj\n<< /Length %zu >>\nstream\n", sizeof(content) - 1u)) return 0;
    if (!buffer_append(out, content, sizeof(content) - 1u)) return 0;
    if (!buffer_append(out, "\nendstream\nendobj\n", 18u)) return 0;
    offsets[4] = out->size;
    if (!buffer_appendf(out,
                        "5 0 obj\n<< /Type /XObject /Subtype /Image /Width 32 /Height 24 "
                        "/ColorSpace /DeviceGray /BitsPerComponent 1 /Filter /JBIG2Decode%s "
                        "/Length %zu >>\nstream\n", invert ? " /Decode [1 0]" : "",
                        sizeof(kJbig2Stream))) {
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

static int expected_dark(uint32_t x, uint32_t y)
{
    if (x >= 4u && x <= 12u && y >= 4u && y <= 10u) return 1;
    if (x >= 18u && x <= 28u && y >= 14u && y <= 20u) return 1;
    if (y >= 14u && y <= 21u && x == y - 12u) return 1;
    return 0;
}

static int render_and_check(int invert, const char *label)
{
    Buffer pdf;
    QwPdfDocument *document = NULL;
    size_t pages = 0u;
    QwConvertReport report;
    QwPageImage page;
    uint32_t x;
    uint32_t y;
    int mismatches = 0;

    CHECK(build_pdf(&pdf, invert) == 1);
    memset(&report, 0, sizeof(report));
    memset(&page, 0, sizeof(page));
    if (qw_pdf_open(pdf.data, pdf.size, &document, &pages, &report) != QW_STATUS_OK ||
        !document || pages != 1u) {
        fprintf(stderr, "%s: open failed\n", label);
        ++g_failures;
        free(pdf.data);
        return 0;
    }
    if (qw_pdf_render_page(document, 0u, 72u, &page, &report) != QW_STATUS_OK ||
        page.width != PAGE_WIDTH || page.height != PAGE_HEIGHT ||
        page.kind != QW_PAGE_IMAGE_RGB24) {
        fprintf(stderr, "%s: render failed (%ux%u kind=%d)\n", label, page.width,
                page.height, (int)page.kind);
        ++g_failures;
        qw_pdf_page_free(&page);
        qw_pdf_close(document);
        free(pdf.data);
        return 0;
    }
    for (y = 0u; y < PAGE_HEIGHT; ++y) {
        for (x = 0u; x < PAGE_WIDTH; ++x) {
            const uint8_t *pixel = page.data + ((size_t)y * page.width + x) * 3u;
            int is_dark = pixel[0] < 128;
            int want = invert ? !expected_dark(x, y) : expected_dark(x, y) != 0;
            if (is_dark != want) {
                if (mismatches < 8) {
                    fprintf(stderr, "%s: pixel %u,%u dark=%d expected=%d\n",
                            label, x, y, is_dark, want);
                }
                ++mismatches;
            }
        }
    }
    if (mismatches) {
        fprintf(stderr, "%s: %d mismatched pixels\n", label, mismatches);
        ++g_failures;
    }
    qw_pdf_page_free(&page);
    qw_pdf_close(document);
    free(pdf.data);
    return mismatches == 0;
}

int main(void)
{
    render_and_check(0, "plain");
    render_and_check(1, "/Decode [1 0]");
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    puts("PASS: PDF JBIG2 image renders");
    return 0;
}
