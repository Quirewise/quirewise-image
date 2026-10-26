#include "image_internal.h"

#include "quirewise/status.h"

#include <errno.h>
#include <float.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PFM_MAX_PIXELS 20000000u

typedef struct QwPfmHeader {
    uint32_t width;
    uint32_t height;
    size_t data_offset;
    size_t pixel_count;
    size_t channels;
    size_t data_size;
    float scale;
    int little_endian;
} QwPfmHeader;

static void pfm_skip_space_and_comments(const uint8_t *data,
                                        size_t size,
                                        size_t *position)
{
    for (;;) {
        while (*position < size && qw_ascii_isspace(data[*position])) {
            ++*position;
        }
        if (*position >= size || data[*position] != '#') return;
        while (*position < size && data[*position] != '\n' &&
               data[*position] != '\r') {
            ++*position;
        }
    }
}

static int pfm_read_token(const uint8_t *data,
                          size_t size,
                          size_t *position,
                          char *token,
                          size_t token_capacity)
{
    size_t start;
    size_t length;
    pfm_skip_space_and_comments(data, size, position);
    start = *position;
    while (*position < size && !qw_ascii_isspace(data[*position]) &&
           data[*position] != '#') {
        ++*position;
    }
    length = *position - start;
    if (!length || length >= token_capacity) return 0;
    memcpy(token, data + start, length);
    token[length] = '\0';
    return 1;
}

static int pfm_parse_unsigned(const char *text, uint32_t *out_value)
{
    uint64_t value = 0u;
    if (!text || !*text || !out_value) return 0;
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return 0;
        value = value * 10u + (*cursor - '0');
        if (value > UINT32_MAX) return 0;
    }
    if (!value) return 0;
    *out_value = (uint32_t)value;
    return 1;
}

static int pfm_parse_header(const void *data,
                            size_t size,
                            QwPfmHeader *header)
{
    const uint8_t *bytes = (const uint8_t *)data;
    char magic[8];
    char width_text[32];
    char height_text[32];
    char scale_text[64];
    char *scale_end = NULL;
    size_t position = 0u;
    size_t float_count;
    float scale;
    uint32_t scale_bits;

    if (!bytes || !header || size < 8u || sizeof(float) != sizeof(uint32_t) ||
        FLT_RADIX != 2 || FLT_MANT_DIG != 24 || FLT_MAX_EXP != 128 ||
        !pfm_read_token(bytes, size, &position, magic,
                                          sizeof(magic)) ||
        (strcmp(magic, "PF") != 0 && strcmp(magic, "Pf") != 0) ||
        !pfm_read_token(bytes, size, &position, width_text,
                        sizeof(width_text)) ||
        !pfm_read_token(bytes, size, &position, height_text,
                        sizeof(height_text)) ||
        !pfm_read_token(bytes, size, &position, scale_text,
                        sizeof(scale_text))) {
        return 0;
    }
    errno = 0;
    scale = strtof(scale_text, &scale_end);
    if (!scale_end || *scale_end || errno == ERANGE || scale == 0.0f ||
        (scale_text[0] == '\0')) {
        return 0;
    }
    memcpy(&scale_bits, &scale, sizeof(scale_bits));
    if ((scale_bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000)) {
        return 0;
    }
    if (position >= size || !qw_ascii_isspace(bytes[position])) return 0;
    if (bytes[position] == '\r' && position + 1u < size &&
        bytes[position + 1u] == '\n') {
        position += 2u;
    } else {
        ++position;
    }
    if (!pfm_parse_unsigned(width_text, &header->width) ||
        !pfm_parse_unsigned(height_text, &header->height)) {
        return 0;
    }
    header->channels = strcmp(magic, "PF") == 0 ? 3u : 1u;
    if ((size_t)header->width > SIZE_MAX / header->height) return 0;
    header->pixel_count = (size_t)header->width * header->height;
    if (!header->pixel_count || header->pixel_count > PFM_MAX_PIXELS ||
        header->pixel_count > SIZE_MAX / header->channels) {
        return 0;
    }
    float_count = header->pixel_count * header->channels;
    if (float_count > SIZE_MAX / sizeof(uint32_t)) return 0;
    header->data_size = float_count * sizeof(uint32_t);
    if (position > size || header->data_size != size - position) return 0;
    header->data_offset = position;
    header->scale = scale < 0.0f ? -scale : scale;
    header->little_endian = scale < 0.0f;
    return 1;
}

int qw_pfm_probe(const void *data, size_t size)
{
    QwPfmHeader header;
    return pfm_parse_header(data, size, &header);
}

static uint32_t pfm_read_u32(const uint8_t *data, int little_endian)
{
    return little_endian ? qw_read_u32_le(data) : qw_read_u32_be(data);
}

static int pfm_sample_to_u8(const uint8_t *data,
                            const QwPfmHeader *header,
                            uint8_t *out_sample)
{
    uint32_t bits = pfm_read_u32(data, header->little_endian);
    float sample;
    float scaled;
    if ((bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000)) {
        return 0;
    }
    memcpy(&sample, &bits, sizeof(sample));
    scaled = sample * header->scale;
    if (!(scaled > 0.0f)) {
        *out_sample = 0u;
    } else if (scaled >= 1.0f) {
        *out_sample = 255u;
    } else {
        *out_sample = (uint8_t)(scaled * 255.0f + 0.5f);
    }
    return 1;
}

int qw_pfm_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwPfmHeader header;
    uint8_t *pixels = NULL;
    size_t output_size;
    size_t output_channels;

    if (!out_page || !pfm_parse_header(data, size, &header)) {
        return QW_STATUS_ERR_FORMAT;
    }
    memset(out_page, 0, sizeof(*out_page));
    output_channels = header.channels;
    if (header.pixel_count > SIZE_MAX / output_channels) {
        return QW_STATUS_ERR_FORMAT;
    }
    output_size = header.pixel_count * output_channels;
    pixels = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!pixels) return QW_STATUS_ERR_OUT_OF_MEMORY;
    for (uint32_t y = 0u; y < header.height; ++y) {
        uint32_t source_y = header.height - 1u - y;
        for (uint32_t x = 0u; x < header.width; ++x) {
            for (size_t channel = 0u; channel < header.channels; ++channel) {
                size_t source_index =
                    ((size_t)source_y * header.width + x) * header.channels +
                    channel;
                size_t output_index =
                    ((size_t)y * header.width + x) * output_channels + channel;
                const uint8_t *sample = bytes + header.data_offset +
                    source_index * sizeof(uint32_t);
                if (!pfm_sample_to_u8(sample, &header,
                                      &pixels[output_index])) {
                    free(pixels);
                    return QW_STATUS_ERR_FORMAT;
                }
            }
        }
    }
    out_page->data = pixels;
    out_page->size = output_size;
    out_page->kind = header.channels == 1u
        ? QW_PAGE_IMAGE_GRAY8 : QW_PAGE_IMAGE_RGB24;
    out_page->width = header.width;
    out_page->height = header.height;
    return QW_STATUS_OK;
}
