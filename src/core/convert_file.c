#include <quirewise/convert.h>
#include <quirewise/pdf.h>

#include "format_registry.h"
#include "../backends/document_backend.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <io.h>
#  include <wchar.h>
#endif

/* A single in-memory conversion; the release path keeps inputs bounded. */
#define QW_CONVERT_FILE_MAX_INPUT (512u * 1024u * 1024u)

/* ------------------------------------------------------------------ */
/* UTF-8 aware file helpers                                            */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)

static wchar_t *qw_utf8_to_wide(const char *text)
{
    int length;
    wchar_t *wide;
    if (!text) return NULL;
    length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (length <= 0) return NULL;
    wide = (wchar_t *)malloc((size_t)length * sizeof(wchar_t));
    if (!wide) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide,
                            length) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static void qw_ascii_to_wide(const char *text, wchar_t *out, size_t capacity)
{
    size_t index = 0u;
    if (!out || capacity == 0u) return;
    while (text && text[index] && index + 1u < capacity) {
        out[index] = (wchar_t)(unsigned char)text[index];
        ++index;
    }
    out[index] = L'\0';
}

static FILE *qw_open_utf8(const char *path, const char *mode)
{
    wchar_t *wide_path = qw_utf8_to_wide(path);
    wchar_t wide_mode[8];
    FILE *file;
    if (!wide_path) return NULL;
    qw_ascii_to_wide(mode, wide_mode, sizeof(wide_mode) / sizeof(wide_mode[0]));
    file = _wfopen(wide_path, wide_mode);
    free(wide_path);
    return file;
}

static int qw_path_exists(const char *path)
{
    wchar_t *wide_path = qw_utf8_to_wide(path);
    int exists;
    if (!wide_path) return 0;
    exists = _waccess(wide_path, 0) == 0;
    free(wide_path);
    return exists;
}

static void qw_remove_path(const char *path)
{
    wchar_t *wide_path = qw_utf8_to_wide(path);
    if (!wide_path) return;
    _wremove(wide_path);
    free(wide_path);
}

#else

static FILE *qw_open_utf8(const char *path, const char *mode)
{
    return fopen(path, mode);
}

static int qw_path_exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}

static void qw_remove_path(const char *path)
{
    remove(path);
}

#endif

/* Lowercase file extension without the dot; empty when there is none. */
static void qw_path_extension(const char *path, char *out, size_t capacity)
{
    const char *base;
    const char *dot;
    size_t length = 0u;
    size_t index;
    if (!out || capacity == 0u) return;
    out[0] = '\0';
    if (!path) return;
    base = path;
    for (index = 0u; path[index]; ++index) {
        if (path[index] == '/' || path[index] == '\\') base = path + index + 1;
    }
    dot = strrchr(base, '.');
    if (!dot || dot == base) return;
    ++dot;
    length = strlen(dot);
    if (length >= capacity) length = capacity - 1u;
    for (index = 0u; index < length; ++index) {
        char ch = dot[index];
        out[index] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    }
    out[length] = '\0';
}

static int qw_is_audio_format(QwInputFormat format)
{
    switch (format) {
    case QW_FORMAT_NCM:
    case QW_FORMAT_QMC:
    case QW_FORMAT_KGM:
    case QW_FORMAT_KWM:
    case QW_FORMAT_MP3:
    case QW_FORMAT_FLAC:
    case QW_FORMAT_M4A:
    case QW_FORMAT_WAV:
    case QW_FORMAT_OGG:
    case QW_FORMAT_AAC:
    case QW_FORMAT_APE:
    case QW_FORMAT_WMA:
    case QW_FORMAT_AIFF:
    case QW_FORMAT_CAF:
    case QW_FORMAT_MATROSKA:
    case QW_FORMAT_AC3:
    case QW_FORMAT_DTS:
    case QW_FORMAT_WAVPACK:
    case QW_FORMAT_TTA:
    case QW_FORMAT_TAK:
    case QW_FORMAT_MPC:
    case QW_FORMAT_ALAC:
    case QW_FORMAT_OPUS:
    case QW_FORMAT_WEBM:
    case QW_FORMAT_OGG_FLAC:
    case QW_FORMAT_MP2:
    case QW_FORMAT_EAC3:
    case QW_FORMAT_SPEEX:
    case QW_FORMAT_AMR:
    case QW_FORMAT_AU:
    case QW_FORMAT_VOC:
    case QW_FORMAT_W64:
    case QW_FORMAT_DSF:
    case QW_FORMAT_DFF:
    case QW_FORMAT_DSS:
        return 1;
    default:
        return 0;
    }
}

static int qw_read_input_file(const char *path, uint8_t **out_data,
                              size_t *out_size, QwConvertReport *report)
{
    FILE *file;
    long length;
    uint8_t *data;

    if (!path || !out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    file = qw_open_utf8(path, "rb");
    if (!file) {
        if (report)
            snprintf(report->note, sizeof(report->note),
                     "cannot open input file");
        return QW_STATUS_ERR_IO;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        if (report)
            snprintf(report->note, sizeof(report->note),
                     "cannot determine input size");
        return QW_STATUS_ERR_IO;
    }
    if ((unsigned long)length > (unsigned long)QW_CONVERT_FILE_MAX_INPUT) {
        fclose(file);
        if (report)
            snprintf(report->note, sizeof(report->note),
                     "input exceeds the 512 MiB conversion limit");
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    data = (uint8_t *)malloc((size_t)length ? (size_t)length : 1u);
    if (!data) {
        fclose(file);
        if (report)
            snprintf(report->note, sizeof(report->note),
                     "out of memory while reading input");
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    if (length > 0 &&
        fread(data, 1u, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        if (report)
            snprintf(report->note, sizeof(report->note),
                     "short read from input file");
        return QW_STATUS_ERR_IO;
    }
    fclose(file);
    *out_data = data;
    *out_size = (size_t)length;
    return QW_STATUS_OK;
}

static int qw_write_output_file(const char *path, const uint8_t *data,
                                size_t size, QwConvertReport *report)
{
    FILE *file;
    if (!path) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (qw_path_exists(path)) {
        if (report)
            snprintf(report->note, sizeof(report->note),
                     "output already exists and is never overwritten");
        return QW_STATUS_ERR_IO;
    }
    file = qw_open_utf8(path, "wb");
    if (!file) {
        if (report)
            snprintf(report->note, sizeof(report->note),
                     "cannot create output file");
        return QW_STATUS_ERR_IO;
    }
    if (size > 0 && fwrite(data, 1u, size, file) != size) {
        fclose(file);
        qw_remove_path(path);
        if (report)
            snprintf(report->note, sizeof(report->note),
                     "short write to output file");
        return QW_STATUS_ERR_IO;
    }
    if (fclose(file) != 0) {
        qw_remove_path(path);
        if (report)
            snprintf(report->note, sizeof(report->note),
                     "cannot flush output file");
        return QW_STATUS_ERR_IO;
    }
    return QW_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Public file API                                                     */
/* ------------------------------------------------------------------ */

/* Lightweight content sniffing for the public detection API. The registry
 * only carries format descriptors that expose their own probe; PDF and the
 * common image/audio magics are covered here. */
static int qw_region_equals(const uint8_t *data, size_t size, size_t offset,
                            const char *text)
{
    size_t length = strlen(text);
    if (!data || offset + length > size) return 0;
    return memcmp(data + offset, text, length) == 0;
}

static int qw_contains_text(const uint8_t *data, size_t size, const char *text)
{
    size_t length = strlen(text);
    size_t index;
    if (!data || !length || size < length) return 0;
    for (index = 0u; index + length <= size; ++index) {
        if (memcmp(data + index, text, length) == 0) return 1;
    }
    return 0;
}

static const uint8_t *qw_skip_preamble(const uint8_t *data, size_t size)
{
    size_t index = 0u;
    if (size >= 3u && data[0] == 0xEFu && data[1] == 0xBBu &&
        data[2] == 0xBFu) {
        index = 3u;
    }
    while (index < size && (data[index] == ' ' || data[index] == '\t' ||
                            data[index] == '\r' || data[index] == '\n')) {
        ++index;
    }
    return data + index;
}

QwInputFormat qw_detect_format(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    const uint8_t *first;
    const QwFormatDescriptor *descriptor;

    if (!bytes || !size) return QW_FORMAT_UNKNOWN;
    if (qw_region_equals(bytes, size, 0u, "%PDF-")) return QW_FORMAT_PDF;
    if (qw_region_equals(bytes, size, 0u, "AT&TFORM")) return QW_FORMAT_DJVU;
    if (qw_region_equals(bytes, size, 0u, "\x89PNG\r\n\x1a\n"))
        return QW_FORMAT_PNG;
    if (size >= 2u && bytes[0] == 0xFFu && bytes[1] == 0xD8u)
        return QW_FORMAT_JPG;
    if (qw_region_equals(bytes, size, 0u, "GIF87a") ||
        qw_region_equals(bytes, size, 0u, "GIF89a"))
        return QW_FORMAT_GIF;
    if (qw_region_equals(bytes, size, 0u, "BM")) return QW_FORMAT_BMP;
    if (qw_region_equals(bytes, size, 0u, "fLaC")) return QW_FORMAT_FLAC;
    if (qw_region_equals(bytes, size, 0u, "OggS")) return QW_FORMAT_OGG;
    if (qw_region_equals(bytes, size, 0u, "caff")) return QW_FORMAT_CAF;
    if (qw_region_equals(bytes, size, 0u, "ID3")) return QW_FORMAT_MP3;
    if (qw_region_equals(bytes, size, 0u, "RIFF")) {
        if (qw_region_equals(bytes, size, 8u, "WAVE")) return QW_FORMAT_WAV;
        if (qw_region_equals(bytes, size, 8u, "WEBP")) return QW_FORMAT_WEBP;
    }
    if (qw_region_equals(bytes, size, 0u, "FORM")) {
        if (qw_region_equals(bytes, size, 8u, "AIFF") ||
            qw_region_equals(bytes, size, 8u, "AIFC"))
            return QW_FORMAT_AIFF;
    }
    if (size >= 4u && bytes[0] == 0xFFu &&
        (bytes[1] == 0xFBu || bytes[1] == 0xFAu || bytes[1] == 0xF3u ||
         bytes[1] == 0xF2u))
        return QW_FORMAT_MP3;
    if (qw_region_equals(bytes, size, 0u, "PK\x03\x04")) {
        if (qw_contains_text(bytes, size, "application/epub+zip"))
            return QW_FORMAT_EPUB;
        descriptor = qw_format_detect(bytes, size);
        return descriptor ? descriptor->id : QW_FORMAT_UNKNOWN;
    }
    first = qw_skip_preamble(bytes, size);
    if (first < bytes + size && first[0] == '<') {
        if (qw_contains_text(bytes, size, "<FictionBook"))
            return QW_FORMAT_FB2;
        if (qw_contains_text(bytes, size, "office:document"))
            return QW_FORMAT_ODT;
        if (qw_contains_text(bytes, size, "<html") ||
            qw_contains_text(bytes, size, "<body") ||
            qw_contains_text(bytes, size, "<p") ||
            qw_contains_text(bytes, size, "<h1"))
            return QW_FORMAT_HTML;
    }
    descriptor = qw_format_detect(bytes, size);
    return descriptor ? descriptor->id : QW_FORMAT_UNKNOWN;
}

const char *qw_format_name(QwInputFormat format)
{
    switch (format) {
    case QW_FORMAT_PDF: return "PDF";
    case QW_FORMAT_TXT: return "TXT";
    case QW_FORMAT_HTML: return "HTML";
    case QW_FORMAT_EPUB: return "EPUB";
    case QW_FORMAT_FB2: return "FB2";
    case QW_FORMAT_DOCX: return "DOCX";
    case QW_FORMAT_ODT: return "ODT";
    case QW_FORMAT_DJVU: return "DjVu";
    case QW_FORMAT_PNG: return "PNG";
    case QW_FORMAT_JPG: return "JPEG";
    case QW_FORMAT_GIF: return "GIF";
    case QW_FORMAT_BMP: return "BMP";
    case QW_FORMAT_WEBP: return "WebP";
    case QW_FORMAT_WAV: return "WAV";
    case QW_FORMAT_FLAC: return "FLAC";
    case QW_FORMAT_MP3: return "MP3";
    case QW_FORMAT_OGG: return "Ogg";
    case QW_FORMAT_AIFF: return "AIFF";
    case QW_FORMAT_CAF: return "CAF";
    case QW_FORMAT_M4A: return "M4A";
    case QW_FORMAT_CSV: return "CSV";
    case QW_FORMAT_JSON: return "JSON";
    default: break;
    }
    {
        const QwFormatDescriptor *descriptor = qw_format_find(format);
        if (descriptor && descriptor->name) return descriptor->name;
    }
    return "unknown";
}

int qw_convert_file_to_format(const char *input_path,
                              const char *output_path,
                              QwOutputFormat output_format,
                              const QwConvertOptions *options,
                              QwConvertReport *report)
{
    QwConvertOptions defaults = QW_CONVERT_OPTIONS_INIT;
    QwConvertReport local_report;
    uint8_t *data = NULL;
    size_t size = 0u;
    uint8_t *output = NULL;
    size_t output_size = 0u;
    char extension[16];
    int reflow = 0;
    int rc;

    if (!input_path || !output_path) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (!options) options = &defaults;
    if (!report) {
        memset(&local_report, 0, sizeof(local_report));
        report = &local_report;
    }
    memset(&local_report, 0, sizeof(local_report));

    rc = qw_read_input_file(input_path, &data, &size, report);
    if (rc != QW_STATUS_OK) return rc;

    qw_path_extension(input_path, extension, sizeof(extension));
    if (strcmp(extension, "epub") == 0) reflow = 1;

    if (output_format == QW_FORMAT_PDF) {
        rc = qw_doc_convert_memory(data, size, extension, reflow, &output,
                                   &output_size, report);
    } else {
        rc = qw_convert_to_format_memory(data, size, output_format, options,
                                         &output, &output_size, report);
    }
    free(data);
    if (rc != QW_STATUS_OK) {
        free(output);
        return rc;
    }
    rc = qw_write_output_file(output_path, output, output_size, report);
    qw_buffer_free(output);
    return rc;
}

int qw_convert_file(const char *input_path,
                    const char *output_path,
                    const QwConvertOptions *options,
                    QwConvertReport *report)
{
    return qw_convert_file_to_format(input_path, output_path, QW_FORMAT_PDF,
                                     options, report);
}

int qw_check_file(const char *input_path,
                  const QwConvertOptions *options,
                  QwConvertReport *report)
{
    QwConvertOptions defaults = QW_CONVERT_OPTIONS_INIT;
    QwConvertReport local_report;
    uint8_t *data = NULL;
    size_t size = 0u;
    const QwFormatDescriptor *descriptor;
    QwInputFormat format;
    char extension[16];
    int reflow = 0;
    int rc;

    if (!input_path) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (!options) options = &defaults;
    (void)options;
    if (!report) {
        memset(&local_report, 0, sizeof(local_report));
        report = &local_report;
    }
    memset(&local_report, 0, sizeof(local_report));

    rc = qw_read_input_file(input_path, &data, &size, report);
    if (rc != QW_STATUS_OK) return rc;

    descriptor = qw_format_detect(data, size);
    format = descriptor ? descriptor->id : QW_FORMAT_UNKNOWN;
    report->format = format;
    qw_path_extension(input_path, extension, sizeof(extension));
    if (strcmp(extension, "epub") == 0) reflow = 1;

    if (format == QW_FORMAT_PDF) {
        QwPdfDocument *document = NULL;
        size_t pages = 0u;
        size_t page;
        rc = qw_pdf_open(data, size, &document, &pages, report);
        if (rc == QW_STATUS_OK) {
            for (page = 0u; page < pages; ++page) {
                QwPageImage image;
                memset(&image, 0, sizeof(image));
                rc = qw_pdf_render_page(document, page, 72u, &image, report);
                qw_pdf_page_free(&image);
                if (rc != QW_STATUS_OK) break;
            }
            report->pages_written = pages;
            qw_pdf_close(document);
        }
    } else if (qw_is_audio_format(format)) {
        if (report)
            snprintf(report->note, sizeof(report->note),
                     "audio check is parser-only and not exposed by the "
                     "document package yet");
        rc = QW_STATUS_ERR_UNSUPPORTED;
    } else {
        uint8_t *output = NULL;
        size_t output_size = 0u;
        rc = qw_doc_convert_memory(data, size, extension, reflow, &output,
                                   &output_size, report);
        free(output);
        if (report) report->output_size = output_size;
    }
    free(data);
    return rc;
}
