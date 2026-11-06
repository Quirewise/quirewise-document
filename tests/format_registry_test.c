#include "format_registry.h"
#include "quirewise/status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

int main(void)
{
    char error[160];
    QwFormatMetadata metadata;
    QwFormatDetection detection;
    size_t index;
    size_t found = 0u;
    const QwFormatDescriptor *format;

    CHECK(qw_format_registry_count() >= 10u);
    CHECK(qw_format_registry_validate(error, sizeof(error)) == 1);
    CHECK(error[0] == '\0');

    /* Enumeration returns stable, non-null descriptors. */
    for (index = 0u; index < qw_format_registry_count(); ++index) {
        format = qw_format_registry_at(index);
        CHECK(format != NULL);
        CHECK(format->name != NULL && format->name[0] != '\0');
        CHECK(format->max_input_bytes > 0u);
        CHECK(qw_format_find(format->id) == format);
        found += 1u;
    }
    CHECK(found == qw_format_registry_count());
    CHECK(qw_format_registry_at(qw_format_registry_count()) == NULL);

    /* Unknown formats are absent instead of fabricated. */
    CHECK(qw_format_find(QW_FORMAT_PNG) == NULL);
    CHECK(qw_format_find(QW_FORMAT_COUNT) == NULL);

    /* ODT is registered with a layout adapter and a PDF transform. */
    format = qw_format_find(QW_FORMAT_ODT);
    CHECK(format != NULL);
    CHECK(qw_format_metadata_get(QW_FORMAT_ODT, &metadata) == 1);
    CHECK(metadata.media_kind == QW_MEDIA_KIND_LAYOUT_DOCUMENT);
    CHECK((metadata.adapter_capabilities & QW_FORMAT_ADAPTER_CAP_LAYOUT_OPEN) != 0u);
    CHECK(qw_format_supports(QW_FORMAT_ODT, QW_FORMAT_CAP_MULTI_PAGE) == 1);
    CHECK(qw_format_supports(QW_FORMAT_ODT, QW_FORMAT_CAP_PAGE_IMAGES) == 0);
    CHECK(qw_format_max_bytes(QW_FORMAT_ODT) == format->max_input_bytes);
    CHECK(qw_format_size_within_limit(QW_FORMAT_ODT, 1024u) == 1);
    CHECK(qw_format_size_within_limit(QW_FORMAT_ODT, format->max_input_bytes + 1u) == 0);
    CHECK(qw_format_resource_profile_get(QW_FORMAT_ODT) != NULL);
    CHECK(qw_format_resource_profile_get(QW_FORMAT_PNG) == NULL);
    CHECK(qw_format_metadata_get(QW_FORMAT_PNG, &metadata) == 0);
    CHECK(metadata.media_kind == QW_MEDIA_KIND_UNKNOWN);

    /* Detection bookkeeping. */
    memset(&detection, 0, sizeof(detection));
    CHECK(qw_format_detection_from_descriptor(format, NULL,
                                              QW_FORMAT_DETECTION_CONTENT,
                                              &detection) == 1);
    CHECK(detection.has_input == 1 && detection.priority == QW_FORMAT_DETECTION_CONTENT);
    CHECK(detection.input.id == QW_FORMAT_ODT);
    CHECK(qw_format_detection_from_descriptor(NULL, NULL, 0u, &detection) == 0);
    CHECK(detection.has_input == 0 && detection.requires_path == 0);
    {
        const struct QwSource *source = NULL;
        CHECK(qw_format_detect_source(source, &detection) == 0);
    }

    /* No path-format adapters are registered in this package. */
    CHECK(qw_path_format_registry_count() == 0u);
    CHECK(qw_path_format_registry_at(0u) == NULL);
    CHECK(qw_format_detect_path("document.odt") == NULL);

    /* Content detection only reports formats with a working probe. */
    {
        static const char fodt[] =
            "<?xml version=\"1.0\"?><office:document "
            "xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
            "office:mimetype=\"application/vnd.oasis.opendocument.text\">"
            "<office:body><office:text/></office:body></office:document>";
        const QwFormatDescriptor *detected =
            qw_format_detect(fodt, sizeof(fodt) - 1u);
        if (detected) CHECK(detected->id == QW_FORMAT_FODT);
        CHECK(qw_format_detect(NULL, 0u) == NULL);
    }

    /* Native text-only PDF path. */
    {
        static const char text[] = "Quirewise text route\nsecond line\n";
        uint8_t *pdf = NULL;
        size_t pdf_size = 0u;
        CHECK(qw_native_text_pdf_from_utf8((const uint8_t *)text,
                                           sizeof(text) - 1u, &pdf, &pdf_size) ==
              QW_STATUS_OK);
        CHECK(pdf != NULL && pdf_size > 200u);
        CHECK(memcmp(pdf, "%PDF-1.4", 8u) == 0);
        free(pdf);
        CHECK(qw_native_text_pdf_from_utf8(NULL, 0u, NULL, NULL) ==
              QW_STATUS_ERR_INVALID_ARGUMENT);
    }

    puts("PASS: format registry lookup, metadata, limits, detection and text-to-PDF");
    return 0;
}
