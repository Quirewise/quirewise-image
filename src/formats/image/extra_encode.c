/* Extra raster writers: Radiance HDR, SGI RGB and PCX.
 *
 * All three encoders take Gray8 or RGB24 input and write an RGB-capable file
 * (Gray8 is replicated):
 * - HDR: RGBE scanlines, flat below 8 pixels wide and new-style RLE above,
 *   with sRGB input converted back to linear radiance.
 * - SGI: uncompressed 8-bit RGB (storage 0, planar channels, bottom-up rows).
 * - PCX: 8-bit RGB, three planes per scanline, run-length encoded.
 */

#include "quirewise/image.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXTRA_LIMIT ((size_t)128u * 1024u * 1024u / 4u)

static int extra_validate(const QwImage *image, uint8_t **out_data,
                          size_t *out_size)
{
    if (!image || !image->pixels || !image->width || !image->height ||
        (image->format != QW_IMAGE_PIXEL_GRAY8 &&
         image->format != QW_IMAGE_PIXEL_RGB24) ||
        !out_data || !out_size)
        return 0;
    if (image->width > EXTRA_LIMIT / 4u || image->height > EXTRA_LIMIT / 4u)
        return 0;
    return 1;
}

static void extra_sample(const QwImage *image, uint32_t x, uint32_t y,
                         uint8_t rgb[3])
{
    if (image->format == QW_IMAGE_PIXEL_RGB24) {
        const uint8_t *source = image->pixels + (size_t)y * image->stride +
                                (size_t)x * 3u;
        rgb[0] = source[0];
        rgb[1] = source[1];
        rgb[2] = source[2];
    } else {
        rgb[0] = image->pixels[(size_t)y * image->stride + x];
        rgb[1] = rgb[0];
        rgb[2] = rgb[0];
    }
}

/* ---------------- HDR (Radiance RGBE) ---------------- */

static void hdr_store_rgbe(uint8_t red, uint8_t green, uint8_t blue,
                           uint8_t rgbe[4])
{
    float channels[3];
    float maximum;
    int exponent = 0;
    float mantissa;
    float scale;

    channels[0] = powf((float)red / 255.0f, 2.2f);
    channels[1] = powf((float)green / 255.0f, 2.2f);
    channels[2] = powf((float)blue / 255.0f, 2.2f);
    maximum = channels[0];
    if (channels[1] > maximum) maximum = channels[1];
    if (channels[2] > maximum) maximum = channels[2];
    if (!(maximum > 1e-6f)) {
        memset(rgbe, 0, 4u);
        return;
    }
    mantissa = frexpf(maximum, &exponent);
    scale = mantissa * 256.0f / maximum;
    for (int index = 0; index < 3; ++index) {
        float value = channels[index] * scale;
        rgbe[index] = value >= 255.0f ? 255u : (uint8_t)value;
    }
    exponent += 128;
    rgbe[3] = exponent > 255 ? 255u : (uint8_t)exponent;
}

static size_t hdr_rle_channel(const uint8_t *source, size_t width,
                              uint8_t *out)
{
    size_t position = 0u;
    size_t emitted = 0u;

    while (position < width) {
        size_t run = 1u;
        while (position + run < width && run < 127u &&
               source[position + run] == source[position])
            ++run;
        if (run >= 4u) {
            out[emitted++] = (uint8_t)(128u + run);
            out[emitted++] = source[position];
            position += run;
            continue;
        }
        {
            size_t literal_start = position;
            size_t literal_count = 0u;
            while (position < width && literal_count < 128u) {
                size_t ahead = 1u;
                while (position + ahead < width && ahead < 4u &&
                       source[position + ahead] == source[position])
                    ++ahead;
                if (ahead >= 4u) break;
                ++position;
                ++literal_count;
            }
            out[emitted++] = (uint8_t)literal_count;
            memcpy(out + emitted, source + literal_start, literal_count);
            emitted += literal_count;
        }
    }
    return emitted;
}

int qw_image_hdr_encode(const QwImage *image, uint8_t **out_data,
                        size_t *out_size)
{
    char header[64];
    int header_size;
    size_t width = image->width;
    size_t height = image->height;
    size_t total;
    uint8_t *data;
    size_t offset;
    uint8_t *rgbe_rows;
    size_t row_bytes;
    size_t channel;

    if (!extra_validate(image, out_data, out_size))
        return (int)QW_IMAGE_ERR_INVALID_ARGUMENT;
    if (width > 65535u || height > 65535u)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    header_size = snprintf(header, sizeof(header),
                           "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n"
                           "-Y %u +X %u\n",
                           (unsigned)height, (unsigned)width);
    if (header_size <= 0 || (size_t)header_size >= sizeof(header))
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    if (width > SIZE_MAX / height ||
        width * height > (EXTRA_LIMIT - (size_t)header_size) / 4u)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    row_bytes = width * 4u;
    if (width >= 32768u) row_bytes = width * 4u;
    rgbe_rows = (uint8_t *)malloc(row_bytes);
    if (!rgbe_rows) return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    /* Flat RGBE is four bytes per pixel; a worst-case RLE row adds one
     * length byte per 128 literals per channel. */
    total = (size_t)header_size +
            height * (4u + 4u * width + 4u * ((width + 127u) / 128u) + 8u);
    data = (uint8_t *)malloc(total);
    if (!data) {
        free(rgbe_rows);
        return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    }
    memcpy(data, header, (size_t)header_size);
    offset = (size_t)header_size;
    for (uint32_t y = 0u; y < (uint32_t)height; ++y) {
        for (uint32_t x = 0u; x < (uint32_t)width; ++x) {
            uint8_t rgb[3];
            extra_sample(image, x, y, rgb);
            hdr_store_rgbe(rgb[0], rgb[1], rgb[2],
                           rgbe_rows + (size_t)x * 4u);
        }
        if (width >= 8u && width < 32768u) {
            data[offset++] = 2u;
            data[offset++] = 2u;
            data[offset++] = (uint8_t)((width >> 8) & 0x7fu);
            data[offset++] = (uint8_t)(width & 0xffu);
            for (channel = 0u; channel < 4u; ++channel) {
                uint8_t *plane = (uint8_t *)malloc(width);
                size_t packed;
                if (!plane) {
                    free(rgbe_rows);
                    free(data);
                    return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
                }
                for (uint32_t x = 0u; x < (uint32_t)width; ++x)
                    plane[x] = rgbe_rows[(size_t)x * 4u + channel];
                packed = hdr_rle_channel(plane, width, data + offset);
                free(plane);
                offset += packed;
            }
        } else {
            memcpy(data + offset, rgbe_rows, row_bytes);
            offset += row_bytes;
        }
    }
    free(rgbe_rows);
    *out_data = data;
    *out_size = offset;
    return (int)QW_IMAGE_OK;
}

/* ---------------- SGI (uncompressed RGB) ---------------- */

static void extra_put_u16_be(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)((value >> 8) & 0xffu);
    out[1] = (uint8_t)(value & 0xffu);
}

static void extra_put_u32_be(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)((value >> 24) & 0xffu);
    out[1] = (uint8_t)((value >> 16) & 0xffu);
    out[2] = (uint8_t)((value >> 8) & 0xffu);
    out[3] = (uint8_t)(value & 0xffu);
}

int qw_image_sgi_encode(const QwImage *image, uint8_t **out_data,
                        size_t *out_size)
{
    size_t width = image->width;
    size_t height = image->height;
    size_t image_bytes;
    uint8_t *data;
    size_t channel;
    size_t source_y;

    if (!extra_validate(image, out_data, out_size))
        return (int)QW_IMAGE_ERR_INVALID_ARGUMENT;
    if (width > 65535u || height > 65535u)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    if (width > SIZE_MAX / height || width * height > EXTRA_LIMIT / 3u)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    image_bytes = width * height * 3u;
    data = (uint8_t *)calloc(1u, 512u + image_bytes);
    if (!data) return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    extra_put_u16_be(data, 474u);
    data[2] = 0u; /* uncompressed */
    data[3] = 1u; /* one byte per channel */
    extra_put_u16_be(data + 4u, 3u);
    extra_put_u16_be(data + 6u, (uint32_t)width);
    extra_put_u16_be(data + 8u, (uint32_t)height);
    extra_put_u16_be(data + 10u, 3u);
    extra_put_u32_be(data + 12u, 0u);
    extra_put_u32_be(data + 16u, 255u);
    for (channel = 0u; channel < 3u; ++channel) {
        for (source_y = 0u; source_y < height; ++source_y) {
            size_t y = height - 1u - source_y;
            uint8_t *row = data + 512u + (channel * height + source_y) * width;
            for (size_t x = 0u; x < width; ++x) {
                uint8_t rgb[3];
                extra_sample(image, (uint32_t)x, (uint32_t)y, rgb);
                row[x] = rgb[channel];
            }
        }
    }
    *out_data = data;
    *out_size = 512u + image_bytes;
    return (int)QW_IMAGE_OK;
}

/* ---------------- PCX (8-bit RGB, three planes, RLE) ---------------- */

static size_t pcx_rle_row(const uint8_t *source, size_t size, uint8_t *out)
{
    size_t position = 0u;
    size_t emitted = 0u;

    while (position < size) {
        size_t run = 1u;
        while (position + run < size && run < 63u &&
               source[position + run] == source[position])
            ++run;
        if (run > 1u || source[position] >= 0xc0u) {
            out[emitted++] = (uint8_t)(0xc0u | run);
            out[emitted++] = source[position];
            position += run;
        } else {
            out[emitted++] = source[position++];
        }
    }
    return emitted;
}

static void extra_put_u16_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
}

int qw_image_pcx_encode(const QwImage *image, uint8_t **out_data,
                        size_t *out_size)
{
    size_t width = image->width;
    size_t height = image->height;
    size_t bytes_per_line;
    size_t row_size;
    uint8_t *data;
    uint8_t *row;
    uint8_t *packed;
    size_t offset;

    if (!extra_validate(image, out_data, out_size))
        return (int)QW_IMAGE_ERR_INVALID_ARGUMENT;
    if (width > 65535u || height > 65535u)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    bytes_per_line = width + (width & 1u);
    if (bytes_per_line > EXTRA_LIMIT / 3u ||
        bytes_per_line * 3u > SIZE_MAX / height)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    row_size = bytes_per_line * 3u;
    row = (uint8_t *)calloc(1u, row_size);
    packed = (uint8_t *)malloc(row_size * 2u + 16u);
    if (!row || !packed) {
        free(row);
        free(packed);
        return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    }
    /* The encoded stream can expand at most 2x (escape byte per value). */
    data = (uint8_t *)calloc(1u, 128u + height * (row_size * 2u + 2u));
    if (!data) {
        free(row);
        free(packed);
        return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    }
    data[0] = 0x0au;
    data[1] = 5u;
    data[2] = 1u;
    data[3] = 8u;
    extra_put_u16_le(data + 4u, 0u);
    extra_put_u16_le(data + 6u, 0u);
    extra_put_u16_le(data + 8u, (uint32_t)(width - 1u));
    extra_put_u16_le(data + 10u, (uint32_t)(height - 1u));
    data[65] = 3u;
    extra_put_u16_le(data + 66u, (uint32_t)bytes_per_line);
    extra_put_u16_le(data + 68u, 1u);
    offset = 128u;
    for (size_t y = 0u; y < height; ++y) {
        size_t row_emitted = 0u;
        for (size_t channel = 0u; channel < 3u; ++channel) {
            for (size_t x = 0u; x < width; ++x) {
                uint8_t rgb[3];
                extra_sample(image, (uint32_t)x, (uint32_t)y, rgb);
                row[channel * bytes_per_line + x] = rgb[channel];
            }
            row_emitted += pcx_rle_row(row + channel * bytes_per_line,
                                       bytes_per_line,
                                       packed + row_emitted);
        }
        memcpy(data + offset, packed, row_emitted);
        offset += row_emitted;
    }
    free(row);
    free(packed);
    *out_data = data;
    *out_size = offset;
    return (int)QW_IMAGE_OK;
}
