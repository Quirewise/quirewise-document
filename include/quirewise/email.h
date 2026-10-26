#ifndef QUIREWISE_EMAIL_H
#define QUIREWISE_EMAIL_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(QW_EMAIL_BUILD_SHARED)
#    if defined(QW_EMAIL_EXPORTS)
#      define QW_EMAIL_API __declspec(dllexport)
#    else
#      define QW_EMAIL_API __declspec(dllimport)
#    endif
#  else
#    define QW_EMAIL_API
#  endif
#elif defined(QW_EMAIL_BUILD_SHARED)
#  define QW_EMAIL_API __attribute__((visibility("default")))
#else
#  define QW_EMAIL_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct QwEmailDocument QwEmailDocument;

typedef enum QwEmailStatus {
    QW_EMAIL_OK = 0,
    QW_EMAIL_ERROR_INVALID_ARGUMENT = -1,
    QW_EMAIL_ERROR_OUT_OF_MEMORY = -2,
    QW_EMAIL_ERROR_FORMAT = -4,
    QW_EMAIL_ERROR_UNSUPPORTED = -5,
    QW_EMAIL_ERROR_DECODE = -6,
    QW_EMAIL_ERROR_RESOURCE_LIMIT = -11,
    QW_EMAIL_ERROR_UNKNOWN = -100
} QwEmailStatus;

typedef struct QwEmailAttachment {
    const char *name;
    const char *media_type;
    const uint8_t *data;
    size_t size;
} QwEmailAttachment;

/* Returns nonzero when the message headers match the native EML probe. */
QW_EMAIL_API int qw_email_probe(const void *data, size_t size);

/*
 * Parse an EML/MIME message into an owned document. The input is borrowed only
 * for this call. HTML and decoded attachment data remain valid until free.
 */
QW_EMAIL_API QwEmailStatus qw_email_parse(const void *data, size_t size,
                                         QwEmailDocument **out_document);
QW_EMAIL_API const uint8_t *qw_email_document_html(
    const QwEmailDocument *document, size_t *out_size);
QW_EMAIL_API size_t qw_email_document_attachment_count(
    const QwEmailDocument *document);
QW_EMAIL_API QwEmailStatus qw_email_document_get_attachment(
    const QwEmailDocument *document, size_t index,
    QwEmailAttachment *out_attachment);
QW_EMAIL_API void qw_email_document_free(QwEmailDocument *document);

#ifdef __cplusplus
}
#endif

#endif
