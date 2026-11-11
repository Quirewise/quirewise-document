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
