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

/* One page costs three objects: page, contents and image XObject. */
#define QW_RASTER_FIRST_PAGE_OBJECT 3u
/* A4 portrait in PDF points; matches the PDZ writer so every page route emits
 * the same media box for --page a4. */
#define QW_RASTER_A4_WIDTH 595.276
#define QW_RASTER_A4_HEIGHT 841.89

static int raster_page_channels(const QwRasterPage *page)
{
    if (page->channels == 1 || page->channels == 3) return page->channels;
    return 0;
}

int qw_raster_pages_to_pdf_mode(const QwRasterPage *pages,
                                size_t count,
                                int page_mode,
                                uint8_t **out_pdf,
                                size_t *out_size,
                                QwConvertReport *report)
{
    QwImagePdfBuffer buffer = {NULL, 0u, 0u};
    size_t objects;
    size_t *offsets;
    size_t page_index;
    int rc = QW_STATUS_OK;
    size_t xref;

    if (!pages || !count || !out_pdf || !out_size)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    if (count > ((size_t)-1 - 2u) / 3u) return QW_STATUS_ERR_RESOURCE_LIMIT;
    objects = 2u + count * 3u;
    offsets = (size_t *)calloc(objects + 1u, sizeof(size_t));
    if (!offsets) return QW_STATUS_ERR_OUT_OF_MEMORY;

    if (!buffer_append(&buffer, "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n", 15)) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto done;
    }
    offsets[1] = buffer.size;
    if (!buffer_printf(&buffer,
                       "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"))
        goto oom;
    offsets[2] = buffer.size;
    if (!buffer_printf(&buffer, "2 0 obj\n<< /Type /Pages /Kids ["))
        goto oom;
    for (page_index = 0u; page_index < count; ++page_index) {
        size_t page_object = QW_RASTER_FIRST_PAGE_OBJECT + page_index * 3u;
        if (!buffer_printf(&buffer, "%zu 0 R ", page_object)) goto oom;
    }
    if (!buffer_printf(&buffer, "] /Count %zu >>\nendobj\n", count)) goto oom;

    for (page_index = 0u; page_index < count; ++page_index) {
        const QwRasterPage *page = &pages[page_index];
        size_t page_object = QW_RASTER_FIRST_PAGE_OBJECT + page_index * 3u;
        size_t content_object = page_object + 1u;
        size_t image_object = page_object + 2u;
        size_t channels = (size_t)raster_page_channels(page);
        size_t row_bytes;
        size_t pixel_bytes;
        size_t row;
        char content[160];
        int content_length;

        if (!page->pixels || !page->width || !page->height || !channels) {
            rc = QW_STATUS_ERR_INVALID_ARGUMENT;
            goto done;
        }
        row_bytes = (size_t)page->width * channels;
        if (page->stride && page->stride < row_bytes) {
            rc = QW_STATUS_ERR_INVALID_ARGUMENT;
            goto done;
        }
        pixel_bytes = row_bytes * (size_t)page->height;
        if (page->height && pixel_bytes / page->height != row_bytes) {
            rc = QW_STATUS_ERR_RESOURCE_LIMIT;
            goto done;
        }
        {
            double scale_x;
            double scale_y;
            double offset_x = 0.0;
            double offset_y = 0.0;
            double box_w = (double)page->width;
            double box_h = (double)page->height;
            if (page_mode == QW_RASTER_PAGE_MODE_A4) {
                double fit = QW_RASTER_A4_WIDTH / (double)page->width;
                double fit_h = QW_RASTER_A4_HEIGHT / (double)page->height;
                if (fit_h < fit) fit = fit_h;
                /* An image XObject paints the unit square, so the cm matrix
                 * carries the drawn size in points, not a pixels-per-point
                 * ratio. */
                scale_x = (double)page->width * fit;
                scale_y = (double)page->height * fit;
                box_w = QW_RASTER_A4_WIDTH;
                box_h = QW_RASTER_A4_HEIGHT;
                offset_x = (QW_RASTER_A4_WIDTH - scale_x) / 2.0;
                offset_y = (QW_RASTER_A4_HEIGHT - scale_y) / 2.0;
            } else {
                /* IMAGE mode: the page box is the pixel size in points, so the
                 * image covers the whole page. */
                scale_x = (double)page->width;
                scale_y = (double)page->height;
            }
            content_length = snprintf(content, sizeof(content),
                                      "q\n%.4f 0 0 %.4f %.4f %.4f cm\n"
                                      "/Im0 Do\nQ\n",
                                      scale_x, scale_y, offset_x, offset_y);
            if (content_length <= 0 ||
                (size_t)content_length >= sizeof(content)) {
                rc = QW_STATUS_ERR_FORMAT;
                goto done;
            }

            offsets[page_object] = buffer.size;
            if (!buffer_printf(&buffer,
                               "%zu 0 obj\n<< /Type /Page /Parent 2 0 R "
                               "/MediaBox [0 0 %g %g] "
                               "/Resources << /XObject << /Im0 %zu 0 R >> >> "
                               "/Contents %zu 0 R >>\nendobj\n",
                               page_object, box_w, box_h,
                               image_object, content_object))
                goto oom;
        }

        offsets[content_object] = buffer.size;
        if (!buffer_printf(&buffer, "%zu 0 obj\n<< /Length %d >>\nstream\n",
                           content_object, content_length) ||
            !buffer_append(&buffer, content, (size_t)content_length) ||
            !buffer_append(&buffer, "endstream\nendobj\n", 17))
            goto oom;

        offsets[image_object] = buffer.size;
        if (!buffer_printf(&buffer,
                           "%zu 0 obj\n<< /Type /XObject /Subtype /Image "
                           "/Width %u /Height %u /ColorSpace /%s "
                           "/BitsPerComponent 8 /Length %zu >>\nstream\n",
                           image_object, page->width, page->height,
                           channels == 3 ? "DeviceRGB" : "DeviceGray",
                           pixel_bytes))
            goto oom;
        if (page->stride == 0u || page->stride == row_bytes) {
            if (!buffer_append(&buffer, page->pixels, pixel_bytes)) goto oom;
        } else {
            for (row = 0u; row < page->height; ++row) {
                if (!buffer_append(&buffer, page->pixels + row * page->stride,
                                   row_bytes))
                    goto oom;
            }
        }
        if (!buffer_append(&buffer, "\nendstream\nendobj\n", 18)) goto oom;
    }

    xref = buffer.size;
    if (!buffer_printf(&buffer, "xref\n0 %zu\n0000000000 65535 f \n",
                       objects + 1u))
        goto oom;
    for (page_index = 1u; page_index <= objects; ++page_index) {
        if (!buffer_printf(&buffer, "%010zu 00000 n \n", offsets[page_index]))
            goto oom;
    }
    if (!buffer_printf(&buffer,
                       "trailer\n<< /Size %zu /Root 1 0 R >>\n"
                       "startxref\n%zu\n%%%%EOF\n",
                       objects + 1u, xref))
        goto oom;

    *out_pdf = buffer.data;
    *out_size = buffer.size;
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "%zu raster page(s) written as uncompressed PDF images "
                 "(QW_ROUTE_LOSS_RASTERIZATION)", count);
        report->output_size = buffer.size;
        report->pages_written = count;
    }
    rc = QW_STATUS_OK;
    goto done;

oom:
    rc = QW_STATUS_ERR_OUT_OF_MEMORY;
    free(buffer.data);
done:
    free(offsets);
    return rc;
}

int qw_raster_pages_to_pdf(const QwRasterPage *pages,
                           size_t count,
                           uint8_t **out_pdf,
                           size_t *out_size,
                           QwConvertReport *report)
{
    return qw_raster_pages_to_pdf_mode(pages, count,
                                       QW_RASTER_PAGE_MODE_IMAGE,
                                       out_pdf, out_size, report);
}

int qw_doc_image_to_pdf_mode(const QwImage *image,
                             int page_mode,
                             uint8_t **out_pdf,
                             size_t *out_size,
                             QwConvertReport *report)
{
    QwRasterPage page;
    int channels;
    if (!image) return QW_STATUS_ERR_INVALID_ARGUMENT;
    channels = image->format == QW_IMAGE_PIXEL_RGB24 ? 3
               : image->format == QW_IMAGE_PIXEL_GRAY8 ? 1
                                                      : 0;
    if (!channels) return QW_STATUS_ERR_UNSUPPORTED;
    memset(&page, 0, sizeof(page));
    page.pixels = image->pixels;
    page.stride = image->stride;
    page.width = image->width;
    page.height = image->height;
    page.channels = channels;
    return qw_raster_pages_to_pdf_mode(&page, 1u, page_mode, out_pdf, out_size,
                                       report);
}

int qw_doc_image_to_pdf(const QwImage *image,
                        uint8_t **out_pdf,
                        size_t *out_size,
                        QwConvertReport *report)
{
    return qw_doc_image_to_pdf_mode(image, QW_RASTER_PAGE_MODE_IMAGE, out_pdf,
                                    out_size, report);
}
