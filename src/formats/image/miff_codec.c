#include "image_internal.h"

#include "quirewise/status.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MIFF_MAX_HEADER 65536u
#define MIFF_MAX_PIXELS 20000000u

typedef struct QwMiffHeader {
    uint32_t width;
    uint32_t height;
    size_t pixel_count;
    size_t pixel_offset;
} QwMiffHeader;

static int miff_space(uint8_t value)
{
    return value <= 32u;
}

static int miff_find_separator(const uint8_t *bytes,
                               size_t size,
                               size_t *header_size,
                               size_t *pixel_offset)
{
    size_t limit = size < MIFF_MAX_HEADER ? size : MIFF_MAX_HEADER;
    for (size_t i = 0u; i + 1u < limit; ++i) {
        if (bytes[i] != ':') continue;
        if (bytes[i + 1u] == 0x1au) {
            *header_size = i;
            *pixel_offset = i + 2u;
            return 1;
        }
        if (bytes[i + 1u] == '\n') {
            *header_size = i;
            *pixel_offset = i + 2u;
            return 1;
        }
        if (bytes[i + 1u] == '\r' && i + 2u < limit &&
            bytes[i + 2u] == '\n') {
            *header_size = i;
            *pixel_offset = i + 3u;
            return 1;
        }
    }
    return 0;
}

static int miff_get_value(const uint8_t *header,
                          size_t header_size,
                          const char *key,
                          char *value,
                          size_t value_capacity)
{
    size_t key_length = strlen(key);
    if (!header || !key || !value || value_capacity < 2u) return 0;
    for (size_t i = 0u; i + key_length < header_size; ++i) {
        size_t start;
        size_t length;
        if (i && !miff_space(header[i - 1u])) continue;
        if (memcmp(header + i, key, key_length) != 0 ||
            header[i + key_length] != '=') continue;
        start = i + key_length + 1u;
        if (start >= header_size || header[start] == '{') return 0;
        length = 0u;
        while (start + length < header_size &&
               !miff_space(header[start + length]) &&
               header[start + length] != '}') {
            ++length;
        }
        if (!length || length >= value_capacity) return 0;
        memcpy(value, header + start, length);
        value[length] = '\0';
        return 1;
    }
    return 0;
}

static int miff_get_dimension(const uint8_t *header,
                              size_t header_size,
                              const char *key,
                              uint32_t *out_value)
{
    char value[16];
    uint64_t number = 0u;
    if (!out_value || !miff_get_value(header, header_size, key, value,
                                      sizeof(value))) {
        return 0;
    }
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return 0;
        number = number * 10u + (*cursor - '0');
        if (number > UINT32_MAX) return 0;
    }
    if (!number) return 0;
    *out_value = (uint32_t)number;
    return 1;
}

static int miff_read_header(const void *data, size_t size, QwMiffHeader *header)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t header_size;
    char value[32];
    uint32_t depth = 8u;
    uint32_t channels;
    if (!bytes || !header || size < 16u ||
        !miff_find_separator(bytes, size, &header_size, &header->pixel_offset) ||
        header_size < 14u || memcmp(bytes, "id=ImageMagick", 14u) != 0 ||
        !miff_get_dimension(bytes, header_size, "columns", &header->width) ||
        !miff_get_dimension(bytes, header_size, "rows", &header->height) ||
        !miff_get_dimension(bytes, header_size, "number-channels", &channels)) {
        return 0;
    }
    if (miff_get_value(bytes, header_size, "depth", value, sizeof(value))) {
        uint64_t parsed = 0u;
        for (const unsigned char *cursor = (const unsigned char *)value;
             *cursor; ++cursor) {
            if (*cursor < '0' || *cursor > '9') return 0;
            parsed = parsed * 10u + (*cursor - '0');
            if (parsed > UINT32_MAX) return 0;
        }
        depth = (uint32_t)parsed;
    }
    if (depth != 8u || channels != 3u) return 0;
    if (miff_get_value(bytes, header_size, "class", value, sizeof(value)) &&
        strcmp(value, "DirectClass") != 0) return 0;
    if (!miff_get_value(bytes, header_size, "colorspace", value, sizeof(value)) ||
        (strcmp(value, "sRGB") != 0 && strcmp(value, "RGB") != 0)) return 0;
    if (miff_get_value(bytes, header_size, "compression", value, sizeof(value)) &&
        strcmp(value, "None") != 0 && strcmp(value, "NoCompression") != 0) {
        return 0;
    }
    {
        uint64_t pixel_count = (uint64_t)header->width * header->height;
        if (!pixel_count || pixel_count > MIFF_MAX_PIXELS ||
            pixel_count > SIZE_MAX / 3u ||
            pixel_count * 3u != size - header->pixel_offset) {
            return 0;
        }
        header->pixel_count = (size_t)pixel_count;
    }
    return 1;
}

int qw_miff_probe(const void *data, size_t size)
{
    QwMiffHeader header;
    return miff_read_header(data, size, &header);
}

int qw_miff_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwMiffHeader header;
    uint8_t *pixels;
    size_t output_size;
    if (!out_page || !miff_read_header(data, size, &header)) {
        return QW_STATUS_ERR_FORMAT;
    }
    memset(out_page, 0, sizeof(*out_page));
    output_size = header.pixel_count * 3u;
    pixels = (uint8_t *)malloc(output_size);
    if (!pixels) return QW_STATUS_ERR_OUT_OF_MEMORY;
    memcpy(pixels, bytes + header.pixel_offset, output_size);
    out_page->data = pixels;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = header.width;
    out_page->height = header.height;
    return QW_STATUS_OK;
}
