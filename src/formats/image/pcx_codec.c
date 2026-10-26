#include "image_internal.h"

#include "quirewise/status.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PCX_HEADER_SIZE 128u
#define PCX_MAX_PIXELS 20000000u

typedef struct QwPcxHeader {
    uint16_t x_min;
    uint16_t y_min;
    uint16_t x_max;
    uint16_t y_max;
    uint16_t bytes_per_line;
    uint8_t encoding;
    uint8_t bits_per_pixel;
    uint8_t planes;
} QwPcxHeader;

static int pcx_read_header(const void *data, size_t size, QwPcxHeader *header)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t width;
    uint32_t height;
    size_t packed_row;

    if (!bytes || !header || size < PCX_HEADER_SIZE || bytes[0] != 0x0au ||
        (bytes[1] != 0u && bytes[1] != 2u && bytes[1] != 3u &&
         bytes[1] != 4u && bytes[1] != 5u) ||
        (bytes[2] != 0u && bytes[2] != 1u)) {
        return 0;
    }
    header->encoding = bytes[2];
    header->bits_per_pixel = bytes[3];
    header->x_min = qw_read_u16_le(bytes + 4u);
    header->y_min = qw_read_u16_le(bytes + 6u);
    header->x_max = qw_read_u16_le(bytes + 8u);
    header->y_max = qw_read_u16_le(bytes + 10u);
    header->planes = bytes[65];
    header->bytes_per_line = qw_read_u16_le(bytes + 66u);
    if (header->x_max < header->x_min || header->y_max < header->y_min ||
        (header->bits_per_pixel != 1u && header->bits_per_pixel != 2u &&
         header->bits_per_pixel != 4u && header->bits_per_pixel != 8u) ||
        !header->planes || header->planes > 4u || !header->bytes_per_line) {
        return 0;
    }
    width = (uint32_t)header->x_max - header->x_min + 1u;
    height = (uint32_t)header->y_max - header->y_min + 1u;
    packed_row = ((size_t)width * header->bits_per_pixel + 7u) / 8u;
    if ((size_t)header->bytes_per_line < packed_row ||
        (uint64_t)width * height > PCX_MAX_PIXELS) {
        return 0;
    }
    if (header->bits_per_pixel == 8u) {
        return header->planes == 1u || header->planes == 3u;
    }
    if (header->bits_per_pixel == 1u) {
        return 1;
    }
    return header->planes == 1u;
}

int qw_pcx_probe(const void *data, size_t size)
{
    QwPcxHeader header;
    return pcx_read_header(data, size, &header);
}

static int pcx_decode_plane_row(const uint8_t *data,
                                size_t data_end,
                                size_t *position,
                                uint8_t *out,
                                size_t row_size,
                                uint8_t encoding)
{
    size_t written = 0u;
    while (written < row_size) {
        uint8_t value;
        size_t count = 1u;
        if (*position >= data_end) return 0;
        value = data[(*position)++];
        if (encoding == 1u && (value & 0xc0u) == 0xc0u) {
            count = value & 0x3fu;
            if (!count || *position >= data_end) return 0;
            value = data[(*position)++];
        }
        if (count > row_size - written) return 0;
        memset(out + written, value, count);
        written += count;
    }
    return 1;
}

static uint8_t pcx_sample(const uint8_t *row,
                          size_t plane_stride,
                          uint8_t bits_per_pixel,
                          uint32_t x)
{
    size_t bit = (size_t)x * bits_per_pixel;
    unsigned int shift = 8u - bits_per_pixel - (unsigned int)(bit & 7u);
    uint8_t mask = (uint8_t)((1u << bits_per_pixel) - 1u);
    return (uint8_t)((row[plane_stride + bit / 8u] >> shift) & mask);
}

int qw_pcx_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;
    const uint8_t *palette16;
    const uint8_t *palette256 = NULL;
    QwPcxHeader header;
    uint8_t *row = NULL;
    uint8_t *rgb = NULL;
    uint32_t width;
    uint32_t height;
    size_t row_size;
    size_t pixel_count;
    size_t output_size;
    size_t data_end = size;
    size_t position = PCX_HEADER_SIZE;
    int rc = QW_STATUS_ERR_FORMAT;

    if (!out_page || !pcx_read_header(data, size, &header)) {
        return QW_STATUS_ERR_FORMAT;
    }
    memset(out_page, 0, sizeof(*out_page));
    width = (uint32_t)header.x_max - header.x_min + 1u;
    height = (uint32_t)header.y_max - header.y_min + 1u;
    if ((size_t)header.bytes_per_line > SIZE_MAX / header.planes) {
        return QW_STATUS_ERR_FORMAT;
    }
    row_size = (size_t)header.bytes_per_line * header.planes;
    if ((size_t)width > SIZE_MAX / height ||
        (size_t)width * height > SIZE_MAX / 3u) {
        return QW_STATUS_ERR_FORMAT;
    }
    pixel_count = (size_t)width * height;
    output_size = pixel_count * 3u;
    palette16 = bytes + 16u;
    if (header.bits_per_pixel == 8u && header.planes == 1u) {
        if (size < PCX_HEADER_SIZE + 769u || bytes[size - 769u] != 0x0cu) {
            return QW_STATUS_ERR_FORMAT;
        }
        data_end = size - 769u;
        palette256 = bytes + size - 768u;
    }
    row = (uint8_t *)malloc(row_size ? row_size : 1u);
    rgb = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!row || !rgb) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (uint32_t y = 0u; y < height; ++y) {
        if (!pcx_decode_plane_row(bytes, data_end, &position, row, row_size,
                                  header.encoding)) {
            goto cleanup;
        }
        for (uint32_t x = 0u; x < width; ++x) {
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            if (header.bits_per_pixel == 8u && header.planes == 3u) {
                red = row[x];
                green = row[header.bytes_per_line + x];
                blue = row[(size_t)header.bytes_per_line * 2u + x];
            } else if (header.bits_per_pixel == 8u) {
                size_t palette_index = row[x];
                red = palette256[palette_index * 3u];
                green = palette256[palette_index * 3u + 1u];
                blue = palette256[palette_index * 3u + 2u];
            } else {
                uint8_t palette_index = 0u;
                if (header.bits_per_pixel == 1u && header.planes > 1u) {
                    for (uint8_t plane = header.planes; plane > 0u; --plane) {
                        palette_index = (uint8_t)(palette_index << 1u);
                        palette_index |= pcx_sample(
                            row, (size_t)(plane - 1u) * header.bytes_per_line,
                            1u, x);
                    }
                } else {
                    palette_index = pcx_sample(row, 0u,
                                               header.bits_per_pixel, x);
                }
                if (palette_index >= 16u) goto cleanup;
                red = palette16[(size_t)palette_index * 3u];
                green = palette16[(size_t)palette_index * 3u + 1u];
                blue = palette16[(size_t)palette_index * 3u + 2u];
            }
            {
                size_t output = ((size_t)y * width + x) * 3u;
                rgb[output] = red;
                rgb[output + 1u] = green;
                rgb[output + 2u] = blue;
            }
        }
    }
    if (palette256 && position != data_end) {
        goto cleanup;
    }
    free(row);
    out_page->data = rgb;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = width;
    out_page->height = height;
    return QW_STATUS_OK;

cleanup:
    free(row);
    free(rgb);
    return rc;
}
