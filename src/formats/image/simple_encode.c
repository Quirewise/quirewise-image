/* Uncompressed image writers: TGA, QOI and PPM.
 *
 * These formats need no compression machinery, so the encoders are small and
 * deterministic:
 * - TGA: uncompressed 24-bit true-color, top-left origin.
 * - QOI: opaque stream using only QOI_OP_RGB literals (valid but unoptimized)
 *   plus the required end marker.
 * - PPM: binary P6, always RGB (Gray8 inputs are replicated).
 */

#include "quirewise/image.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QW_SIMPLE_ENCODE_LIMIT ((size_t)128u * 1024u * 1024u / 4u)

static int simple_channels(const QwImage *image)
{
    return image->format == QW_IMAGE_PIXEL_RGB24 ? 3 : 1;
}

static int simple_validate(const QwImage *image, uint8_t **out_data,
                           size_t *out_size)
{
    if (!image || !image->pixels || !image->width || !image->height ||
        (image->format != QW_IMAGE_PIXEL_GRAY8 &&
         image->format != QW_IMAGE_PIXEL_RGB24))
        return 0;
    if (!out_data || !out_size) return 0;
    if (image->width > QW_SIMPLE_ENCODE_LIMIT / 4u ||
        image->height > QW_SIMPLE_ENCODE_LIMIT / 4u)
        return 0;
    return 1;
}

static void simple_sample(const QwImage *image, size_t pixel, uint8_t *red,
                          uint8_t *green, uint8_t *blue)
{
    if (simple_channels(image) == 3) {
        const uint8_t *source = image->pixels + pixel * 3u;
        *red = source[0];
        *green = source[1];
        *blue = source[2];
    } else {
        *red = image->pixels[pixel];
        *green = *red;
        *blue = *red;
    }
}

int qw_image_tga_encode(const QwImage *image, uint8_t **out_data,
                        size_t *out_size)
{
    size_t row_bytes;
    size_t total;
    uint8_t *data;
    uint32_t row;
    uint32_t column;

    if (!simple_validate(image, out_data, out_size))
        return (int)QW_IMAGE_ERR_INVALID_ARGUMENT;
    row_bytes = (size_t)image->width * 3u;
    if (row_bytes > QW_SIMPLE_ENCODE_LIMIT ||
        row_bytes > SIZE_MAX / image->height - 18u)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    total = 18u + row_bytes * image->height;
    data = (uint8_t *)calloc(1u, total);
    if (!data) return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    data[2] = 2u; /* uncompressed true-color */
    data[12] = (uint8_t)(image->width & 0xffu);
    data[13] = (uint8_t)((image->width >> 8) & 0xffu);
    data[14] = (uint8_t)(image->height & 0xffu);
    data[15] = (uint8_t)((image->height >> 8) & 0xffu);
    data[16] = 24u;
    data[17] = 0x20u; /* top-left origin */
    for (row = 0u; row < image->height; ++row) {
        uint8_t *line = data + 18u + (size_t)row * row_bytes;
        for (column = 0u; column < image->width; ++column) {
            size_t pixel = (size_t)row * image->width + column;
            uint8_t red;
            uint8_t green;
            uint8_t blue;

            simple_sample(image, pixel, &red, &green, &blue);
            line[column * 3u] = blue;
            line[column * 3u + 1u] = green;
            line[column * 3u + 2u] = red;
        }
    }
    *out_data = data;
    *out_size = total;
    return (int)QW_IMAGE_OK;
}

int qw_image_qoi_encode(const QwImage *image, uint8_t **out_data,
                        size_t *out_size)
{
    size_t pixel_count;
    size_t total;
    uint8_t *data;
    size_t pixel;
    size_t offset = 0u;

    if (!simple_validate(image, out_data, out_size))
        return (int)QW_IMAGE_ERR_INVALID_ARGUMENT;
    if (image->width > SIZE_MAX / image->height)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    pixel_count = (size_t)image->width * image->height;
    if (pixel_count > (QW_SIMPLE_ENCODE_LIMIT - 22u) / 4u)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    total = 22u + pixel_count * 4u;
    data = (uint8_t *)malloc(total);
    if (!data) return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    memcpy(data, "qoif", 4u);
    data[4] = (uint8_t)(image->width >> 24);
    data[5] = (uint8_t)(image->width >> 16);
    data[6] = (uint8_t)(image->width >> 8);
    data[7] = (uint8_t)(image->width & 0xffu);
    data[8] = (uint8_t)(image->height >> 24);
    data[9] = (uint8_t)(image->height >> 16);
    data[10] = (uint8_t)(image->height >> 8);
    data[11] = (uint8_t)(image->height & 0xffu);
    data[12] = 3u; /* channels: RGB */
    data[13] = 0u; /* sRGB with linear alpha */
    offset = 14u;
    for (pixel = 0u; pixel < pixel_count; ++pixel) {
        uint8_t red;
        uint8_t green;
        uint8_t blue;

        simple_sample(image, pixel, &red, &green, &blue);
        data[offset++] = 0xfeu; /* QOI_OP_RGB */
        data[offset++] = red;
        data[offset++] = green;
        data[offset++] = blue;
    }
    memset(data + offset, 0, 7u);
    offset += 7u;
    data[offset++] = 1u;
    *out_data = data;
    *out_size = offset;
    return (int)QW_IMAGE_OK;
}

int qw_image_ppm_encode(const QwImage *image, uint8_t **out_data,
                        size_t *out_size)
{
    char header[64];
    int header_size;
    size_t pixel_count;
    size_t total;
    uint8_t *data;
    size_t pixel;

    if (!simple_validate(image, out_data, out_size))
        return (int)QW_IMAGE_ERR_INVALID_ARGUMENT;
    if (image->width > SIZE_MAX / image->height)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    pixel_count = (size_t)image->width * image->height;
    header_size = snprintf(header, sizeof(header), "P6\n%u %u\n255\n",
                           (unsigned)image->width,
                           (unsigned)image->height);
    if (header_size <= 0 || (size_t)header_size >= sizeof(header))
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    if (pixel_count > (QW_SIMPLE_ENCODE_LIMIT - (size_t)header_size) / 3u)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    total = (size_t)header_size + pixel_count * 3u;
    data = (uint8_t *)malloc(total);
    if (!data) return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    memcpy(data, header, (size_t)header_size);
    for (pixel = 0u; pixel < pixel_count; ++pixel) {
        uint8_t red;
        uint8_t green;
        uint8_t blue;

        simple_sample(image, pixel, &red, &green, &blue);
        data[(size_t)header_size + pixel * 3u] = red;
        data[(size_t)header_size + pixel * 3u + 1u] = green;
        data[(size_t)header_size + pixel * 3u + 2u] = blue;
    }
    *out_data = data;
    *out_size = total;
    return (int)QW_IMAGE_OK;
}
