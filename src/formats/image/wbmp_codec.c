#include "image_internal.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


static int wbmp_read_mbi(const uint8_t *data,
                         size_t size,
                         size_t *cursor,
                         uint32_t *out_value)
{
    uint32_t value = 0u;
    size_t i;

    if (!data || !cursor || !out_value) {
        return 0;
    }
    for (i = 0u; i < 5u; ++i) {
        uint8_t byte;
        if (*cursor >= size) {
            return 0;
        }
        byte = data[(*cursor)++];
        if (i == 0u && byte == 0x80u) {
            return 0;
        }
        if (value > (UINT32_MAX >> 7)) {
            return 0;
        }
        value = (value << 7) | (uint32_t)(byte & 0x7fu);
        if ((byte & 0x80u) == 0u) {
            *out_value = value;
            return 1;
        }
    }
    return 0;
}

static int wbmp_parse_header(const uint8_t *data,
                             size_t size,
                             uint32_t *out_width,
                             uint32_t *out_height,
                             size_t *out_payload_offset,
                             size_t *out_row_bytes,
                             size_t *out_payload_size)
{
    size_t cursor = 0u;
    size_t row_bytes;
    size_t payload_size;
    uint32_t type;
    uint32_t width;
    uint32_t height;

    if (!data || !out_width || !out_height || !out_payload_offset ||
        !out_row_bytes || !out_payload_size ||
        !wbmp_read_mbi(data, size, &cursor, &type) || type != 0u ||
        cursor >= size || data[cursor++] != 0u ||
        !wbmp_read_mbi(data, size, &cursor, &width) ||
        !wbmp_read_mbi(data, size, &cursor, &height) ||
        !width || !height || width > (uint32_t)INT_MAX ||
        height > (uint32_t)INT_MAX) {
        return 0;
    }
    row_bytes = ((size_t)width + 7u) / 8u;
    if (!qw_checked_size_mul(row_bytes, (size_t)height, &payload_size) ||
        cursor > size || size - cursor != payload_size) {
        return 0;
    }
    *out_width = width;
    *out_height = height;
    *out_payload_offset = cursor;
    *out_row_bytes = row_bytes;
    *out_payload_size = payload_size;
    return 1;
}

int qw_wbmp_probe(const void *data, size_t size)
{
    uint32_t width;
    uint32_t height;
    size_t payload_offset;
    size_t row_bytes;
    size_t payload_size;

    return wbmp_parse_header((const uint8_t *)data, size,
                             &width, &height, &payload_offset,
                             &row_bytes, &payload_size);
}

int qw_wbmp_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint8_t *rgb = NULL;
    uint32_t width;
    uint32_t height;
    size_t payload_offset;
    size_t row_bytes;
    size_t payload_size;
    size_t pixel_count;
    size_t output_size;

    if (!data || !out_page) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    memset(out_page, 0, sizeof(*out_page));
    if (!wbmp_parse_header(bytes, size, &width, &height, &payload_offset,
                           &row_bytes, &payload_size)) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (!qw_image_dimensions_within_limits(width, height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (!qw_checked_size_mul((size_t)width, (size_t)height, &pixel_count) ||
        !qw_checked_size_mul(pixel_count, 3u, &output_size)) {
        return QW_STATUS_ERR_FORMAT;
    }
    if ((width & 7u) != 0u) {
        unsigned unused_bits = 8u - (width & 7u);
        uint8_t padding_mask = (uint8_t)((1u << unused_bits) - 1u);
        for (size_t y = 0u; y < (size_t)height; ++y) {
            uint8_t last = bytes[payload_offset + y * row_bytes + row_bytes - 1u];
            if (last & padding_mask) {
                return QW_STATUS_ERR_FORMAT;
            }
        }
    }
    rgb = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!rgb) {
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    for (size_t y = 0u; y < (size_t)height; ++y) {
        const uint8_t *row = bytes + payload_offset + y * row_bytes;
        for (size_t x = 0u; x < (size_t)width; ++x) {
            uint8_t bit = (uint8_t)((row[x / 8u] >> (7u - (x & 7u))) & 1u);
            uint8_t color = bit ? 255u : 0u;
            size_t destination = (y * (size_t)width + x) * 3u;
            rgb[destination] = color;
            rgb[destination + 1u] = color;
            rgb[destination + 2u] = color;
        }
    }
    out_page->data = rgb;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = width;
    out_page->height = height;
    return QW_STATUS_OK;
}

static int wbmp_write_byte(uint8_t *data,
                           size_t capacity,
                           size_t *cursor,
                           uint8_t value)
{
    if (!data || !cursor || *cursor >= capacity) {
        return 0;
    }
    data[(*cursor)++] = value;
    return 1;
}

static int wbmp_write_mbi(uint8_t *data,
                          size_t capacity,
                          size_t *cursor,
                          uint32_t value)
{
    uint8_t groups[5];
    size_t count = 0u;

    do {
        groups[count++] = (uint8_t)(value & 0x7fu);
        value >>= 7;
    } while (value && count < sizeof(groups));
    while (count > 0u) {
        uint8_t byte = groups[--count];
        if (count > 0u) {
            byte |= 0x80u;
        }
        if (!wbmp_write_byte(data, capacity, cursor, byte)) {
            return 0;
        }
    }
    return 1;
}

static uint8_t wbmp_source_is_white(const QwPageImage *page,
                                   size_t pixel_index)
{
    if (page->kind == QW_PAGE_IMAGE_GRAY8) {
        return page->data[pixel_index] >= 128u;
    }
    {
        size_t offset = pixel_index * 3u;
        uint32_t luminance = 299u * page->data[offset] +
                             587u * page->data[offset + 1u] +
                             114u * page->data[offset + 2u];
        return luminance >= 128000u;
    }
}

int qw_wbmp_encode_page(const QwPageImage *page,
                        uint8_t **out_data,
                        size_t *out_size)
{
    uint8_t *encoded = NULL;
    size_t pixel_count;
    size_t source_channels;
    size_t source_size;
    size_t row_bytes;
    size_t payload_size;
    size_t capacity;
    size_t cursor = 0u;
    int rc = QW_STATUS_ERR_FORMAT;

    if (!page || !out_data || !out_size) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_data = NULL;
    *out_size = 0u;
    if (!page->data || !page->width || !page->height) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (page->kind == QW_PAGE_IMAGE_GRAY8) {
        source_channels = 1u;
    } else if (page->kind == QW_PAGE_IMAGE_RGB24) {
        source_channels = 3u;
    } else {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (!qw_checked_size_mul((size_t)page->width, (size_t)page->height,
                               &pixel_count) ||
        !qw_checked_size_mul(pixel_count, source_channels, &source_size) ||
        source_size > page->size || (size_t)page->width > SIZE_MAX - 7u) {
        return QW_STATUS_ERR_FORMAT;
    }
    row_bytes = ((size_t)page->width + 7u) / 8u;
    if (!qw_checked_size_mul(row_bytes, (size_t)page->height,
                               &payload_size) ||
        payload_size > SIZE_MAX - 12u) {
        return QW_STATUS_ERR_FORMAT;
    }
    capacity = 12u + payload_size;
    encoded = (uint8_t *)calloc(capacity ? capacity : 1u, 1u);
    if (!encoded) {
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }

    if (!wbmp_write_byte(encoded, capacity, &cursor, 0u) ||
        !wbmp_write_byte(encoded, capacity, &cursor, 0u) ||
        !wbmp_write_mbi(encoded, capacity, &cursor, page->width) ||
        !wbmp_write_mbi(encoded, capacity, &cursor, page->height)) {
        goto done;
    }
    for (size_t y = 0u; y < (size_t)page->height; ++y) {
        uint8_t *row = encoded + cursor + y * row_bytes;
        for (size_t x = 0u; x < (size_t)page->width; ++x) {
            size_t source_index = y * (size_t)page->width + x;
            if (wbmp_source_is_white(page, source_index)) {
                row[x / 8u] |= (uint8_t)(0x80u >> (x & 7u));
            }
        }
    }
    cursor += payload_size;
    *out_data = encoded;
    *out_size = cursor;
    encoded = NULL;
    rc = QW_STATUS_OK;

done:
    free(encoded);
    return rc;
}
