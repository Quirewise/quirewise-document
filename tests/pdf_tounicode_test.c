/*
 * Regression: ToUnicode CMaps turn CID glyph codes into text.
 *
 * Without the CMap a Type0 font's codes are interpreted as UCS-2, which yields
 * mojibake for every embedded CJK font. This fixture uses:
 *   - bfchar for two codes (你 好),
 *   - bfrange for two more (世 丗),
 *   - a surrogate pair (𠀀, U+20000) to exercise 4-byte UTF-8 output,
 *   - a simple font without a CMap to keep the legacy fallback working.
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

static const char cmap_stream[] =
    "/CIDInit /ProcSet findresource begin\n"
    "12 dict begin\n"
    "begincmap\n"
    "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
    "/CMapName /Adobe-Identity-UCS def\n"
    "/CMapType 2 def\n"
    "1 begincodespacerange\n"
    "<0000> <FFFF>\n"
    "endcodespacerange\n"
    "3 beginbfchar\n"
    "<0001> <4F60>\n"
    "<0002> <597D>\n"
    "<0005> <D840DC00>\n"
    "endbfchar\n"
    "1 beginbfrange\n"
    "<0003> <0004> <4E16>\n"
    "endbfrange\n"
    "endcmap\n"
    "CMapName currentdict /CMap defineresource pop\n"
    "end\n"
    "end\n";

static const char content_stream[] =
    "BT /F1 12 Tf 10 40 Td <00010002000300040005> Tj /F2 12 Tf <41> Tj ET\n";

static size_t build_pdf(uint8_t *out, size_t capacity)
{
    size_t position = 0u;
    size_t offsets[9];
    size_t xref;
    int written;

    written = snprintf((char *)out + position, capacity - position,
                       "%%PDF-1.7\n");
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
                       "[0 0 200 100] /Resources << /Font << /F1 5 0 R /F2 8 0 R "
                       ">> >> /Contents 4 0 R >>\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[4] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "4 0 obj\n<< /Length %zu >>\nstream\n%s"
                       "endstream\nendobj\n",
                       strlen(content_stream), content_stream);
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[5] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "5 0 obj\n<< /Type /Font /Subtype /Type0 /BaseFont "
                       "/SimSun /Encoding /Identity-H /DescendantFonts "
                       "[6 0 R] /ToUnicode 7 0 R >>\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[6] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "6 0 obj\n<< /Type /Font /Subtype /CIDFontType2 "
                       "/BaseFont /SimSun /CIDSystemInfo << /Registry (Adobe) "
                       "/Ordering (Identity) /Supplement 0 >> >>\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[7] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "7 0 obj\n<< /Length %zu >>\nstream\n%s"
                       "endstream\nendobj\n",
                       strlen(cmap_stream), cmap_stream);
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[8] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "8 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont "
                       "/Helvetica >>\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    xref = position;
    written = snprintf((char *)out + position, capacity - position,
                       "xref\n0 9\n0000000000 65535 f \n");
    if (written <= 0) return 0u;
    position += (size_t)written;
    {
        size_t index;
        for (index = 1u; index < 9u; ++index) {
            written = snprintf((char *)out + position, capacity - position,
                               "%010zu 00000 n \n", offsets[index]);
            if (written <= 0) return 0u;
            position += (size_t)written;
        }
    }
    written = snprintf((char *)out + position, capacity - position,
                       "trailer\n<< /Size 9 /Root 1 0 R >>\nstartxref\n%zu\n"
                       "%%%%EOF\n",
                       xref);
    if (written <= 0) return 0u;
    position += (size_t)written;
    return position;
}

static int contains(const uint8_t *data, size_t size, const char *text,
                    size_t text_size)
{
    size_t index;
    if (!text_size || size < text_size) return 0;
    for (index = 0u; index + text_size <= size; ++index) {
        if (memcmp(data + index, text, text_size) == 0) return 1;
    }
    return 0;
}

int main(void)
{
    static uint8_t pdf[8192];
    size_t size = build_pdf(pdf, sizeof(pdf));
    static const char expected_cjk[] = "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96"
                                       "\xE4\xB8\x97";
    static const char expected_supplementary[] = "\xF0\xA0\x80\x80";
    QwPdfReader *reader = NULL;
    size_t pages = 0u;
    QwConvertReport report;
    uint8_t *text = NULL;
    size_t text_size = 0u;
    int rc;

    CHECK(size > 0u);
    memset(&report, 0, sizeof(report));
    rc = qw_pdf_reader_open(pdf, size, &reader, &pages, &report);
    CHECK(rc == 0 && reader && pages == 1u);
    rc = qw_pdf_reader_extract_text(reader, &text, &text_size, &report);
    qw_pdf_reader_close(reader);
    CHECK(rc == 0 && text);
    CHECK(contains(text, text_size, expected_cjk, sizeof(expected_cjk) - 1u));
    CHECK(contains(text, text_size, expected_supplementary,
                   sizeof(expected_supplementary) - 1u));
    CHECK(contains(text, text_size, "A", 1u));
    free(text);
    return 0;
}
