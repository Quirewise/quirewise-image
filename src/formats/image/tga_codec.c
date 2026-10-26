#include "image_internal.h"

#include "../../core/byte_utils.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    TGA_HEADER_SIZE = 18,
    TGA_TYPE_COLOR_MAPPED = 1,
    TGA_TYPE_TRUE_COLOR = 2,
    TGA_TYPE_GRAYSCALE = 3,
    TGA_TYPE_COLOR_MAPPED_RLE = 9,
    TGA_TYPE_TRUE_COLOR_RLE = 10,
    TGA_TYPE_GRAYSCALE_RLE = 11
};

typedef struct QwTgaInfo {
    const uint8_t *bytes;
    size_t size;
    size_t pixel_offset;
    size_t map_offset;
    size_t map_entry_size;
    size_t pixel_count;
    uint32_t width;
    uint32_t height;
    uint16_t map_first;
    uint16_t map_length;
    uint8_t image_type;
    uint8_t map_type;
    uint8_t map_entry_bits;
    uint8_t pixel_bits;
    uint8_t descriptor;
    uint8_t alpha_bits;
    int indexed;
    int grayscale;
    int rle;
} QwTgaInfo;

static int tga_is_indexed(uint8_t image_type)
{
    return image_type == TGA_TYPE_COLOR_MAPPED ||
           image_type == TGA_TYPE_COLOR_MAPPED_RLE;
}

static int tga_is_grayscale(uint8_t image_type)
{
    return image_type == TGA_TYPE_GRAYSCALE ||
           image_type == TGA_TYPE_GRAYSCALE_RLE;
}

static int tga_is_rle(uint8_t image_type)
{
    return image_type == TGA_TYPE_COLOR_MAPPED_RLE ||
           image_type == TGA_TYPE_TRUE_COLOR_RLE ||
           image_type == TGA_TYPE_GRAYSCALE_RLE;
}

static int tga_type_supported(uint8_t image_type)
{
    return image_type == TGA_TYPE_COLOR_MAPPED ||
           image_type == TGA_TYPE_TRUE_COLOR ||
           image_type == TGA_TYPE_GRAYSCALE ||
           image_type == TGA_TYPE_COLOR_MAPPED_RLE ||
           image_type == TGA_TYPE_TRUE_COLOR_RLE ||
           image_type == TGA_TYPE_GRAYSCALE_RLE;
}

static int tga_color_depth_supported(uint8_t depth)
{
    return depth == 15u || depth == 16u || depth == 24u || depth == 32u;
}

static int tga_candidate(const uint8_t *bytes, size_t size)
{
    uint8_t image_type;
    uint8_t map_type;
    uint8_t pixel_bits;
    uint8_t map_bits;
    uint16_t map_length;
    uint16_t width;
    uint16_t height;
    size_t map_entry_size;
    size_t map_size;
    size_t offset;
    int indexed;
    int grayscale;

    if (!bytes || size < TGA_HEADER_SIZE) return 0;
    image_type = bytes[2];
    map_type = bytes[1];
    pixel_bits = bytes[16];
    map_bits = bytes[7];
    map_length = qw_read_u16_le(bytes + 5u);
    width = qw_read_u16_le(bytes + 12u);
    height = qw_read_u16_le(bytes + 14u);
    indexed = tga_is_indexed(image_type);
    grayscale = tga_is_grayscale(image_type);
    if (!tga_type_supported(image_type) || map_type > 1u ||
        !width || !height) {
        return 0;
    }
    if (indexed) {
        if (map_type != 1u || !map_length ||
            (pixel_bits != 8u && pixel_bits != 16u) ||
            !tga_color_depth_supported(map_bits)) {
            return 0;
        }
    } else if (grayscale) {
        if (pixel_bits != 8u && pixel_bits != 16u) return 0;
    } else if (!tga_color_depth_supported(pixel_bits)) {
        return 0;
    }
    if (map_type == 1u &&
        (!map_length || !tga_color_depth_supported(map_bits))) {
        return 0;
    }
    map_size = 0u;
    if (map_type == 1u) {
        map_entry_size = ((size_t)map_bits + 7u) / 8u;
        if (!qw_checked_size_mul((size_t)map_length, map_entry_size,
                                 &map_size)) {
            return 0;
        }
    }
    offset = TGA_HEADER_SIZE + (size_t)bytes[0];
    if (offset > size || map_size > size - offset) return 0;
    offset += map_size;
    return offset <= size;
}

static int tga_parse_header(const uint8_t *bytes,
                            size_t size,
                            QwTgaInfo *info)
{
    size_t map_size;
    size_t offset;

    if (!info || !tga_candidate(bytes, size)) return QW_STATUS_ERR_FORMAT;
    memset(info, 0, sizeof(*info));
    info->bytes = bytes;
    info->size = size;
    info->image_type = bytes[2];
    info->map_type = bytes[1];
    info->map_first = qw_read_u16_le(bytes + 3u);
    info->map_length = qw_read_u16_le(bytes + 5u);
    info->map_entry_bits = bytes[7];
    info->width = qw_read_u16_le(bytes + 12u);
    info->height = qw_read_u16_le(bytes + 14u);
    info->pixel_bits = bytes[16];
    info->descriptor = bytes[17];
    info->alpha_bits = info->descriptor & 0x0fu;
    info->indexed = tga_is_indexed(info->image_type);
    info->grayscale = tga_is_grayscale(info->image_type);
    info->rle = tga_is_rle(info->image_type);

    if (!qw_image_dimensions_within_limits(info->width, info->height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if ((info->descriptor & 0xc0u) != 0u) return QW_STATUS_ERR_UNSUPPORTED;
    if (!qw_checked_size_mul((size_t)info->width, (size_t)info->height,
                             &info->pixel_count)) {
        return QW_STATUS_ERR_FORMAT;
    }

    info->map_entry_size = ((size_t)info->map_entry_bits + 7u) / 8u;
    map_size = 0u;
    if (info->map_type == 1u) {
        if (!info->map_length) return QW_STATUS_ERR_FORMAT;
        if (!tga_color_depth_supported(info->map_entry_bits)) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if (!qw_checked_size_mul((size_t)info->map_length,
                                 info->map_entry_size, &map_size)) {
            return QW_STATUS_ERR_FORMAT;
        }
    }
    offset = TGA_HEADER_SIZE + (size_t)bytes[0];
    if (offset > size || map_size > size - offset) return QW_STATUS_ERR_FORMAT;
    info->map_offset = offset;
    info->pixel_offset = offset + map_size;
    if (info->pixel_offset > size) return QW_STATUS_ERR_FORMAT;

    if (info->indexed) {
        if (info->map_type != 1u || !info->map_length ||
            (info->pixel_bits != 8u && info->pixel_bits != 16u)) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (info->alpha_bits != 0u && info->alpha_bits != 1u &&
            info->alpha_bits != 8u) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if (!tga_color_depth_supported(info->map_entry_bits)) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if ((info->map_entry_bits == 15u || info->map_entry_bits == 24u) &&
            info->alpha_bits != 0u) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if (info->map_entry_bits == 16u && info->alpha_bits > 1u) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if (info->map_entry_bits == 32u && info->alpha_bits != 0u &&
            info->alpha_bits != 8u) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if ((uint32_t)info->map_first + info->map_length > 65536u) {
            return QW_STATUS_ERR_FORMAT;
        }
    } else if (info->grayscale) {
        if (info->pixel_bits == 8u && info->alpha_bits == 0u) {
            /* Single-channel grayscale. */
        } else if (info->pixel_bits == 16u && info->alpha_bits == 8u) {
            /* Eight-bit grayscale followed by eight-bit alpha. */
        } else {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
    } else {
        if (!tga_color_depth_supported(info->pixel_bits)) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if ((info->pixel_bits == 15u || info->pixel_bits == 24u) &&
            info->alpha_bits != 0u) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if (info->pixel_bits == 16u && info->alpha_bits > 1u) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if (info->pixel_bits == 32u && info->alpha_bits != 0u &&
            info->alpha_bits != 8u) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
    }

    return QW_STATUS_OK;
}

static uint8_t tga_scale_5bit(uint32_t value)
{
    return (uint8_t)((value * 255u + 15u) / 31u);
}

static uint8_t tga_composite_white(uint8_t color, uint8_t alpha)
{
    return (uint8_t)(((uint32_t)color * alpha +
                      255u * (255u - alpha) + 127u) / 255u);
}

static int tga_decode_color(const uint8_t *sample,
                            uint8_t depth,
                            uint8_t alpha_bits,
                            uint8_t *red,
                            uint8_t *green,
                            uint8_t *blue,
                            uint8_t *alpha)
{
    if (!sample || !red || !green || !blue || !alpha) return 0;
    if (depth == 15u || depth == 16u) {
        uint16_t packed = qw_read_u16_le(sample);
        *blue = tga_scale_5bit((uint32_t)(packed & 0x1fu));
        *green = tga_scale_5bit((uint32_t)((packed >> 5) & 0x1fu));
        *red = tga_scale_5bit((uint32_t)((packed >> 10) & 0x1fu));
        *alpha = alpha_bits == 1u && (packed & 0x8000u) == 0u
            ? 0u : 255u;
        return 1;
    }
    if (depth == 24u || depth == 32u) {
        *blue = sample[0];
        *green = sample[1];
        *red = sample[2];
        *alpha = depth == 32u && alpha_bits == 8u ? sample[3] : 255u;
        return 1;
    }
    return 0;
}

static int tga_read_color_index(const QwTgaInfo *info,
                                size_t *source_offset,
                                uint8_t *red,
                                uint8_t *green,
                                uint8_t *blue,
                                uint8_t *alpha)
{
    size_t index_size = (size_t)info->pixel_bits / 8u;
    uint32_t index;
    const uint8_t *entry;

    if (*source_offset > info->size ||
        index_size > info->size - *source_offset) {
        return QW_STATUS_ERR_FORMAT;
    }
    index = info->pixel_bits == 8u
        ? info->bytes[*source_offset]
        : qw_read_u16_le(info->bytes + *source_offset);
    *source_offset += index_size;
    if (index < info->map_first ||
        index >= (uint32_t)info->map_first + info->map_length) {
        return QW_STATUS_ERR_FORMAT;
    }
    entry = info->bytes + info->map_offset +
            (size_t)(index - info->map_first) * info->map_entry_size;
    if (!tga_decode_color(entry, info->map_entry_bits, info->alpha_bits,
                          red, green, blue, alpha)) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    return QW_STATUS_OK;
}

static int tga_read_pixel(const QwTgaInfo *info,
                          size_t *source_offset,
                          uint8_t *red,
                          uint8_t *green,
                          uint8_t *blue,
                          uint8_t *alpha)
{
    size_t sample_size;
    const uint8_t *sample;

    if (info->indexed) {
        return tga_read_color_index(info, source_offset, red, green, blue,
                                    alpha);
    }
    sample_size = ((size_t)info->pixel_bits + 7u) / 8u;
    if (*source_offset > info->size ||
        sample_size > info->size - *source_offset) {
        return QW_STATUS_ERR_FORMAT;
    }
    sample = info->bytes + *source_offset;
    *source_offset += sample_size;
    if (info->grayscale) {
        *red = sample[0];
        *green = sample[0];
        *blue = sample[0];
        *alpha = info->pixel_bits == 16u ? sample[1] : 255u;
        return QW_STATUS_OK;
    }
    if (!tga_decode_color(sample, info->pixel_bits, info->alpha_bits,
                          red, green, blue, alpha)) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    return QW_STATUS_OK;
}

static void tga_store_pixel(const QwTgaInfo *info,
                            size_t sequence_index,
                            uint8_t red,
                            uint8_t green,
                            uint8_t blue,
                            uint8_t alpha,
                            uint8_t *pixels)
{
    uint32_t sequence_x = (uint32_t)(sequence_index % info->width);
    uint32_t sequence_y = (uint32_t)(sequence_index / info->width);
    uint32_t x = (info->descriptor & 0x10u)
        ? info->width - 1u - sequence_x : sequence_x;
    uint32_t y = (info->descriptor & 0x20u)
        ? sequence_y : info->height - 1u - sequence_y;
    uint8_t *destination = pixels +
        ((size_t)y * info->width + x) * 3u;

    destination[0] = tga_composite_white(red, alpha);
    destination[1] = tga_composite_white(green, alpha);
    destination[2] = tga_composite_white(blue, alpha);
}

static int tga_decode_pixels(const QwTgaInfo *info, uint8_t *pixels)
{
    size_t source_offset = info->pixel_offset;
    size_t decoded = 0u;

    while (decoded < info->pixel_count) {
        size_t packet_count = 1u;
        int repeated = 0;
        uint8_t red;
        uint8_t green;
        uint8_t blue;
        uint8_t alpha;
        int rc;

        if (info->rle) {
            uint8_t packet_header;
            if (source_offset >= info->size) return QW_STATUS_ERR_FORMAT;
            packet_header = info->bytes[source_offset++];
            packet_count = (size_t)(packet_header & 0x7fu) + 1u;
            repeated = (packet_header & 0x80u) != 0u;
            if (packet_count > info->pixel_count - decoded) {
                return QW_STATUS_ERR_FORMAT;
            }
        }

        if (repeated || !info->rle) {
            rc = tga_read_pixel(info, &source_offset, &red, &green, &blue,
                                &alpha);
            if (rc != QW_STATUS_OK) return rc;
            for (size_t i = 0u; i < packet_count; ++i) {
                tga_store_pixel(info, decoded++, red, green, blue, alpha,
                                pixels);
            }
        } else {
            for (size_t i = 0u; i < packet_count; ++i) {
                rc = tga_read_pixel(info, &source_offset, &red, &green, &blue,
                                    &alpha);
                if (rc != QW_STATUS_OK) return rc;
                tga_store_pixel(info, decoded++, red, green, blue, alpha,
                                pixels);
            }
        }
    }
    return QW_STATUS_OK;
}

int qw_tga_probe(const void *data, size_t size)
{
    return tga_candidate((const uint8_t *)data, size);
}

int qw_tga_decode_page(const void *data,
                       size_t size,
                       QwPageImage *out_page)
{
    QwTgaInfo info;
    uint8_t *pixels;
    size_t output_size;
    int rc;

    if (!data || !size || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    rc = tga_parse_header((const uint8_t *)data, size, &info);
    if (rc != QW_STATUS_OK) return rc;
    if (!qw_checked_size_mul(info.pixel_count, 3u, &output_size)) {
        return QW_STATUS_ERR_FORMAT;
    }
    pixels = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!pixels) return QW_STATUS_ERR_OUT_OF_MEMORY;
    rc = tga_decode_pixels(&info, pixels);
    if (rc != QW_STATUS_OK) {
        free(pixels);
        return rc;
    }
    out_page->data = pixels;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = info.width;
    out_page->height = info.height;
    return QW_STATUS_OK;
}
