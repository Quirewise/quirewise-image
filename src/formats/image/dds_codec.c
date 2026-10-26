#include "image_internal.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DDS_HEADER_SIZE 128u
#define DDS_MAX_PIXELS 20000000u

typedef struct QwDdsHeader {
    uint32_t width;
    uint32_t height;
    size_t block_columns;
    size_t block_rows;
    size_t block_data_size;
} QwDdsHeader;



static int dds_read_header(const void *data, size_t size, QwDdsHeader *header)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t block_count;
    uint64_t pixel_count;
    if (!bytes || !header || size < DDS_HEADER_SIZE ||
        memcmp(bytes, "DDS ", 4u) != 0 ||
        qw_read_u32_le(bytes + 4u) != 124u ||
        qw_read_u32_le(bytes + 76u) != 32u ||
        (qw_read_u32_le(bytes + 80u) & 0x4u) == 0u ||
        memcmp(bytes + 84u, "DXT1", 4u) != 0 ||
        qw_read_u32_le(bytes + 24u) != 0u ||
        (qw_read_u32_le(bytes + 112u) & 0x00200200u) != 0u) {
        return 0;
    }
    header->height = qw_read_u32_le(bytes + 12u);
    header->width = qw_read_u32_le(bytes + 16u);
    if (!header->width || !header->height) return 0;
    pixel_count = (uint64_t)header->width * header->height;
    if (pixel_count > DDS_MAX_PIXELS) return 0;
    header->block_columns = ((size_t)header->width + 3u) / 4u;
    header->block_rows = ((size_t)header->height + 3u) / 4u;
    if (header->block_columns > SIZE_MAX / header->block_rows) return 0;
    block_count = header->block_columns * header->block_rows;
    if (block_count > SIZE_MAX / 8u) return 0;
    header->block_data_size = block_count * 8u;
    return header->block_data_size <= size - DDS_HEADER_SIZE;
}

int qw_dds_probe(const void *data, size_t size)
{
    QwDdsHeader header;
    return dds_read_header(data, size, &header);
}

static uint8_t dds_expand5(uint32_t value)
{
    return (uint8_t)((value * 255u + 15u) / 31u);
}

static uint8_t dds_expand6(uint32_t value)
{
    return (uint8_t)((value * 255u + 31u) / 63u);
}

static void dds_color565(uint16_t value, uint8_t *out_rgb)
{
    out_rgb[0] = dds_expand5((value >> 11) & 31u);
    out_rgb[1] = dds_expand6((value >> 5) & 63u);
    out_rgb[2] = dds_expand5(value & 31u);
}

int qw_dds_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwDdsHeader header;
    uint8_t *rgb = NULL;
    size_t pixel_count;
    size_t output_size;

    if (!out_page || !dds_read_header(data, size, &header)) {
        return QW_STATUS_ERR_FORMAT;
    }
    memset(out_page, 0, sizeof(*out_page));
    pixel_count = (size_t)header.width * header.height;
    if (pixel_count > SIZE_MAX / 3u) return QW_STATUS_ERR_FORMAT;
    output_size = pixel_count * 3u;
    rgb = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!rgb) return QW_STATUS_ERR_OUT_OF_MEMORY;
    for (size_t block_y = 0u; block_y < header.block_rows; ++block_y) {
        for (size_t block_x = 0u; block_x < header.block_columns; ++block_x) {
            const uint8_t *block = bytes + DDS_HEADER_SIZE +
                (block_y * header.block_columns + block_x) * 8u;
            uint16_t color0 = qw_read_u16_le(block);
            uint16_t color1 = qw_read_u16_le(block + 2u);
            uint32_t indices = qw_read_u32_le(block + 4u);
            uint8_t colors[4][3];
            int transparent_color;

            dds_color565(color0, colors[0]);
            dds_color565(color1, colors[1]);
            if (color0 > color1) {
                for (size_t channel = 0u; channel < 3u; ++channel) {
                    colors[2][channel] = (uint8_t)(
                        (2u * colors[0][channel] + colors[1][channel]) / 3u);
                    colors[3][channel] = (uint8_t)(
                        (colors[0][channel] + 2u * colors[1][channel]) / 3u);
                }
                transparent_color = 0;
            } else {
                for (size_t channel = 0u; channel < 3u; ++channel) {
                    colors[2][channel] = (uint8_t)(
                        (colors[0][channel] + colors[1][channel]) / 2u);
                    colors[3][channel] = 255u;
                }
                transparent_color = 1;
            }
            for (size_t y = 0u; y < 4u; ++y) {
                for (size_t x = 0u; x < 4u; ++x) {
                    size_t image_x = block_x * 4u + x;
                    size_t image_y = block_y * 4u + y;
                    uint32_t color_index = (indices >> (2u * (y * 4u + x))) & 3u;
                    size_t output;
                    if (image_x >= header.width || image_y >= header.height) {
                        continue;
                    }
                    output = (image_y * header.width + image_x) * 3u;
                    if (transparent_color && color_index == 3u) {
                        rgb[output] = 255u;
                        rgb[output + 1u] = 255u;
                        rgb[output + 2u] = 255u;
                    } else {
                        memcpy(rgb + output, colors[color_index], 3u);
                    }
                }
            }
        }
    }
    out_page->data = rgb;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = header.width;
    out_page->height = header.height;
    return QW_STATUS_OK;
}
