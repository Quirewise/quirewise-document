/*
 * Regression: text must actually reach the raster.
 *
 * The rasteriser used to treat Tj/TJ as "skipped", so a text-only page came
 * out entirely white. Text is now drawn with the built-in approximation font;
 * this test pins the pixels down and checks the documented degradation note
 * for strings the font cannot represent.
 */

#include "document_backend.h"

#include <quirewise/document.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

static const char text_pdf[] =
    "%PDF-1.4\n"
    "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] "
    "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>\nendobj\n"
    "4 0 obj\n<< /Length 32 >>\nstream\n"
    "BT /F1 24 Tf 10 20 Td (A) Tj ET\n"
    "endstream\nendobj\n"
    "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\n"
    "endobj\n"
    "trailer\n<< /Root 1 0 R >>\n%%EOF\n";

/* Same page, but the string only contains bytes the font cannot map. */
static const char unprintable_pdf[] =
    "%PDF-1.4\n"
    "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] "
    "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>\nendobj\n"
    "4 0 obj\n<< /Length 35 >>\nstream\n"
    "BT /F1 24 Tf 10 20 Td <8001> Tj ET\n"
    "endstream\nendobj\n"
    "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\n"
    "endobj\n"
    "trailer\n<< /Root 1 0 R >>\n%%EOF\n";

static int pixel_is_white(const QwPageImage *page, size_t x, size_t y)
{
    size_t offset = (y * page->width + x) * 3u;
    return page->data[offset] == 0xffu && page->data[offset + 1u] == 0xffu &&
           page->data[offset + 2u] == 0xffu;
}

static size_t region_ink(const QwPageImage *page, size_t x0, size_t x1,
                         size_t y0, size_t y1)
{
    size_t x;
    size_t y;
    size_t ink = 0u;
    if (x1 > page->width) x1 = page->width;
    if (y1 > page->height) y1 = page->height;
    for (y = y0; y < y1; ++y) {
        for (x = x0; x < x1; ++x) {
            if (!pixel_is_white(page, x, y)) ++ink;
        }
    }
    return ink;
}

static int render(const char *pdf, size_t size, QwPageImage *out,
                  QwConvertReport *report)
{
    QwPdfReader *reader = NULL;
    size_t pages = 0u;
    int rc = qw_pdf_reader_open((const uint8_t *)pdf, size, &reader, &pages,
                                report);
    if (rc != 0) return rc;
    rc = qw_pdf_reader_render_page(reader, 0u, 72u, out, report);
    qw_pdf_reader_close(reader);
    return rc;
}

int main(void)
{
    QwConvertReport report;
    QwPageImage page;
    size_t ink;

    memset(&report, 0, sizeof(report));
    memset(&page, 0, sizeof(page));
    CHECK(render(text_pdf, sizeof(text_pdf) - 1u, &page, &report) == 0);
    CHECK(page.width == 100u && page.height == 100u);
    /* "A" sits at x=10..25, baseline y=20, cap height ~17 points. The raster
     * row is flipped, so user y maps to image row height-1-y. */
    ink = region_ink(&page, 9u, 26u, 61u, 81u);
    CHECK(ink > 10u);
    /* The rest of the page stays white. */
    CHECK(region_ink(&page, 55u, 95u, 55u, 95u) == 0u);
    CHECK(strstr(report.note, "approximation font") != NULL);
    free((void *)page.data);

    memset(&report, 0, sizeof(report));
    memset(&page, 0, sizeof(page));
    CHECK(render(unprintable_pdf, sizeof(unprintable_pdf) - 1u, &page,
                 &report) == 0);
    CHECK(region_ink(&page, 0u, 100u, 0u, 100u) == 0u);
    CHECK(strstr(report.note, "not painted") != NULL);
    free((void *)page.data);
    return 0;
}
