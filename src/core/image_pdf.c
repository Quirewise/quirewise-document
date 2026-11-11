#include "document_backend.h"

#include <quirewise/image.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct QwImagePdfBuffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
} QwImagePdfBuffer;

static int buffer_reserve(QwImagePdfBuffer *buffer, size_t extra)
{
    size_t needed = buffer->size + extra;
    size_t capacity;
    uint8_t *grown;
    if (needed < buffer->size) return 0;
    if (needed <= buffer->capacity) return 1;
    capacity = buffer->capacity ? buffer->capacity : 4096u;
    while (capacity < needed) {
        if (capacity > (size_t)-1 / 2u) return 0;
        capacity *= 2u;
    }
    grown = (uint8_t *)realloc(buffer->data, capacity);
    if (!grown) return 0;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 1;
}

static int buffer_append(QwImagePdfBuffer *buffer, const void *data,
                         size_t size)
{
    if (!buffer_reserve(buffer, size)) return 0;
    if (size) memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 1;
}

static int buffer_printf(QwImagePdfBuffer *buffer, const char *format, ...)
{
    char text[512];
    va_list args;
    int length;
    va_start(args, format);
    length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (length < 0) return 0;
    if ((size_t)length >= sizeof(text)) return 0;
    return buffer_append(buffer, text, (size_t)length);
}

/* Emit one uncompressed image page. Page size equals the pixel size in
 * points; RGB24 and Gray8 are written directly as an image XObject. */
int qw_doc_image_to_pdf(const QwImage *image,
                        uint8_t **out_pdf,
                        size_t *out_size,
                        QwConvertReport *report)
{
    QwImagePdfBuffer buffer = {NULL, 0u, 0u};
    size_t offsets[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    const uint8_t *pixels;
    size_t pixel_bytes;
    size_t row_bytes;
    size_t rows;
    int channels;
    int rc = QW_STATUS_OK;
    char content[160];
    int content_length;
    size_t index;

    if (!image || !out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    if (!image->pixels || !image->width || !image->height)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    channels = image->format == QW_IMAGE_PIXEL_RGB24 ? 3
               : image->format == QW_IMAGE_PIXEL_GRAY8 ? 1
                                                      : 0;
    if (!channels) return QW_STATUS_ERR_UNSUPPORTED;
    row_bytes = (size_t)image->width * (size_t)channels;
    rows = (size_t)image->height;
    if (image->stride && image->stride < row_bytes)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    pixel_bytes = row_bytes * rows;
    if (rows && pixel_bytes / rows != row_bytes)
        return QW_STATUS_ERR_RESOURCE_LIMIT;

    content_length = snprintf(content, sizeof(content),
                              "q\n%u 0 0 %u 0 0 cm\n/Im0 Do\nQ\n",
                              image->width, image->height);
    if (content_length <= 0 || (size_t)content_length >= sizeof(content))
        return QW_STATUS_ERR_FORMAT;

    if (!buffer_append(&buffer, "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n", 15)) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto done;
    }
    offsets[1] = buffer.size;
    if (!buffer_printf(&buffer,
                       "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"))
        goto oom;
    offsets[2] = buffer.size;
    if (!buffer_printf(&buffer,
                       "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"))
        goto oom;
    offsets[3] = buffer.size;
    if (!buffer_printf(&buffer,
                       "3 0 obj\n<< /Type /Page /Parent 2 0 R "
                       "/MediaBox [0 0 %u %u] "
                       "/Resources << /XObject << /Im0 5 0 R >> >> "
                       "/Contents 4 0 R >>\nendobj\n",
                       image->width, image->height))
        goto oom;
    offsets[4] = buffer.size;
    if (!buffer_printf(&buffer, "4 0 obj\n<< /Length %d >>\nstream\n",
                       content_length) ||
        !buffer_append(&buffer, content, (size_t)content_length) ||
        !buffer_append(&buffer, "endstream\nendobj\n", 16))
        goto oom;
    offsets[5] = buffer.size;
    if (!buffer_printf(&buffer,
                       "5 0 obj\n<< /Type /XObject /Subtype /Image "
                       "/Width %u /Height %u /ColorSpace /%s "
                       "/BitsPerComponent 8 /Length %zu >>\nstream\n",
                       image->width, image->height,
                       channels == 3 ? "DeviceRGB" : "DeviceGray",
                       pixel_bytes))
        goto oom;
    pixels = image->pixels;
    if (image->stride == 0u || image->stride == row_bytes) {
        if (!buffer_append(&buffer, pixels, pixel_bytes)) goto oom;
    } else {
        for (index = 0u; index < rows; ++index) {
            if (!buffer_append(&buffer, pixels + index * image->stride,
                               row_bytes))
                goto oom;
        }
    }
    if (!buffer_append(&buffer, "\nendstream\nendobj\n", 17)) goto oom;

    {
        size_t xref = buffer.size;
        if (!buffer_printf(&buffer, "xref\n0 6\n0000000000 65535 f \n"))
            goto oom;
        for (index = 1u; index < 6u; ++index) {
            if (!buffer_printf(&buffer, "%010zu 00000 n \n", offsets[index]))
                goto oom;
        }
        if (!buffer_printf(&buffer,
                           "trailer\n<< /Size 6 /Root 1 0 R >>\n"
                           "startxref\n%zu\n%%%%EOF\n",
                           xref))
            goto oom;
    }

    *out_pdf = buffer.data;
    *out_size = buffer.size;
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "image rendered as a single PDF page at %ux%u points "
                 "(QW_ROUTE_LOSS_RASTERIZATION)",
                 image->width, image->height);
        report->output_size = buffer.size;
        report->pages_written = 1u;
    }
    return QW_STATUS_OK;

oom:
    rc = QW_STATUS_ERR_OUT_OF_MEMORY;
done:
    free(buffer.data);
    return rc;
}
