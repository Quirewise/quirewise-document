/*
 * Regression: stream payloads are located with /Length, not by scanning for
 * the first "endstream" byte sequence.
 *
 * A content stream that itself contains the word "endstream" (in a comment) is
 * legal. Marker scanning truncates the payload at that point and the trailing
 * text is lost; with a direct /Length the reader takes exactly the declared
 * bytes. This pins the /Length-first rule.
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

static const char content_stream[] =
    "% endstream is a legal word inside a stream\n"
    "BT /F1 12 Tf 10 40 Td (After marker) Tj ET\n";

static size_t build_pdf(uint8_t *out, size_t capacity)
{
    size_t position = 0u;
    size_t offsets[6];
    size_t xref;
    int written;

    written = snprintf((char *)out + position, capacity - position,
                       "%%PDF-1.4\n");
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
                       "[0 0 200 100] /Resources << /Font << /F1 5 0 R >> >> "
                       "/Contents 4 0 R >>\nendobj\n");
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
                       "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont "
                       "/Helvetica >>\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    xref = position;
    written = snprintf((char *)out + position, capacity - position,
                       "xref\n0 6\n0000000000 65535 f \n");
    if (written <= 0) return 0u;
    position += (size_t)written;
    {
        size_t index;
        for (index = 1u; index < 6u; ++index) {
            written = snprintf((char *)out + position, capacity - position,
                               "%010zu 00000 n \n", offsets[index]);
            if (written <= 0) return 0u;
            position += (size_t)written;
        }
    }
    written = snprintf((char *)out + position, capacity - position,
                       "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%zu\n"
                       "%%%%EOF\n",
                       xref);
    if (written <= 0) return 0u;
    position += (size_t)written;
    return position;
}

static int contains(const uint8_t *data, size_t size, const char *text)
{
    size_t length = strlen(text);
    size_t index;
    if (!length || size < length) return 0;
    for (index = 0u; index + length <= size; ++index) {
        if (memcmp(data + index, text, length) == 0) return 1;
    }
    return 0;
}

int main(void)
{
    static uint8_t pdf[8192];
    size_t size = build_pdf(pdf, sizeof(pdf));
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
    CHECK(contains(text, text_size, "After marker"));
    free(text);
    return 0;
}
