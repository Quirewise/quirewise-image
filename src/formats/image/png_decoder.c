#include "image_internal.h"
#include "../../core/byte_utils.h"

#include <quirewise/compression.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t png_signature[8] = {
    0x89u, 'P', 'N', 'G', '\r', '\n', 0x1au, '\n'
};

typedef struct QwPngInfo {
    uint32_t width;
    uint32_t height;
    uint8_t bit_depth;
    uint8_t color_type;
    uint8_t interlace;
    uint8_t palette[768];
    size_t palette_entries;
    uint8_t palette_alpha[256];
    uint16_t transparent_sample[3];
    size_t idat_size;
    int have_palette;
    int have_transparency;
} QwPngInfo;

typedef struct QwPngPass {
    uint32_t x_start;
    uint32_t y_start;
    uint32_t x_step;
    uint32_t y_step;
    uint32_t width;
    uint32_t height;
    size_t row_bytes;
} QwPngPass;

static int png_chunk_type_valid(const uint8_t *type)
{
    for (size_t i = 0; i < 4u; ++i) {
        if (!((type[i] >= 'A' && type[i] <= 'Z') ||
              (type[i] >= 'a' && type[i] <= 'z'))) {
            return 0;
        }
    }
    /* The third chunk-type byte is reserved and must be uppercase. */
    return type[2] >= 'A' && type[2] <= 'Z';
}

static int png_chunk_crc_valid(const uint8_t *type,
                               const uint8_t *payload,
                               size_t payload_size,
                               uint32_t expected_crc)
{
    if (!type || (!payload && payload_size) || payload_size > SIZE_MAX - 4u) {
        return 0;
    }
    /* PNG chunk type and payload are contiguous in the original input buffer. */
    return qw_compression_crc32(type, 4u + payload_size) == expected_crc;
}

static int png_bit_depth_valid(uint8_t color_type, uint8_t bit_depth)
{
    switch (color_type) {
    case 0u:
        return bit_depth == 1u || bit_depth == 2u || bit_depth == 4u ||
               bit_depth == 8u || bit_depth == 16u;
    case 2u:
        return bit_depth == 8u || bit_depth == 16u;
    case 3u:
        return bit_depth == 1u || bit_depth == 2u || bit_depth == 4u ||
               bit_depth == 8u;
    case 4u:
    case 6u:
        return bit_depth == 8u || bit_depth == 16u;
    default:
        return 0;
    }
}

static int png_parse_header(QwPngInfo *info,
                            const uint8_t *payload,
                            size_t payload_size)
{
    if (!info || !payload || payload_size != 13u) return 0;
    info->width = qw_read_u32_be(payload);
    info->height = qw_read_u32_be(payload + 4u);
    info->bit_depth = payload[8];
    info->color_type = payload[9];
    info->interlace = payload[12];
    return info->width && info->height &&
           png_bit_depth_valid(info->color_type, info->bit_depth) &&
           payload[10] == 0u && payload[11] == 0u && info->interlace <= 1u;
}

static int png_parse_palette(QwPngInfo *info,
                             const uint8_t *payload,
                             size_t payload_size)
{
    size_t entries;
    if (!info || !payload || info->have_palette || info->color_type == 0u ||
        info->color_type == 4u || payload_size < 3u || payload_size > 768u ||
        payload_size % 3u != 0u) {
        return 0;
    }
    entries = payload_size / 3u;
    if (info->color_type == 3u &&
        entries > ((size_t)1u << info->bit_depth)) {
        return 0;
    }
    memcpy(info->palette, payload, payload_size);
    info->palette_entries = entries;
    info->have_palette = 1;
    return 1;
}

static int png_parse_transparency(QwPngInfo *info,
                                  const uint8_t *payload,
                                  size_t payload_size)
{
    if (!info || !payload || info->have_transparency) return 0;
    if (info->color_type == 0u) {
        uint16_t value;
        if (payload_size != 2u) return 0;
        value = qw_read_u16_be(payload);
        if (info->bit_depth < 16u)
            value = (uint16_t)(value & (((uint16_t)1u << info->bit_depth) - 1u));
        info->transparent_sample[0] = value;
    } else if (info->color_type == 2u) {
        if (payload_size != 6u) return 0;
        for (size_t i = 0; i < 3u; ++i) {
            uint16_t value = qw_read_u16_be(payload + i * 2u);
            if (info->bit_depth == 8u) value &= 0x00ffu;
            info->transparent_sample[i] = value;
        }
    } else if (info->color_type == 3u) {
        if (!info->have_palette || payload_size > info->palette_entries) {
            return 0;
        }
        for (size_t i = 0; i < payload_size; ++i) {
            info->palette_alpha[i] = payload[i];
        }
    } else {
        return 0;
    }
    info->have_transparency = 1;
    return 1;
}

static int png_parse_chunks(const uint8_t *bytes,
                            size_t size,
                            QwPngInfo *info)
{
    size_t position = sizeof(png_signature);
    int have_header = 0;
    int have_data = 0;
    int data_ended = 0;
    int have_end = 0;

    if (!bytes || !info || size < sizeof(png_signature) + 12u ||
        memcmp(bytes, png_signature, sizeof(png_signature)) != 0) {
        return 0;
    }
    memset(info, 0, sizeof(*info));
    memset(info->palette_alpha, 255, sizeof(info->palette_alpha));

    while (position <= size && size - position >= 12u) {
        uint32_t length = qw_read_u32_be(bytes + position);
        const uint8_t *type = bytes + position + 4u;
        const uint8_t *payload = bytes + position + 8u;
        size_t chunk_size;
        uint32_t expected_crc;

        if ((size_t)length > size - position - 12u ||
            !png_chunk_type_valid(type)) {
            return 0;
        }
        chunk_size = (size_t)length + 12u;
        expected_crc = qw_read_u32_be(payload + (size_t)length);
        if (!png_chunk_crc_valid(type, payload, length, expected_crc)) {
            return 0;
        }

        if (!have_header) {
            if (position != sizeof(png_signature) ||
                memcmp(type, "IHDR", 4u) != 0 ||
                !png_parse_header(info, payload, length)) {
                return 0;
            }
            have_header = 1;
        } else if (memcmp(type, "IHDR", 4u) == 0) {
            return 0;
        } else if (memcmp(type, "PLTE", 4u) == 0) {
            if (have_data || !png_parse_palette(info, payload, length)) {
                return 0;
            }
        } else if (memcmp(type, "tRNS", 4u) == 0) {
            if (have_data || !png_parse_transparency(info, payload, length)) {
                return 0;
            }
        } else if (memcmp(type, "IDAT", 4u) == 0) {
            if (data_ended || (info->color_type == 3u && !info->have_palette) ||
                info->idat_size > SIZE_MAX - (size_t)length) {
                return 0;
            }
            have_data = 1;
            info->idat_size += (size_t)length;
        } else if (memcmp(type, "IEND", 4u) == 0) {
            if (length != 0u || !have_data) return 0;
            have_end = 1;
            position += chunk_size;
            break;
        } else {
            if (!(type[0] >= 'a' && type[0] <= 'z')) {
                return 0;
            }
            if (have_data) data_ended = 1;
        }

        if (have_data && memcmp(type, "IDAT", 4u) != 0) {
            data_ended = 1;
        }
        position += chunk_size;
    }

    if (info->color_type == 3u && !info->have_palette) return 0;
    return have_header && have_data && info->idat_size != 0u && have_end &&
           position == size;
}

int qw_png_probe(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    return bytes && size >= sizeof(png_signature) &&
           memcmp(bytes, png_signature, sizeof(png_signature)) == 0;
}

int qw_png_dimensions(const void *data,
                      size_t size,
                      uint32_t *out_width,
                      uint32_t *out_height)
{
    QwPngInfo info;
    if (!out_width || !out_height) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_width = 0u;
    *out_height = 0u;
    if (!png_parse_chunks((const uint8_t *)data, size, &info)) {
        return QW_STATUS_ERR_FORMAT;
    }
    *out_width = info.width;
    *out_height = info.height;
    return QW_STATUS_OK;
}

static size_t png_channel_count(uint8_t color_type)
{
    switch (color_type) {
    case 0u: return 1u;
    case 2u: return 3u;
    case 3u: return 1u;
    case 4u: return 2u;
    case 6u: return 4u;
    default: return 0u;
    }
}

static uint32_t png_pass_extent(uint32_t extent,
                                uint32_t start,
                                uint32_t step)
{
    if (extent <= start) return 0u;
    return (uint32_t)(((uint64_t)(extent - start) + step - 1u) / step);
}

static int png_build_pass(const QwPngInfo *info,
                          size_t pass_index,
                          QwPngPass *pass)
{
    static const uint8_t x_start[7] = {0u, 4u, 0u, 2u, 0u, 1u, 0u};
    static const uint8_t y_start[7] = {0u, 0u, 4u, 0u, 2u, 0u, 1u};
    static const uint8_t x_step[7] = {8u, 8u, 4u, 4u, 2u, 2u, 1u};
    static const uint8_t y_step[7] = {8u, 8u, 8u, 4u, 4u, 2u, 2u};
    size_t channels;
    size_t bits_per_pixel;
    size_t row_bits;

    if (!info || !pass) return 0;
    if (!info->interlace) {
        if (pass_index != 0u) return 0;
        pass->x_start = 0u;
        pass->y_start = 0u;
        pass->x_step = 1u;
        pass->y_step = 1u;
        pass->width = info->width;
        pass->height = info->height;
    } else {
        if (pass_index >= 7u) return 0;
        pass->x_start = x_start[pass_index];
        pass->y_start = y_start[pass_index];
        pass->x_step = x_step[pass_index];
        pass->y_step = y_step[pass_index];
        pass->width = png_pass_extent(info->width, pass->x_start,
                                      pass->x_step);
        pass->height = png_pass_extent(info->height, pass->y_start,
                                       pass->y_step);
    }
    channels = png_channel_count(info->color_type);
    if (!pass->width || !pass->height || !channels ||
        !qw_checked_size_mul(channels, info->bit_depth, &bits_per_pixel) ||
        !qw_checked_size_mul((size_t)pass->width, bits_per_pixel,
                             &row_bits)) {
        pass->row_bytes = 0u;
        return pass->width == 0u || pass->height == 0u;
    }
    pass->row_bytes = row_bits / 8u + (row_bits % 8u != 0u);
    return pass->row_bytes != 0u;
}

static int png_calculate_layout(const QwPngInfo *info,
                                size_t *out_raw_size,
                                size_t *out_output_size,
                                size_t *out_output_channels)
{
    size_t raw_size = 0u;
    size_t pixel_count;
    size_t output_channels;
    size_t output_size;
    size_t pass_count;

    if (!info || !out_raw_size || !out_output_size ||
        !out_output_channels ||
        !qw_image_dimensions_within_limits(info->width, info->height) ||
        !qw_checked_size_mul((size_t)info->width, (size_t)info->height,
                             &pixel_count)) {
        return 0;
    }
    output_channels = (info->color_type == 0u || info->color_type == 4u)
        ? 1u : 3u;
    if (!qw_checked_size_mul(pixel_count, output_channels, &output_size)) {
        return 0;
    }
    pass_count = info->interlace ? 7u : 1u;
    for (size_t i = 0; i < pass_count; ++i) {
        QwPngPass pass = {0};
        size_t pass_size;
        if (!png_build_pass(info, i, &pass)) return 0;
        if (!pass.width || !pass.height) continue;
        if (pass.row_bytes == SIZE_MAX ||
            !qw_checked_size_mul(pass.row_bytes + 1u,
                                 (size_t)pass.height, &pass_size) ||
            raw_size > SIZE_MAX - pass_size) {
            return 0;
        }
        raw_size += pass_size;
    }
    *out_raw_size = raw_size;
    *out_output_size = output_size;
    *out_output_channels = output_channels;
    return raw_size != 0u;
}

static int png_copy_idat(const uint8_t *bytes,
                         size_t size,
                         uint8_t *out_compressed,
                         size_t compressed_size)
{
    size_t position = sizeof(png_signature);
    size_t copied = 0u;
    while (position <= size && size - position >= 12u) {
        uint32_t length = qw_read_u32_be(bytes + position);
        const uint8_t *type = bytes + position + 4u;
        size_t chunk_size = (size_t)length + 12u;
        if ((size_t)length > size - position - 12u) return 0;
        if (memcmp(type, "IDAT", 4u) == 0) {
            if ((size_t)length > compressed_size - copied) return 0;
            if (length) {
                memcpy(out_compressed + copied, bytes + position + 8u,
                       (size_t)length);
            }
            copied += (size_t)length;
        } else if (memcmp(type, "IEND", 4u) == 0) {
            break;
        }
        position += chunk_size;
    }
    return copied == compressed_size;
}

static int png_inflate_idat(const uint8_t *compressed,
                            size_t compressed_size,
                            uint8_t *raw,
                            size_t raw_size)
{
    if (!compressed || !raw) return 0;
    return qw_compression_inflate_zlib(compressed, compressed_size, raw,
                                       raw_size);
}

static uint8_t png_paeth(uint8_t left, uint8_t above, uint8_t upper_left)
{
    int prediction = (int)left + (int)above - (int)upper_left;
    int left_distance = abs(prediction - (int)left);
    int above_distance = abs(prediction - (int)above);
    int upper_left_distance = abs(prediction - (int)upper_left);
    if (left_distance <= above_distance && left_distance <= upper_left_distance) {
        return left;
    }
    if (above_distance <= upper_left_distance) return above;
    return upper_left;
}

static int png_unfilter_row(uint8_t *row,
                            const uint8_t *previous,
                            size_t row_bytes,
                            size_t bytes_per_pixel,
                            uint8_t filter)
{
    if (!row || !bytes_per_pixel || filter > 4u) return 0;
    if (filter == 0u) return 1;
    for (size_t i = 0; i < row_bytes; ++i) {
        uint8_t left = i >= bytes_per_pixel ? row[i - bytes_per_pixel] : 0u;
        uint8_t above = previous ? previous[i] : 0u;
        uint8_t upper_left = previous && i >= bytes_per_pixel
            ? previous[i - bytes_per_pixel] : 0u;
        uint8_t predictor;
        if (filter == 1u) {
            predictor = left;
        } else if (filter == 2u) {
            predictor = above;
        } else if (filter == 3u) {
            predictor = (uint8_t)(((unsigned)left + (unsigned)above) / 2u);
        } else {
            predictor = png_paeth(left, above, upper_left);
        }
        row[i] = (uint8_t)(row[i] + predictor);
    }
    return 1;
}

static uint16_t png_read_sample(const uint8_t *row,
                                size_t sample_index,
                                uint8_t bit_depth)
{
    if (bit_depth == 16u) {
        size_t offset = sample_index * 2u;
        return (uint16_t)(((uint16_t)row[offset] << 8) | row[offset + 1u]);
    }
    if (bit_depth == 8u) return row[sample_index];
    {
        size_t bit_offset = sample_index * bit_depth;
        unsigned shift = 8u - bit_depth - (unsigned)(bit_offset % 8u);
        unsigned mask = (1u << bit_depth) - 1u;
        return (uint16_t)((row[bit_offset / 8u] >> shift) & mask);
    }
}

static uint8_t png_sample_to_byte(uint16_t sample, uint8_t bit_depth)
{
    uint32_t maximum = bit_depth == 16u ? 65535u :
                       (1u << bit_depth) - 1u;
    if (bit_depth == 8u) return (uint8_t)sample;
    return (uint8_t)(((uint32_t)sample * 255u + maximum / 2u) / maximum);
}

static uint8_t png_composite_white(uint8_t component, uint8_t alpha)
{
    return (uint8_t)(((uint32_t)component * alpha +
                      255u * (255u - alpha) + 127u) / 255u);
}

static int png_decode_pixel(const QwPngInfo *info,
                            const uint16_t samples[4],
                            uint8_t *output)
{
    uint8_t alpha = 255u;
    uint8_t components[3];
    if (!info || !samples || !output) return 0;
    switch (info->color_type) {
    case 0u:
        components[0] = png_sample_to_byte(samples[0], info->bit_depth);
        if (info->have_transparency &&
            samples[0] == info->transparent_sample[0]) {
            alpha = 0u;
        }
        output[0] = png_composite_white(components[0], alpha);
        return 1;
    case 2u:
        for (size_t i = 0; i < 3u; ++i) {
            components[i] = png_sample_to_byte(samples[i], info->bit_depth);
        }
        if (info->have_transparency &&
            samples[0] == info->transparent_sample[0] &&
            samples[1] == info->transparent_sample[1] &&
            samples[2] == info->transparent_sample[2]) {
            alpha = 0u;
        }
        for (size_t i = 0; i < 3u; ++i) {
            output[i] = png_composite_white(components[i], alpha);
        }
        return 1;
    case 3u: {
        size_t palette_index = samples[0];
        if (!info->have_palette || palette_index >= info->palette_entries) {
            return 0;
        }
        alpha = info->palette_alpha[palette_index];
        for (size_t i = 0; i < 3u; ++i) {
            output[i] = png_composite_white(
                info->palette[palette_index * 3u + i], alpha);
        }
        return 1;
    }
    case 4u:
        components[0] = png_sample_to_byte(samples[0], info->bit_depth);
        alpha = png_sample_to_byte(samples[1], info->bit_depth);
        output[0] = png_composite_white(components[0], alpha);
        return 1;
    case 6u:
        alpha = png_sample_to_byte(samples[3], info->bit_depth);
        for (size_t i = 0; i < 3u; ++i) {
            components[i] = png_sample_to_byte(samples[i], info->bit_depth);
            output[i] = png_composite_white(components[i], alpha);
        }
        return 1;
    default:
        return 0;
    }
}

static int png_decode_pass(const QwPngInfo *info,
                           const QwPngPass *pass,
                           uint8_t *raw,
                           size_t raw_size,
                           size_t *raw_position,
                           size_t bytes_per_pixel,
                           size_t output_channels,
                           uint8_t *output)
{
    uint8_t *previous = NULL;
    size_t channels = png_channel_count(info->color_type);
    size_t row_span = pass->row_bytes + 1u;

    if (!info || !pass || !raw || !raw_position || !output ||
        *raw_position > raw_size ||
        (size_t)pass->height > (raw_size - *raw_position) / row_span) {
        return 0;
    }
    for (uint32_t y = 0u; y < pass->height; ++y) {
        uint8_t filter = raw[*raw_position];
        uint8_t *row = raw + *raw_position + 1u;
        if (!png_unfilter_row(row, previous, pass->row_bytes,
                              bytes_per_pixel, filter)) {
            return 0;
        }
        for (uint32_t x = 0u; x < pass->width; ++x) {
            uint16_t samples[4] = {0u, 0u, 0u, 0u};
            uint8_t *pixel;
            size_t pixel_index;
            for (size_t channel = 0u; channel < channels; ++channel) {
                samples[channel] = png_read_sample(
                    row, (size_t)x * channels + channel, info->bit_depth);
            }
            pixel_index = ((size_t)pass->y_start +
                           (size_t)y * pass->y_step) *
                          (size_t)info->width +
                          (size_t)pass->x_start +
                          (size_t)x * pass->x_step;
            pixel = output + pixel_index * output_channels;
            if (!png_decode_pixel(info, samples, pixel)) return 0;
        }
        previous = row;
        *raw_position += row_span;
    }
    return 1;
}

static int png_decode_scanlines(const QwPngInfo *info,
                                uint8_t *raw,
                                size_t raw_size,
                                size_t output_channels,
                                uint8_t *output)
{
    size_t raw_position = 0u;
    size_t channels = png_channel_count(info->color_type);
    size_t bits_per_pixel;
    size_t bytes_per_pixel;
    size_t pass_count = info->interlace ? 7u : 1u;

    if (!qw_checked_size_mul(channels, info->bit_depth, &bits_per_pixel)) {
        return 0;
    }
    bytes_per_pixel = bits_per_pixel / 8u +
                      (bits_per_pixel % 8u != 0u);
    if (!bytes_per_pixel) bytes_per_pixel = 1u;
    for (size_t i = 0u; i < pass_count; ++i) {
        QwPngPass pass = {0};
        int pass_rc;
        if (!png_build_pass(info, i, &pass)) return 0;
        if (!pass.width || !pass.height) continue;
        pass_rc = png_decode_pass(info, &pass, raw, raw_size, &raw_position,
                                  bytes_per_pixel, output_channels, output);
        if (!pass_rc) return 0;
    }
    return raw_position == raw_size;
}

int qw_png_decode_page(const void *data,
                       size_t size,
                       QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwPngInfo info;
    uint8_t *compressed = NULL;
    uint8_t *raw = NULL;
    uint8_t *pixels = NULL;
    size_t raw_size = 0u;
    size_t output_size = 0u;
    size_t output_channels = 0u;
    int rc = QW_STATUS_ERR_FORMAT;

    if (!data || !size || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    if (!png_parse_chunks(bytes, size, &info)) return QW_STATUS_ERR_FORMAT;
    if (!qw_image_dimensions_within_limits(info.width, info.height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (!png_calculate_layout(&info, &raw_size, &output_size,
                              &output_channels)) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (info.idat_size > (size_t)UINT_MAX || raw_size > (size_t)UINT_MAX) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    compressed = (uint8_t *)malloc(info.idat_size);
    raw = (uint8_t *)malloc(raw_size);
    pixels = (uint8_t *)calloc(output_size ? output_size : 1u, 1u);
    if (!compressed || !raw || !pixels) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    if (!png_copy_idat(bytes, size, compressed, info.idat_size) ||
        !png_inflate_idat(compressed, info.idat_size, raw, raw_size) ||
        !png_decode_scanlines(&info, raw, raw_size, output_channels, pixels)) {
        goto cleanup;
    }
    out_page->data = pixels;
    out_page->size = output_size;
    out_page->kind = output_channels == 1u ? QW_PAGE_IMAGE_GRAY8
                                           : QW_PAGE_IMAGE_RGB24;
    out_page->width = info.width;
    out_page->height = info.height;
    pixels = NULL;
    rc = QW_STATUS_OK;

cleanup:
    free(compressed);
    free(raw);
    free(pixels);
    return rc;
}
