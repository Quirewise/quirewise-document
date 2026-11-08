/* The PDF writer emits bitonal pages with /CCITTFaxDecode. Before the reader
 * grew a T.6 path those pages rendered blank, which looks exactly like a broken
 * document. The stream below comes from libtiff (compression=group4) for a
 * 64x40 page whose ink follows ((x + y * 7) % 13) < 3, so the expectation does
 * not depend on this package's own encoder. */

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

#define PAGE_WIDTH 64u
#define PAGE_HEIGHT 40u

/* libtiff Group 4 strip for the test page. */
static const uint8_t kCcittStream[] = {
    0x30, 0x10, 0xc0, 0x43, 0x01, 0x0c, 0x04, 0x30, 0x20, 0x22, 0x50, 0x26,
    0x81, 0x34, 0x09, 0xa0, 0x4d, 0x10, 0xc4, 0xd0, 0x26, 0x81, 0x34, 0x09,
    0xa0, 0x61, 0x13, 0x81, 0x34, 0x09, 0xa0, 0x4d, 0x02, 0x68, 0xb1, 0x34,
    0x09, 0xa0, 0x4d, 0x02, 0x68, 0x15, 0x45, 0xc0, 0x9a, 0x04, 0xd0, 0x26,
    0x81, 0x34, 0x38, 0x9a, 0x04, 0xd0, 0x26, 0x81, 0x34, 0x08, 0x59, 0xa0,
    0x4d, 0x02, 0x68, 0x13, 0x40, 0x9a, 0x03, 0x89, 0xa0, 0x4d, 0x02, 0x68,
    0x13, 0x40, 0x82, 0xcf, 0x02, 0x68, 0x13, 0x40, 0x9a, 0x04, 0xd0, 0x0e,
    0x26, 0x81, 0x34, 0x09, 0xa0, 0x4d, 0x02, 0x68, 0xca, 0x81, 0x34, 0x09,
    0xa0, 0x4d, 0x02, 0x68, 0x1c, 0x4d, 0x02, 0x68, 0x13, 0x40, 0x9a, 0x04,
    0xd1, 0x06, 0x26, 0x81, 0x34, 0x09, 0xa0, 0x4d, 0x03, 0x04, 0x4a, 0x04,
    0xd0, 0x26, 0x81, 0x34, 0x09, 0xa2, 0x18, 0x9a, 0x04, 0xd0, 0x26, 0x81,
    0x34, 0x0c, 0x22, 0x70, 0x26, 0x81, 0x34, 0x09, 0xa0, 0x4d, 0x16, 0x26,
    0x81, 0x34, 0x09, 0xa0, 0x4d, 0x02, 0xa8, 0xb8, 0x13, 0x40, 0x9a, 0x04,
    0xd0, 0x26, 0x87, 0x13, 0x40, 0x9a, 0x04, 0xd0, 0x26, 0x81, 0x0b, 0x34,
    0x09, 0xa0, 0x4d, 0x02, 0x68, 0x13, 0x40, 0x71, 0x34, 0x09, 0xa0, 0x4d,
    0x02, 0x68, 0x10, 0x59, 0xe0, 0x4d, 0x02, 0x68, 0x13, 0x40, 0x9a, 0x01,
    0xc4, 0xd0, 0x26, 0x81, 0x34, 0x09, 0xa0, 0x4d, 0x19, 0x50, 0x26, 0x81,
    0x34, 0x09, 0xa0, 0x4d, 0x03, 0x89, 0xa0, 0x4d, 0x02, 0x68, 0x13, 0x40,
    0x9a, 0x20, 0xc4, 0xd0, 0x26, 0x81, 0x34, 0x09, 0xa0, 0x60, 0x89, 0x40,
    0x9a, 0x04, 0xd0, 0x26, 0x81, 0x34, 0x43, 0x13, 0x40, 0x9a, 0x04, 0xd0,
    0x26, 0x81, 0x84, 0x4e, 0x04, 0xd0, 0x26, 0x81, 0x34, 0x09, 0xa2, 0xc4,
    0xd0, 0x26, 0x81, 0x34, 0x09, 0xa0, 0x55, 0x17, 0x02, 0x68, 0x13, 0x40,
    0x9a, 0x04, 0xd0, 0xe2, 0x68, 0x13, 0x40, 0x9a, 0x04, 0xd0, 0x21, 0x66,
    0x81, 0x34, 0x09, 0xa0, 0x4d, 0x02, 0x68, 0x0e, 0x26, 0x81, 0x34, 0x09,
    0xa0, 0x4d, 0x02, 0x0b, 0x3c, 0x09, 0xa0, 0x4d, 0x02, 0x68, 0x13, 0x40,
    0x38, 0x9a, 0x04, 0xd0, 0x26, 0x81, 0x34, 0x09, 0xa3, 0x2a, 0x04, 0xd0,
    0x26, 0x81, 0x34, 0x09, 0xa0, 0x71, 0x34, 0x09, 0xa0, 0x4d, 0x02, 0x68,
    0x13, 0x44, 0x18, 0x9a, 0x04, 0xd0, 0x26, 0x81, 0x34, 0x0c, 0x00, 0x40,
    0x04,
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
    static const char content[] = "q 64 0 0 40 0 0 cm /Im0 Do Q";

    memset(out, 0, sizeof(*out));
    if (!buffer_append(out, "%PDF-1.4\n", 9u)) return 0;
    offsets[0] = out->size;
    if (!buffer_appendf(out, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) return 0;
    offsets[1] = out->size;
    if (!buffer_appendf(out, "2 0 obj\n<< /Type /Pages /Count 1 /Kids [3 0 R] >>\nendobj\n")) return 0;
    offsets[2] = out->size;
    if (!buffer_appendf(out,
                        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 64 40] "
                        "/Resources << /XObject << /Im0 5 0 R >> >> /Contents 4 0 R >>\nendobj\n")) {
        return 0;
    }
    offsets[3] = out->size;
    if (!buffer_appendf(out, "4 0 obj\n<< /Length %zu >>\nstream\n", sizeof(content) - 1u)) return 0;
    if (!buffer_append(out, content, sizeof(content) - 1u)) return 0;
    if (!buffer_append(out, "\nendstream\nendobj\n", 18u)) return 0;
    offsets[4] = out->size;
    if (!buffer_appendf(out,
                        "5 0 obj\n<< /Type /XObject /Subtype /Image /Width 64 /Height 40 "
                        "/ColorSpace /DeviceGray /BitsPerComponent 1 /Filter /CCITTFaxDecode "
                        "/DecodeParms << /K -1 /Columns 64 /Rows 40 /BlackIs1 true >> "
                        "/Length %zu >>\nstream\n", sizeof(kCcittStream))) {
        return 0;
    }
    if (!buffer_append(out, kCcittStream, sizeof(kCcittStream))) return 0;
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
    size_t dark = 0u;

    CHECK(build_pdf(&pdf) == 1);
    memset(&report, 0, sizeof(report));
    memset(&page, 0, sizeof(page));
    CHECK(qw_pdf_open(pdf.data, pdf.size, &document, &pages, &report) == QW_STATUS_OK);
    CHECK(document != NULL);
    CHECK(pages == 1u);
    if (document && pages == 1u) {
        CHECK(qw_pdf_render_page(document, 0u, 72u, &page, &report) == QW_STATUS_OK);
        CHECK(page.width == PAGE_WIDTH);
        CHECK(page.height == PAGE_HEIGHT);
        if (page.data && page.kind == QW_PAGE_IMAGE_RGB24 &&
            page.width == PAGE_WIDTH && page.height == PAGE_HEIGHT) {
            for (y = 0u; y < PAGE_HEIGHT; ++y) {
                for (x = 0u; x < PAGE_WIDTH; ++x) {
                    const uint8_t *pixel = page.data + ((size_t)y * page.width + x) * 3u;
                    int is_dark = pixel[0] < 128;
                    int expected = ((x + y * 7u) % 13u) < 3u;
                    if (is_dark) ++dark;
                    if (is_dark != expected) {
                        if (g_failures < 8) {
                            fprintf(stderr, "pixel %u,%u: dark=%d expected=%d\n",
                                    x, y, is_dark, expected);
                        }
                        ++g_failures;
                    }
                }
            }
            /* A blank page would pass only if nothing is expected dark. */
            CHECK(dark > 100u);
        } else {
            fprintf(stderr, "unexpected render format: kind=%d %ux%u size=%zu\n",
                    (int)page.kind, page.width, page.height, page.size);
            ++g_failures;
        }
    }
    qw_pdf_page_free(&page);
    if (document) qw_pdf_close(document);
    free(pdf.data);
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    puts("PASS: PDF CCITT Group 4 image renders");
    return 0;
}
