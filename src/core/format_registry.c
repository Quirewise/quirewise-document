/* Document-local format registry.
 *
 * The registry header declares a full dispatch surface, but nothing defined it.
 * This file implements the parts that make sense for the formats this package
 * owns (OpenDocument text/presentation, flat ODF, HWPX, EML): lookup,
 * enumeration, content detection, capability queries, resource profiles,
 * validation and detection bookkeeping.
 *
 * Formats owned by other packages (images, audio, archives, spreadsheets, ...)
 * are deliberately absent: referencing their descriptors here would create
 * undefined symbols for a standalone document install. Their lookups return NULL
 * or an empty result instead of guessing.
 *
 * Return convention: predicates and metadata getters return 1 on success and 0
 * on failure; the resource query returns a profile pointer (never NULL for a
 * known format). */

#include "format_registry.h"

#include "../backends/mupdf_backend.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const QwFormatDescriptor qw_format_eml;
extern const QwFormatDescriptor qw_format_fodp;
extern const QwFormatDescriptor qw_format_fodt;
extern const QwFormatDescriptor qw_format_hwpx;
extern const QwFormatDescriptor qw_format_odp;
extern const QwFormatDescriptor qw_format_odt;
extern const QwFormatDescriptor qw_format_otp;
extern const QwFormatDescriptor qw_format_ott;
extern const QwFormatDescriptor qw_format_potm;
extern const QwFormatDescriptor qw_format_potx;
extern const QwFormatDescriptor qw_format_ppsm;
extern const QwFormatDescriptor qw_format_ppsx;
extern const QwFormatDescriptor qw_format_pptm;
extern const QwFormatDescriptor qw_format_pptx;

static const QwFormatDescriptor *const qw_format_registry[] = {
    &qw_format_eml,
    &qw_format_fodp,
    &qw_format_fodt,
    &qw_format_hwpx,
    &qw_format_odp,
    &qw_format_odt,
    &qw_format_otp,
    &qw_format_ott,
    &qw_format_potm,
    &qw_format_potx,
    &qw_format_ppsm,
    &qw_format_ppsx,
    &qw_format_pptm,
    &qw_format_pptx
};

#define QW_FORMAT_REGISTRY_COUNT \
    (sizeof(qw_format_registry) / sizeof(qw_format_registry[0]))

size_t qw_format_registry_count(void)
{
    return QW_FORMAT_REGISTRY_COUNT;
}

const QwFormatDescriptor *qw_format_registry_at(size_t index)
{
    if (index >= QW_FORMAT_REGISTRY_COUNT) return NULL;
    return qw_format_registry[index];
}

const QwFormatDescriptor *qw_format_find(QwInputFormat id)
{
    size_t index;
    for (index = 0u; index < QW_FORMAT_REGISTRY_COUNT; ++index) {
        if (qw_format_registry[index]->id == id) return qw_format_registry[index];
    }
    return NULL;
}

const QwFormatDescriptor *qw_format_detect(const void *data, size_t size)
{
    size_t index;
    if (!data || !size) return NULL;
    for (index = 0u; index < QW_FORMAT_REGISTRY_COUNT; ++index) {
        const QwFormatDescriptor *format = qw_format_registry[index];
        if (format->probe && format->probe(data, size)) return format;
    }
    return NULL;
}

size_t qw_path_format_registry_count(void)
{
    return 0u;
}

const QwPathFormatDescriptor *qw_path_format_registry_at(size_t index)
{
    (void)index;
    return NULL;
}

const QwPathFormatDescriptor *qw_format_detect_path(const char *path)
{
    (void)path;
    return NULL;
}

static unsigned qw_adapter_capabilities(const QwFormatDescriptor *format)
{
    unsigned capabilities = 0u;
    if (format->probe) capabilities |= QW_FORMAT_ADAPTER_CAP_CONTENT_PROBE;
    if (format->open_memory) capabilities |= QW_FORMAT_ADAPTER_CAP_MEMORY_OPEN;
    if (format->open_layout) capabilities |= QW_FORMAT_ADAPTER_CAP_LAYOUT_OPEN;
    if (format->open_audio) capabilities |= QW_FORMAT_ADAPTER_CAP_AUDIO_INPUT;
    if (format->open_spreadsheet) capabilities |= QW_FORMAT_ADAPTER_CAP_SPREADSHEET_OPEN;
    if (format->open_subtitle) capabilities |= QW_FORMAT_ADAPTER_CAP_SUBTITLE_OPEN;
    if (format->open_data) capabilities |= QW_FORMAT_ADAPTER_CAP_DATA_OPEN;
    if (format->convert_to_pdf) capabilities |= QW_FORMAT_ADAPTER_CAP_PDF_TRANSFORM;
    return capabilities;
}

int qw_format_metadata_get(QwInputFormat id, QwFormatMetadata *out_metadata)
{
    const QwFormatDescriptor *format;
    if (!out_metadata) return 0;
    memset(out_metadata, 0, sizeof(*out_metadata));
    format = qw_format_find(id);
    if (!format) return 0;
    out_metadata->media_kind = QW_MEDIA_KIND_FORMAT_DOCUMENT;
    if (format->open_layout) {
        out_metadata->media_kind = QW_MEDIA_KIND_LAYOUT_DOCUMENT;
        out_metadata->document_kinds |= QW_MEDIA_KIND_BIT(QW_MEDIA_KIND_LAYOUT_DOCUMENT);
    }
    out_metadata->format_capabilities = format->capabilities;
    out_metadata->adapter_capabilities = qw_adapter_capabilities(format);
    return 1;
}

int qw_format_supports(QwInputFormat id, unsigned capability)
{
    const QwFormatDescriptor *format = qw_format_find(id);
    if (!format) return 0;
    return (format->capabilities & capability) ? 1 : 0;
}

int qw_format_output_supports(QwOutputFormat id, unsigned capability)
{
    /* The document package exposes no audio-style output adapters. */
    (void)id;
    (void)capability;
    return 0;
}

size_t qw_format_max_bytes(QwInputFormat id)
{
    const QwFormatDescriptor *format = qw_format_find(id);
    return format ? format->max_input_bytes : 0u;
}

int qw_format_size_within_limit(QwInputFormat id, size_t input_bytes)
{
    const QwFormatDescriptor *format = qw_format_find(id);
    if (!format) return 0;
    if (!format->max_input_bytes) return 0;
    return input_bytes <= format->max_input_bytes ? 1 : 0;
}

/* Stable storage for the default policy: the public macro expands to a compound
 * literal, whose address must not escape the function that created it. */
static const QwFormatInputResourceProfile qw_default_resource_profile = {
    QW_INPUT_RESOURCE_MAX_ARCHIVE_INPUT_BYTES,
    QW_INPUT_RESOURCE_MAX_ARCHIVE_ENTRIES,
    QW_INPUT_RESOURCE_MAX_ARCHIVE_ENTRY_BYTES,
    QW_INPUT_RESOURCE_MAX_ARCHIVE_TOTAL_BYTES,
    QW_INPUT_RESOURCE_MAX_IMAGE_DIMENSION,
    QW_INPUT_RESOURCE_MAX_DECODED_PIXELS,
    QW_INPUT_BACKEND_POLICY_FORMAT_METADATA,
    QW_INPUT_LIFECYCLE_TYPED_DOCUMENT_DISPOSE
};

const QwFormatInputResourceProfile *qw_format_resource_profile_get(QwInputFormat id)
{
    const QwFormatDescriptor *format = qw_format_find(id);
    if (format && format->resource_profile) return format->resource_profile;
    if (format) return &qw_default_resource_profile;
    return NULL;
}

int qw_format_registry_validate(char *error, size_t error_capacity)
{
    size_t outer;
    if (error && error_capacity) error[0] = '\0';
    if (QW_FORMAT_REGISTRY_COUNT == 0u) {
        if (error && error_capacity) {
            snprintf(error, error_capacity, "format registry is empty");
        }
        return 0;
    }
    for (outer = 0u; outer < QW_FORMAT_REGISTRY_COUNT; ++outer) {
        const QwFormatDescriptor *format = qw_format_registry[outer];
        size_t inner;
        if (!format->name || !format->name[0]) {
            if (error && error_capacity) {
                snprintf(error, error_capacity, "format %zu has no name", outer);
            }
            return 0;
        }
        if (!format->max_input_bytes) {
            if (error && error_capacity) {
                snprintf(error, error_capacity, "%s has no input byte limit",
                         format->name);
            }
            return 0;
        }
        for (inner = outer + 1u; inner < QW_FORMAT_REGISTRY_COUNT; ++inner) {
            if (qw_format_registry[inner]->id == format->id) {
                if (error && error_capacity) {
                    snprintf(error, error_capacity, "%s is registered twice",
                             format->name);
                }
                return 0;
            }
        }
    }
    return 1;
}

int qw_format_detect_source(const struct QwSource *source,
                            QwFormatDetection *out_detection)
{
    (void)source;
    if (out_detection) memset(out_detection, 0, sizeof(*out_detection));
    /* Source-based detection needs a path context supplied by the caller. */
    return 0;
}

int qw_format_detection_from_descriptor(const QwFormatDescriptor *format,
                                        const QwPathFormatDescriptor *path,
                                        unsigned priority,
                                        QwFormatDetection *out_detection)
{
    if (!out_detection) return 0;
    memset(out_detection, 0, sizeof(*out_detection));
    if (format) {
        out_detection->input = *format;
        out_detection->has_input = 1;
    }
    out_detection->path_format = path;
    out_detection->requires_path = (path && !format) ? 1 : 0;
    out_detection->priority = priority;
    return (format || path) ? 1 : 0;
}

/* Native text-only PDF route used by simple packaged text formats. */
int qw_native_text_pdf_from_utf8(const uint8_t *text, size_t text_size,
                                uint8_t **out_pdf, size_t *out_size)
{
    size_t capacity;
    size_t position = 0u;
    char *html;
    int rc;

    if (!out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    if (!text && text_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (text_size > ((size_t)64u * 1024u * 1024u)) return QW_STATUS_ERR_RESOURCE_LIMIT;

    /* Two bytes per input byte for "&#NN;" expansions plus paragraph markup. */
    capacity = text_size * 8u + 256u;
    html = (char *)malloc(capacity);
    if (!html) return QW_STATUS_ERR_OUT_OF_MEMORY;
    memcpy(html, "<html><body>", 12u);
    position = 12u;
    {
        size_t index = 0u;
        int line_open = 0;
        while (index < text_size) {
            unsigned char value = text[index];
            if (value == '\n') {
                if (line_open) {
                    memcpy(html + position, "</p>", 4u);
                    position += 4u;
                    line_open = 0;
                }
                ++index;
                continue;
            }
            if (!line_open) {
                memcpy(html + position, "<p>", 3u);
                position += 3u;
                line_open = 1;
            }
            if (value == '&') {
                memcpy(html + position, "&amp;", 5u);
                position += 5u;
            } else if (value == '<') {
                memcpy(html + position, "&lt;", 4u);
                position += 4u;
            } else if (value == '>') {
                memcpy(html + position, "&gt;", 4u);
                position += 4u;
            } else {
                html[position++] = (char)value;
            }
            ++index;
            if (position + 8u >= capacity) {
                char *grown;
                capacity *= 2u;
                grown = (char *)realloc(html, capacity);
                if (!grown) {
                    free(html);
                    return QW_STATUS_ERR_OUT_OF_MEMORY;
                }
                html = grown;
            }
        }
        if (line_open) {
            memcpy(html + position, "</p>", 4u);
            position += 4u;
        }
    }
    memcpy(html + position, "</body></html>", 14u);
    position += 14u;
    rc = qw_mupdf_convert_html(html, position, out_pdf, out_size, NULL);
    free(html);
    return rc;
}
