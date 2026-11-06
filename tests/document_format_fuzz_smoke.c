#include <quirewise/docx.h>
#include <quirewise/email.h>
#include <quirewise/hwpx.h>
#include <quirewise/odf_presentation.h>
#include <quirewise/odt.h>
#include <quirewise/pptx.h>
#include <quirewise/xml.h>
#include <quirewise/xps.h>

#include "document_seeds.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FUZZ_SEED UINT32_C(0x51f0ee11)
#define FUZZ_RUNS 120u

static uint32_t rng_state = FUZZ_SEED;

static uint32_t next_random(void)
{
    uint32_t value = rng_state;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    rng_state = value;
    return value;
}

static size_t mutate(const unsigned char *seed, size_t seed_size,
                     unsigned char *buffer, size_t capacity)
{
    size_t length = seed_size > capacity ? capacity : seed_size;
    unsigned int flips = 1u + next_random() % 4u;
    size_t index;
    memcpy(buffer, seed, length);
    if (next_random() % 8u == 0u && length > 1u)
        length = 1u + next_random() % length;
    for (index = 0u; index < flips && length; ++index) {
        size_t position = next_random() % length;
        buffer[position] = (unsigned char)(buffer[position] ^
                                           (unsigned char)(1u << (next_random() % 8u)));
    }
    return length;
}

int main(void)
{
    static const char email_seed[] =
        "From: a@example.com\r\nTo: b@example.com\r\nSubject: Fuzz\r\n"
        "MIME-Version: 1.0\r\nContent-Type: text/plain\r\n\r\nHello fuzz.\r\n";
    static const char fodt_seed[] =
        "<?xml version=\"1.0\"?><office:document "
        "xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
        "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
        "office:version=\"1.3\"><office:body><office:text><text:p>Hello</text:p>"
        "</office:text></office:body></office:document>";
    static const char xml_seed[] =
        "<?xml version=\"1.0\"?><root id=\"r\"><child>text</child></root>";
    unsigned char buffer[8192];
    size_t run;
    size_t ok = 0u;
    size_t rejected = 0u;
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        QwOdtDocument *document = NULL;
        size_t size = mutate(odt_seed, sizeof(odt_seed), buffer,
                             sizeof(buffer));
        if (qw_odt_parse(QW_ODT_FORMAT_ODT, buffer, size, &document) ==
                QW_ODT_OK &&
            document) {
            ++ok;
            qw_odt_document_free(document);
        } else {
            ++rejected;
        }
    }
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        QwDocxDocument *document = NULL;
        size_t size = mutate(docx_seed, sizeof(docx_seed), buffer,
                             sizeof(buffer));
        if (qw_docx_parse(buffer, size, &document) == QW_DOCX_OK && document) {
            ++ok;
            qw_docx_document_free(document);
        } else {
            ++rejected;
        }
    }
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        QwHwpxDocument *document = NULL;
        size_t size = mutate(hwpx_seed, sizeof(hwpx_seed), buffer,
                             sizeof(buffer));
        if (qw_hwpx_parse(buffer, size, &document) == QW_HWPX_OK && document) {
            ++ok;
            qw_hwpx_document_free(document);
        } else {
            ++rejected;
        }
    }
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        QwOdpDocument *document = NULL;
        size_t size = mutate(odp_seed, sizeof(odp_seed), buffer,
                             sizeof(buffer));
        if (qw_odp_parse(QW_ODP_FORMAT_ODP, buffer, size, &document) ==
                QW_ODP_OK &&
            document) {
            ++ok;
            qw_odp_document_free(document);
        } else {
            ++rejected;
        }
    }
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        QwPptxDocument *document = NULL;
        size_t size = mutate(pptx_seed, sizeof(pptx_seed), buffer,
                             sizeof(buffer));
        if (qw_pptx_parse(QW_PPTX_FORMAT_PPTX, buffer, size, &document) ==
                QW_PPTX_OK &&
            document) {
            ++ok;
            qw_pptx_document_free(document);
        } else {
            ++rejected;
        }
    }
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        QwXpsDocument *document = NULL;
        size_t size = mutate(xps_seed, sizeof(xps_seed), buffer,
                             sizeof(buffer));
        if (qw_xps_document_open(buffer, size, &document) == QW_XPS_OK &&
            document) {
            ++ok;
            qw_xps_document_close(document);
        } else {
            ++rejected;
        }
    }
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        QwEmailDocument *document = NULL;
        size_t size = mutate((const unsigned char *)email_seed,
                             sizeof(email_seed) - 1u, buffer, sizeof(buffer));
        if (qw_email_parse(buffer, size, &document) == QW_EMAIL_OK && document) {
            ++ok;
            qw_email_document_free(document);
        } else {
            ++rejected;
        }
    }
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        QwOdtDocument *document = NULL;
        size_t size = mutate((const unsigned char *)fodt_seed,
                             sizeof(fodt_seed) - 1u, buffer, sizeof(buffer));
        if (qw_odt_parse(QW_ODT_FORMAT_FODT, buffer, size, &document) ==
                QW_ODT_OK &&
            document) {
            ++ok;
            qw_odt_document_free(document);
        } else {
            ++rejected;
        }
    }
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        QwXmlError error;
        QwXmlNode *node;
        size_t size = mutate((const unsigned char *)xml_seed,
                             sizeof(xml_seed) - 1u, buffer, sizeof(buffer));
        node = qw_xml_parse(buffer, size, &error);
        if (node) {
            ++ok;
            qw_xml_drop(node);
        } else {
            ++rejected;
        }
    }
    printf("Document format fuzz smoke: %zu ok, %zu rejected, seed=0x%08x runs=%u\n",
           ok, rejected, FUZZ_SEED, FUZZ_RUNS);
    return 0;
}
