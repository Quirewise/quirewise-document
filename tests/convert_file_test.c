#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

static const char *HTML_PATH = "qw_convert_file_test.html";
static const char *PDF_PATH = "qw_convert_file_test.pdf";
static const char *TXT_PATH = "qw_convert_file_test.txt";
static const char *NOTES_PATH = "qw_convert_file_test_notes.txt";
static const char *NOTES_PDF_PATH = "qw_convert_file_test_notes.pdf";
static const char *PNG_PATH = "qw_convert_file_test_image.png";
static const char *PNG_PDF_PATH = "qw_convert_file_test_image.pdf";

static const unsigned char PNG_1X1[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
    0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xFC, 0xCF, 0xC0, 0x50,
    0x0F, 0x00, 0x04, 0x85, 0x01, 0x80, 0x84, 0xA9, 0x8C, 0x21, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
};

static int write_bytes(const char *path, const unsigned char *bytes,
                       size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    if (size && fwrite(bytes, 1u, size, file) != size) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static int write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    if (fputs(text, file) == EOF) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static int file_starts_with(const char *path, const char *prefix)
{
    char buffer[8];
    size_t length = strlen(prefix);
    FILE *file;
    if (length > sizeof(buffer)) return 0;
    file = fopen(path, "rb");
    if (!file) return 0;
    if (fread(buffer, 1u, length, file) != length) {
        fclose(file);
        return 0;
    }
    fclose(file);
    return memcmp(buffer, prefix, length) == 0;
}

static int file_contains(const char *path, const char *needle)
{
    FILE *file = fopen(path, "rb");
    long length;
    char *data;
    int found = 0;
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    data = (char *)malloc((size_t)length + 1u);
    if (!data) {
        fclose(file);
        return 0;
    }
    if (fread(data, 1u, (size_t)length, file) == (size_t)length) {
        data[length] = '\0';
        found = strstr(data, needle) != NULL;
    }
    free(data);
    fclose(file);
    return found;
}

static unsigned char *read_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long length;
    unsigned char *data;
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = (unsigned char *)malloc((size_t)length ? (size_t)length : 1u);
    if (!data) {
        fclose(file);
        return NULL;
    }
    if (length > 0 && fread(data, 1u, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)length;
    return data;
}

static void remove_outputs(void)
{
    remove(HTML_PATH);
    remove(PDF_PATH);
    remove(TXT_PATH);
    remove(NOTES_PATH);
    remove(NOTES_PDF_PATH);
    remove(PNG_PATH);
    remove(PNG_PDF_PATH);
}

int main(void)
{
    static const char html[] =
        "\xEF\xBB\xBF" /* UTF-8 BOM is tolerated */
        "\n<html><body><h1>CLI port</h1><p>File API works.</p></body></html>";
    QwConvertOptions options = QW_CONVERT_OPTIONS_INIT;
    QwConvertReport report;

    remove_outputs();
    CHECK(write_text(HTML_PATH, html));

    memset(&report, 0, sizeof(report));
    CHECK(qw_check_file(HTML_PATH, &options, &report) == QW_STATUS_OK);

    memset(&report, 0, sizeof(report));
    CHECK(qw_convert_file(HTML_PATH, PDF_PATH, &options, &report) ==
          QW_STATUS_OK);
    CHECK(file_starts_with(PDF_PATH, "%PDF-"));

    memset(&report, 0, sizeof(report));
    CHECK(qw_convert_file_to_format(PDF_PATH, TXT_PATH, QW_FORMAT_TXT, &options,
                                    &report) == QW_STATUS_OK);
    CHECK(file_contains(TXT_PATH, "CLI port"));

    /* Plain text (with BOM) is rendered through the paragraph route. */
    CHECK(write_text(NOTES_PATH, "\xEF\xBB\xBFHello\nWorld\n"));
    memset(&report, 0, sizeof(report));
    CHECK(qw_convert_file(NOTES_PATH, NOTES_PDF_PATH, &options, &report) ==
          QW_STATUS_OK);
    CHECK(file_starts_with(NOTES_PDF_PATH, "%PDF-"));

    /* A still image becomes a one-page PDF at pixel size. */
    CHECK(write_bytes(PNG_PATH, PNG_1X1, sizeof(PNG_1X1)));
    memset(&report, 0, sizeof(report));
    CHECK(qw_convert_file(PNG_PATH, PNG_PDF_PATH, &options, &report) ==
          QW_STATUS_OK);
    CHECK(file_starts_with(PNG_PDF_PATH, "%PDF-"));
    memset(&report, 0, sizeof(report));
    CHECK(qw_check_file(PNG_PATH, &options, &report) == QW_STATUS_OK);
    CHECK(qw_detect_format(PNG_1X1, sizeof(PNG_1X1)) == QW_FORMAT_PNG);

    /* Output paths are never overwritten. */
    memset(&report, 0, sizeof(report));
    CHECK(qw_convert_file(HTML_PATH, PDF_PATH, &options, &report) ==
          QW_STATUS_ERR_IO);

    {
        unsigned char *bytes;
        size_t bytes_size = 0u;
        bytes = read_file(PDF_PATH, &bytes_size);
        CHECK(bytes != NULL);
        CHECK(qw_detect_format(bytes, bytes_size) == QW_FORMAT_PDF);
        free(bytes);
        CHECK(qw_detect_format("plain text", 10u) == QW_FORMAT_UNKNOWN);
        CHECK(strcmp(qw_format_name(QW_FORMAT_PDF), "unknown") != 0);
    }

    remove_outputs();
    puts("file conversion API OK");
    return 0;
}
