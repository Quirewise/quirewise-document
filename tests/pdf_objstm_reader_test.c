/*
 * Regression: a document with more than one /Type /ObjStm stream.
 *
 * The reader keeps every member of every object stream in one growable slice
 * array. The reservation size used to be a local variable that restarted at
 * zero for each object stream, so a second stream re-allocated the array back
 * to 64 entries while the existing slice count was still larger and then wrote
 * past the end. This test builds two object streams whose members total more
 * than 64 slices, one page in each, and requires the reader to return both
 * pages.
 */

#include "document_backend.h"

#include <quirewise/document.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OBJSTM1_OBJECTS 100u
#define OBJSTM2_OBJECTS 2u

static uint32_t adler32_bytes(const uint8_t *data, size_t size)
{
    uint32_t low = 1u;
    uint32_t high = 0u;
    size_t index;
    for (index = 0u; index < size; ++index) {
        low = (low + data[index]) % 65521u;
        high = (high + low) % 65521u;
    }
    return (high << 16) | low;
}

static size_t zlib_store(const uint8_t *source, size_t size, uint8_t *dest,
                         size_t capacity)
{
    uint32_t checksum;
    uint16_t length = (uint16_t)size;
    size_t needed = size + 11u;
    if (size > 65535u || needed > capacity) return 0u;
    dest[0] = 0x78u;
    dest[1] = 0x01u;
    dest[2] = 0x01u; /* BFINAL = 1, BTYPE = 00 (stored) */
    dest[3] = (uint8_t)(length & 0xffu);
    dest[4] = (uint8_t)((length >> 8) & 0xffu);
    dest[5] = (uint8_t)(~length & 0xffu);
    dest[6] = (uint8_t)((~length >> 8) & 0xffu);
    memcpy(dest + 7u, source, size);
    checksum = adler32_bytes(source, size);
    dest[7u + size] = (uint8_t)(checksum >> 24);
    dest[8u + size] = (uint8_t)(checksum >> 16);
    dest[9u + size] = (uint8_t)(checksum >> 8);
    dest[10u + size] = (uint8_t)checksum;
    return needed;
}

static int build_object_stream(uint8_t *out, size_t capacity,
                               size_t *out_size, size_t *out_first,
                               size_t first_object, size_t count,
                               size_t page_object)
{
    char content[65536];
    size_t content_size = 0u;
    size_t header_size;
    size_t index;
    size_t offset = 0u;
    for (index = 0u; index < count; ++index) {
        size_t number = first_object + index;
        const char *body = number == page_object
                               ? "<< /Type /Page /Parent 2 0 R "
                                 "/MediaBox [0 0 100 100] >>"
                               : "<< /X 1 >>";
        int written = snprintf(content + content_size,
                               sizeof(content) - content_size, "%zu %zu ",
                               number, offset);
        if (written <= 0 ||
            content_size + (size_t)written >= sizeof(content)) {
            return 0;
        }
        content_size += (size_t)written;
        offset += strlen(body);
    }
    header_size = content_size;
    for (index = 0u; index < count; ++index) {
        size_t number = first_object + index;
        const char *body = number == page_object
                               ? "<< /Type /Page /Parent 2 0 R "
                                 "/MediaBox [0 0 100 100] >>"
                               : "<< /X 1 >>";
        size_t length = strlen(body);
        if (content_size + length >= sizeof(content)) return 0;
        memcpy(content + content_size, body, length);
        content_size += length;
    }
    *out_first = header_size;
    *out_size = zlib_store((const uint8_t *)content, content_size, out,
                           capacity);
    return *out_size != 0u;
}

static size_t build_pdf(uint8_t *pdf, size_t capacity)
{
    uint8_t stream1[8192];
    uint8_t stream2[1024];
    size_t size1 = 0u;
    size_t size2 = 0u;
    size_t first1 = 0u;
    size_t first2 = 0u;
    size_t position = 0u;
    int written;

    if (!build_object_stream(stream1, sizeof(stream1), &size1, &first1, 10u,
                             OBJSTM1_OBJECTS, 10u)) {
        return 0u;
    }
    if (!build_object_stream(stream2, sizeof(stream2), &size2, &first2, 200u,
                             OBJSTM2_OBJECTS, 200u)) {
        return 0u;
    }

    written = snprintf((char *)pdf + position, capacity - position,
                       "%%PDF-1.5\n"
                       "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
                       "2 0 obj\n<< /Type /Pages /Kids [10 0 R 200 0 R] "
                       "/Count 2 >>\nendobj\n"
                       "5 0 obj\n<< /Type /ObjStm /N %u /First %zu "
                       "/Length %zu >>\nstream\n",
                       OBJSTM1_OBJECTS, first1, size1);
    if (written <= 0 || position + (size_t)written >= capacity) return 0u;
    position += (size_t)written;
    if (position + size1 >= capacity) return 0u;
    memcpy(pdf + position, stream1, size1);
    position += size1;
    written = snprintf((char *)pdf + position, capacity - position,
                       "\nendstream\nendobj\n"
                       "6 0 obj\n<< /Type /ObjStm /N %u /First %zu "
                       "/Length %zu >>\nstream\n",
                       OBJSTM2_OBJECTS, first2, size2);
    if (written <= 0 || position + (size_t)written >= capacity) return 0u;
    position += (size_t)written;
    if (position + size2 >= capacity) return 0u;
    memcpy(pdf + position, stream2, size2);
    position += size2;
    written = snprintf((char *)pdf + position, capacity - position,
                       "\nendstream\nendobj\n%%%%EOF\n");
    if (written <= 0 || position + (size_t)written >= capacity) return 0u;
    position += (size_t)written;
    return position;
}

int main(void)
{
    static uint8_t pdf[65536];
    size_t pdf_size = build_pdf(pdf, sizeof(pdf));
    QwPdfReader *reader = NULL;
    size_t pages = 0u;
    QwConvertReport report;
    int rc;

    if (!pdf_size) {
        fprintf(stderr, "objstm reader: could not build the fixture\n");
        return 1;
    }
    memset(&report, 0, sizeof(report));
    rc = qw_pdf_reader_open(pdf, pdf_size, &reader, &pages, &report);
    if (rc != 0 || !reader || pages != 2u) {
        fprintf(stderr, "objstm reader: rc=%d pages=%zu note=%s\n", rc, pages,
                report.note);
        if (reader) qw_pdf_reader_close(reader);
        return 2;
    }
    qw_pdf_reader_close(reader);
    return 0;
}
