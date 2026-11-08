#ifndef QUIREWISE_TEST_PDF_INCREMENTAL_FIXTURE_H
#define QUIREWISE_TEST_PDF_INCREMENTAL_FIXTURE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * A two-revision PDF: object 4 (the page content stream) is first defined with
 * the text "OLD" and redefined in an appended revision with "NEW". The newest
 * xref section points at the NEW definition and links back through /Prev.
 *
 * Shared by the xref reader test and the merger test.
 */
static size_t pdf_build_incremental(uint8_t *out, size_t capacity)
{
    size_t position = 0u;
    size_t offsets[6];
    size_t xref1;
    size_t xref2;
    size_t off4b;
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
                       "4 0 obj\n<< /Length 34 >>\nstream\n"
                       "BT /F1 12 Tf 10 40 Td (OLD) Tj ET\n"
                       "endstream\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    offsets[5] = position;
    written = snprintf((char *)out + position, capacity - position,
                       "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont "
                       "/Helvetica >>\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    xref1 = position;
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
                       xref1);
    if (written <= 0) return 0u;
    position += (size_t)written;

    off4b = position;
    written = snprintf((char *)out + position, capacity - position,
                       "4 0 obj\n<< /Length 34 >>\nstream\n"
                       "BT /F1 12 Tf 10 40 Td (NEW) Tj ET\n"
                       "endstream\nendobj\n");
    if (written <= 0) return 0u;
    position += (size_t)written;

    xref2 = position;
    written = snprintf((char *)out + position, capacity - position,
                       "xref\n4 1\n%010zu 00000 n \n", off4b);
    if (written <= 0) return 0u;
    position += (size_t)written;
    written = snprintf((char *)out + position, capacity - position,
                       "trailer\n<< /Size 6 /Root 1 0 R /Prev %zu >>\n"
                       "startxref\n%zu\n%%%%EOF\n",
                       xref1, xref2);
    if (written <= 0) return 0u;
    position += (size_t)written;
    return position;
}

#endif
