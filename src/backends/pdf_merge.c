/* Internal PDF merging for the former MuPDF backend interface.
 *
 * The merger copies the object graph of every input document into one new PDF:
 * object numbers are shifted by a per-input offset, indirect references inside
 * dictionaries are renumbered, stream payloads are copied verbatim, and a fresh
 * catalog plus page tree lists every page in input order. A classic
 * cross-reference table is written.
 *
 * Documents whose pages live in compressed object streams (or that use
 * cross-reference streams) are rejected with `QW_STATUS_ERR_UNSUPPORTED`:
 * rebuilding such a graph needs a full xref-stream writer, which this backend
 * does not provide yet. No MuPDF code is involved. */

#include "mupdf_backend.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QW_MERGE_MAX_PAGES 100000u
#define QW_MERGE_MAX_OBJECTS 1000000u
#define QW_MERGE_MAX_INPUT ((size_t)512u * 1024u * 1024u)

typedef struct QwMergeObject {
    size_t input_index;
    size_t number;
    size_t new_number;
    const char *body;
    size_t body_size;
    const char *stream;
    size_t stream_size;
} QwMergeObject;

typedef struct QwMergePage {
    size_t input_index;
    size_t object_number;
} QwMergePage;

typedef struct QwMergeInput {
    const uint8_t *data;
    size_t size;
    size_t object_offset;
    size_t max_object_number;
    size_t page_start;
    size_t page_count;
    size_t info_object;
    size_t outline_object;
    size_t metadata_object;
} QwMergeInput;

static const char *merge_memmem(const char *haystack, size_t haystack_size,
                                const char *needle, size_t needle_size)
{
    size_t index;
    if (!needle_size || haystack_size < needle_size) return NULL;
    for (index = 0u; index + needle_size <= haystack_size; ++index) {
        if (memcmp(haystack + index, needle, needle_size) == 0) {
            return haystack + index;
        }
    }
    return NULL;
}

static int merge_name_is(const char *position, size_t available, const char *name)
{
    size_t length = strlen(name);
    size_t index = 0u;
    while (index < available && (position[index] == ' ' || position[index] == '\t' ||
                                 position[index] == '\r' || position[index] == '\n')) {
        ++index;
    }
    if (index < available && position[index] == '/') ++index;
    if (available - index < length) return 0;
    if (memcmp(position + index, name, length) != 0) return 0;
    if (available - index == length) return 1;
    return !isalnum((unsigned char)position[index + length]);
}

static const char *merge_find_typed(const char *data, size_t size, const char *name)
{
    size_t index = 0u;
    while (index + 5u < size) {
        if (data[index] == '/' && memcmp(data + index, "/Type", 5u) == 0 &&
            merge_name_is(data + index + 5u, size - index - 5u, name)) {
            return data + index;
        }
        ++index;
    }
    return NULL;
}

static int merge_number_before(const char *base, const char *position,
                               size_t *out_number)
{
    const char *cursor = position;
    while (cursor > base) {
        if (cursor - base >= 6 && memcmp(cursor - 6, " 0 obj", 6) == 0) {
            const char *digits_end = cursor - 6;
            const char *digits_start = digits_end;
            size_t value = 0u;
            size_t scale = 1u;
            while (digits_start > base && isdigit((unsigned char)digits_start[-1]) &&
                   digits_end - digits_start < 9) {
                --digits_start;
            }
            if (digits_start == digits_end) return 0;
            while (digits_end > digits_start) {
                --digits_end;
                value += (size_t)(*digits_end - '0') * scale;
                scale *= 10u;
            }
            *out_number = value;
            return 1;
        }
        --cursor;
    }
    return 0;
}


static size_t merge_find_ref(const char *data, size_t size, const char *tag)
{
    size_t tag_size = strlen(tag);
    size_t position = 0u;
    while (position + tag_size < size) {
        const char *found = merge_memmem(data + position, size - position, tag,
                                          tag_size);
        char *end = NULL;
        unsigned long number;
        if (!found) return 0u;
        number = strtoul(found + tag_size, &end, 10);
        if (end != found + tag_size && number > 0u && number < 1000000u)
            return (size_t)number;
        position = (size_t)(found - data) + tag_size;
    }
    return 0u;
}

static int merge_collect_objects(const QwMergeInput *input, size_t input_index,
                                 QwMergeObject **out_objects, size_t *out_count)
{
    const char *data = (const char *)input->data;
    size_t size = input->size;
    size_t position = 0u;
    size_t count = 0u;
    size_t capacity = 0u;
    QwMergeObject *objects = NULL;

    while (position + 6u < size) {
        const char *marker =
            merge_memmem(data + position, size - position, " 0 obj", 6u);
        const char *body;
        const char *endobj;
        const char *stream_payload = NULL;
        size_t stream_size_value = 0u;
        size_t dict_size;
        size_t number;
        size_t offset;
        if (!marker) break;
        {
            /* marker points at the space of "N 0 obj"; parse the digits before it. */
            const char *digits_end = marker;
            const char *digits_start = digits_end;
            size_t value = 0u;
            size_t scale = 1u;
            while (digits_start > data && isdigit((unsigned char)digits_start[-1]) &&
                   digits_end - digits_start < 9) {
                --digits_start;
            }
            if (digits_start == digits_end) {
                position = (size_t)(marker - data) + 6u;
                continue;
            }
            while (digits_end > digits_start) {
                --digits_end;
                value += (size_t)(*digits_end - '0') * scale;
                scale *= 10u;
            }
            number = value;
        }
        body = marker + 6u;
        offset = (size_t)(body - data);
        endobj = merge_memmem(body, size - offset, "endobj", 6u);
        if (!endobj) break;
        dict_size = (size_t)(endobj - body);
        {
            const char *stream = merge_memmem(body, dict_size, "stream", 6u);
            if (stream && dict_size > (size_t)(stream - body) + 7u &&
                !(stream[6] == 'e' && stream[7] == 'n')) {
                const char *payload = stream + 6;
                const char *endstream;
                dict_size = (size_t)(stream - body);
                /* References live in the dictionary only; the payload is copied
                 * verbatim, so it must not be rewritten. */
                while (dict_size && (body[dict_size - 1u] == '\n' ||
                                     body[dict_size - 1u] == '\r' ||
                                     body[dict_size - 1u] == ' ')) {
                    dict_size -= 1u;
                }
                while (payload < endobj && (*payload == '\r' || *payload == '\n')) {
                    ++payload;
                }
                endstream = merge_memmem(payload, (size_t)(endobj - payload),
                                         "endstream", 9u);
                if (!endstream) {
                    free(objects);
                    return 0;
                }
                stream_payload = payload;
                stream_size_value = (size_t)(endstream - payload);
                while (stream_size_value &&
                       (payload[stream_size_value - 1u] == '\n' ||
                        payload[stream_size_value - 1u] == '\r')) {
                    stream_size_value -= 1u;
                }
            }
        }
        if (count >= QW_MERGE_MAX_OBJECTS) {
            free(objects);
            return 0;
        }
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2u : 64u;
            QwMergeObject *resized =
                (QwMergeObject *)realloc(objects, grown * sizeof(*resized));
            if (!resized) {
                free(objects);
                return 0;
            }
            objects = resized;
            capacity = grown;
        }
        objects[count].input_index = input_index;
        objects[count].number = number;
        objects[count].new_number = 0u;
        objects[count].body = body;
        objects[count].body_size = dict_size;
        objects[count].stream = stream_payload;
        objects[count].stream_size = stream_size_value;
        count += 1u;
        position = (size_t)(endobj - data) + 6u;
    }
    *out_objects = objects;
    *out_count = count;
    return count > 0u;
}

static char *merge_rewrite_references(const char *text, size_t size, size_t offset,
                                      size_t new_parent, size_t *out_size)
{
    size_t capacity = size * 3u + 64u;
    char *output = (char *)malloc(capacity);
    size_t position = 0u;
    size_t index = 0u;
    if (!output) return NULL;
    while (index < size) {
        if (isdigit((unsigned char)text[index])) {
            size_t value = 0u;
            while (index < size && isdigit((unsigned char)text[index])) {
                value = value * 10u + (size_t)(text[index] - '0');
                ++index;
            }
            if (index + 2u < size && text[index] == ' ' && text[index + 1u] == '0' &&
                (text[index + 2u] == 'R' ||
                 (index + 3u < size && text[index + 2u] == ' ' && text[index + 3u] == 'R'))) {
                size_t consumed = (text[index + 2u] == 'R') ? 3u : 4u;
                int is_parent = 0;
                size_t look = position;
                while (look > 0u && (output[look - 1u] == ' ' || output[look - 1u] == '\t' ||
                                     output[look - 1u] == '\r' || output[look - 1u] == '\n')) {
                    --look;
                }
                if (look >= 7u && memcmp(output + look - 7u, "/Parent", 7u) == 0) {
                    is_parent = 1;
                }
                position += (size_t)snprintf(output + position, capacity - position,
                                             "%zu 0 R",
                                             is_parent ? new_parent : value + offset);
                index += consumed;
                continue;
            }
            position += (size_t)snprintf(output + position, capacity - position, "%zu",
                                         value);
            continue;
        }
        output[position++] = text[index++];
    }
    *out_size = position;
    return output;
}

int qw_mupdf_merge_pdf_documents(const uint8_t *const *pdfs, const size_t *sizes,
                                 size_t count, uint8_t **out_pdf, size_t *out_size,
                                 size_t *out_pages, QwConvertReport *report)
{
    QwMergeInput *inputs = NULL;
    QwMergeObject **objects = NULL;
    size_t *object_counts = NULL;
    QwMergePage *pages = NULL;
    size_t *xref_offsets = NULL;
    size_t page_total = 0u;
    size_t object_total = 0u;
    size_t running_offset = 0u;
    size_t index;
    size_t input_index;
    size_t catalog_object;
    size_t pages_object;
    int rc = QW_STATUS_OK;
    uint8_t *merged = NULL;
    size_t merged_size = 0u;
    size_t capacity = 0u;

    if (out_pdf) *out_pdf = NULL;
    if (out_size) *out_size = 0u;
    if (out_pages) *out_pages = 0u;
    if (!pdfs || !sizes || !count || !out_pdf || !out_size) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    for (index = 0u; index < count; ++index) {
        if (!pdfs[index] || !sizes[index] || sizes[index] > QW_MERGE_MAX_INPUT) {
            return QW_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (sizes[index] < 8u || memcmp(pdfs[index], "%PDF-", 5u) != 0) {
            return QW_STATUS_ERR_FORMAT;
        }
    }

    inputs = (QwMergeInput *)calloc(count, sizeof(*inputs));
    objects = (QwMergeObject **)calloc(count, sizeof(*objects));
    object_counts = (size_t *)calloc(count, sizeof(*object_counts));
    if (!inputs || !objects || !object_counts) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }

    for (input_index = 0u; input_index < count; ++input_index) {
        const char *data = (const char *)pdfs[input_index];
        size_t size = sizes[input_index];
        size_t pages_found = 0u;
        size_t scan = 0u;
        int has_compressed_objects;
        inputs[input_index].data = pdfs[input_index];
        inputs[input_index].size = size;
        inputs[input_index].page_start = page_total;
        has_compressed_objects =
            merge_find_typed(data, size, "ObjStm") != NULL;
        if (has_compressed_objects) {
            rc = QW_STATUS_ERR_UNSUPPORTED;
            if (report) {
                snprintf(report->note, sizeof(report->note),
                         "PDF merging does not support documents with compressed object streams");
            }
            goto cleanup;
        }
        if (!merge_collect_objects(&inputs[input_index], input_index,
                                   &objects[input_index], &object_counts[input_index])) {
            rc = QW_STATUS_ERR_FORMAT;
            if (report) {
                snprintf(report->note, sizeof(report->note),
                         "Input %zu has no readable objects", input_index);
            }
            goto cleanup;
        }
        while (scan < size) {
            const char *found = merge_find_typed(data + scan, size - scan, "Page");
            size_t object = 0u;
            if (!found) break;
            if (merge_number_before(data, found, &object)) {
                if (page_total + pages_found >= QW_MERGE_MAX_PAGES) {
                    rc = QW_STATUS_ERR_RESOURCE_LIMIT;
                    goto cleanup;
                }
                {
                    QwMergePage *resized = (QwMergePage *)realloc(
                        pages, (page_total + pages_found + 1u) * sizeof(*pages));
                    if (!resized) {
                        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
                        goto cleanup;
                    }
                    pages = resized;
                }
                pages[page_total + pages_found].input_index = input_index;
                pages[page_total + pages_found].object_number = object;
                pages_found += 1u;
            }
            scan = (size_t)(found - data) + 5u;
        }
        if (!pages_found) {
            rc = QW_STATUS_ERR_FORMAT;
            if (report) {
                snprintf(report->note, sizeof(report->note),
                         "Input %zu contains no pages", input_index);
            }
            goto cleanup;
        }
        {
            size_t object;
            size_t largest = 0u;
            for (object = 0u; object < object_counts[input_index]; ++object) {
                if (objects[input_index][object].number > largest) {
                    largest = objects[input_index][object].number;
                }
            }
            inputs[input_index].max_object_number = largest;
            inputs[input_index].object_offset = running_offset;
            inputs[input_index].page_count = pages_found;
            {
                const char *raw = (const char *)inputs[input_index].data;
                size_t raw_size = inputs[input_index].size;
                inputs[input_index].info_object =
                    merge_find_ref(raw, raw_size, "/Info ");
                inputs[input_index].outline_object =
                    merge_find_ref(raw, raw_size, "/Outlines ");
                inputs[input_index].metadata_object =
                    merge_find_ref(raw, raw_size, "/Metadata ");
            }
            running_offset += largest + 2u;
            page_total += pages_found;
        }
        if (running_offset > QW_MERGE_MAX_OBJECTS) {
            rc = QW_STATUS_ERR_RESOURCE_LIMIT;
            goto cleanup;
        }
    }

    object_total = running_offset;
    catalog_object = object_total + 1u;
    pages_object = object_total + 2u;
    object_total += 2u;

    capacity = 1u << 20;
    merged = (uint8_t *)malloc(capacity);
    xref_offsets = (size_t *)calloc(object_total + 1u, sizeof(*xref_offsets));
    if (!merged || !xref_offsets) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }

    {
        size_t position = 0u;
        int header = snprintf((char *)merged, capacity, "%%PDF-1.4\n");
        if (header <= 0) {
            rc = QW_STATUS_ERR_IO;
            goto cleanup;
        }
        position = (size_t)header;
        for (input_index = 0u; input_index < count; ++input_index) {
            size_t object;
            size_t offset = inputs[input_index].object_offset;
            for (object = 0u; object < object_counts[input_index]; ++object) {
                QwMergeObject *entry = &objects[input_index][object];
                char *rewritten;
                size_t rewritten_size;
                size_t needed;
                entry->new_number = entry->number + offset;
                if (entry->new_number == 0u || entry->new_number > object_total - 2u) {
                    rc = QW_STATUS_ERR_FORMAT;
                    goto cleanup;
                }
                rewritten = merge_rewrite_references(entry->body, entry->body_size,
                                                     offset, pages_object,
                                                     &rewritten_size);
                if (!rewritten) {
                    rc = QW_STATUS_ERR_OUT_OF_MEMORY;
                    goto cleanup;
                }
                needed = rewritten_size + entry->stream_size + 64u;
                if (position + needed > capacity) {
                    uint8_t *grown;
                    while (position + needed > capacity) capacity *= 2u;
                    grown = (uint8_t *)realloc(merged, capacity);
                    if (!grown) {
                        free(rewritten);
                        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
                        goto cleanup;
                    }
                    merged = grown;
                }
                xref_offsets[entry->new_number] = position;
                position += (size_t)snprintf((char *)merged + position,
                                             capacity - position, "%zu 0 obj\n",
                                             entry->new_number);
                memcpy(merged + position, rewritten, rewritten_size);
                position += rewritten_size;
                if (entry->stream && entry->stream_size) {
                    memcpy(merged + position, "\nstream\n", 8u);
                    position += 8u;
                    memcpy(merged + position, entry->stream, entry->stream_size);
                    position += entry->stream_size;
                    memcpy(merged + position, "\nendstream\n", 11u);
                    position += 11u;
                }
                memcpy(merged + position, "endobj\n", 7u);
                position += 7u;
                free(rewritten);
            }
        }

        {
            size_t needed = 256u + page_total * 32u;
            if (position + needed > capacity) {
                uint8_t *grown;
                capacity += needed;
                grown = (uint8_t *)realloc(merged, capacity);
                if (!grown) {
                    rc = QW_STATUS_ERR_OUT_OF_MEMORY;
                    goto cleanup;
                }
                merged = grown;
            }
        }
        xref_offsets[catalog_object] = position;
        {
            char extra[160];
            size_t used = 0u;
            size_t outline_ref =
                inputs[0].outline_object
                    ? inputs[0].outline_object + inputs[0].object_offset
                    : 0u;
            size_t metadata_ref =
                inputs[0].metadata_object
                    ? inputs[0].metadata_object + inputs[0].object_offset
                    : 0u;
            extra[0] = '\0';
            if (outline_ref)
                used += (size_t)snprintf(extra + used, sizeof(extra) - used,
                                         " /Outlines %zu 0 R", outline_ref);
            if (metadata_ref && used < sizeof(extra))
                used += (size_t)snprintf(extra + used, sizeof(extra) - used,
                                         " /Metadata %zu 0 R", metadata_ref);
            position += (size_t)snprintf(
                (char *)merged + position, capacity - position,
                "%zu 0 obj\n<< /Type /Catalog /Pages %zu 0 R%s >>\nendobj\n",
                catalog_object, pages_object, extra);
        }
        xref_offsets[pages_object] = position;
        position += (size_t)snprintf((char *)merged + position, capacity - position,
                                     "%zu 0 obj\n<< /Type /Pages /Count %zu /Kids [",
                                     pages_object, page_total);
        {
            size_t item;
            for (item = 0u; item < page_total; ++item) {
                size_t number = pages[item].object_number +
                                inputs[pages[item].input_index].object_offset;
                position += (size_t)snprintf((char *)merged + position,
                                             capacity - position, "%zu 0 R ", number);
            }
        }
        position += (size_t)snprintf((char *)merged + position, capacity - position,
                                     "] >>\nendobj\n");

        {
            size_t number;
            size_t xref_offset = position;
            position += (size_t)snprintf((char *)merged + position, capacity - position,
                                         "xref\n0 %zu\n0000000000 65535 f \n",
                                         object_total + 1u);
            for (number = 1u; number <= object_total; ++number) {
                if (xref_offsets[number]) {
                    position += (size_t)snprintf((char *)merged + position,
                                                 capacity - position,
                                                 "%010zu 00000 n \n",
                                                 xref_offsets[number]);
                } else {
                    position += (size_t)snprintf((char *)merged + position,
                                                 capacity - position,
                                                 "0000000000 65535 f \n");
                }
            }
            {
            char info[64];
            size_t info_ref =
                inputs[0].info_object
                    ? inputs[0].info_object + inputs[0].object_offset
                    : 0u;
            info[0] = '\0';
            if (info_ref)
                snprintf(info, sizeof(info), " /Info %zu 0 R", info_ref);
            position += (size_t)snprintf(
                (char *)merged + position, capacity - position,
                "trailer\n<< /Size %zu /Root %zu 0 R%s >>\nstartxref\n%zu\n%%%%EOF\n",
                object_total + 1u, catalog_object, info, xref_offset);
         }
            merged_size = position;
        }
    }

    *out_pdf = merged;
    *out_size = merged_size;
    if (out_pages) *out_pages = page_total;
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "Merged %zu document(s) into %zu page(s)", count, page_total);
    }
    merged = NULL;
    rc = QW_STATUS_OK;

cleanup:
    if (objects) {
        for (input_index = 0u; input_index < count; ++input_index) {
            free(objects[input_index]);
        }
    }
    free(objects);
    free(object_counts);
    free(inputs);
    free(pages);
    free(xref_offsets);
    free(merged);
    return rc;
}
