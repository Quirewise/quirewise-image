#include "image_internal.h"
#include "../../core/byte_utils.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    BMP_BI_RGB = 0u,
    BMP_BI_BITFIELDS = 3u,
    BMP_BI_ALPHABITFIELDS = 6u
};

typedef struct QwBmpInfo {
    const uint8_t *bytes;
    const uint8_t *palette;
    size_t size;
    size_t pixel_offset;
    size_t row_stride;
    size_t palette_entries;
    size_t palette_entry_size;
    uint32_t width;
    uint32_t height;
    uint16_t bit_count;
    uint32_t compression;
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint32_t alpha_mask;
    int top_down;
} QwBmpInfo;

static int bmp_bit_count_supported(uint16_t bit_count)
{
    return bit_count == 1u || bit_count == 4u || bit_count == 8u ||
           bit_count == 16u || bit_count == 24u || bit_count == 32u;
}

static int bmp_mask_properties(uint32_t mask,
                              uint16_t bit_count,
                              unsigned *out_shift,
                              uint32_t *out_maximum)
{
    uint32_t allowed = bit_count == 32u ? UINT32_MAX :
                       (((uint32_t)1u << bit_count) - 1u);
    unsigned shift = 0u;
    uint32_t value;

    if (!mask || (mask & ~allowed)) return 0;
    while (((mask >> shift) & 1u) == 0u) ++shift;
    value = mask >> shift;
    if ((value & (value + 1u)) != 0u) return 0;
    if (out_shift) *out_shift = shift;
    if (out_maximum) *out_maximum = value;
    return 1;
}

static int bmp_masks_valid(const QwBmpInfo *info)
{
    if (!bmp_mask_properties(info->red_mask, info->bit_count, NULL, NULL) ||
        !bmp_mask_properties(info->green_mask, info->bit_count, NULL, NULL) ||
        !bmp_mask_properties(info->blue_mask, info->bit_count, NULL, NULL) ||
        (info->red_mask & info->green_mask) ||
        (info->red_mask & info->blue_mask) ||
        (info->green_mask & info->blue_mask)) {
        return 0;
    }
    if (info->alpha_mask &&
        (!bmp_mask_properties(info->alpha_mask, info->bit_count, NULL, NULL) ||
         (info->alpha_mask & info->red_mask) ||
         (info->alpha_mask & info->green_mask) ||
         (info->alpha_mask & info->blue_mask))) {
        return 0;
    }
    return 1;
}

static int bmp_load_bitfields(const uint8_t *bytes,
                              size_t size,
                              size_t pixel_offset,
                              uint32_t dib_size,
                              uint32_t compression,
                              QwBmpInfo *info,
                              size_t *out_metadata_end)
{
    size_t dib_end = 14u + (size_t)dib_size;
    const uint8_t *masks;

    if (compression == BMP_BI_RGB) {
        if (info->bit_count == 16u) {
            info->red_mask = 0x7c00u;
            info->green_mask = 0x03e0u;
            info->blue_mask = 0x001fu;
        } else if (info->bit_count == 32u) {
            info->red_mask = 0x00ff0000u;
            info->green_mask = 0x0000ff00u;
            info->blue_mask = 0x000000ffu;
        }
        *out_metadata_end = dib_end;
        return 1;
    }

    if (dib_size >= 52u) {
        masks = bytes + 14u + 40u;
        info->red_mask = qw_read_u32_le(masks);
        info->green_mask = qw_read_u32_le(masks + 4u);
        info->blue_mask = qw_read_u32_le(masks + 8u);
        if (dib_size >= 56u) info->alpha_mask = qw_read_u32_le(masks + 12u);
        if (compression == BMP_BI_ALPHABITFIELDS && dib_size < 56u) {
            return 0;
        }
        *out_metadata_end = dib_end;
    } else {
        size_t mask_count = compression == BMP_BI_ALPHABITFIELDS ? 4u : 3u;
        size_t mask_bytes = mask_count * 4u;
        if (dib_size != 40u || dib_end > size ||
            mask_bytes > size - dib_end ||
            dib_end > pixel_offset || mask_bytes > pixel_offset - dib_end) {
            return 0;
        }
        masks = bytes + dib_end;
        info->red_mask = qw_read_u32_le(masks);
        info->green_mask = qw_read_u32_le(masks + 4u);
        info->blue_mask = qw_read_u32_le(masks + 8u);
        if (mask_count == 4u) info->alpha_mask = qw_read_u32_le(masks + 12u);
        *out_metadata_end = dib_end + mask_bytes;
    }
    if (compression == BMP_BI_ALPHABITFIELDS && !info->alpha_mask) return 0;
    return bmp_masks_valid(info);
}

static int bmp_parse_header(const uint8_t *bytes,
                            size_t size,
                            QwBmpInfo *info)
{
    uint32_t dib_size;
    uint32_t colors_used = 0u;
    size_t metadata_end;
    size_t palette_bytes;
    size_t row_bits;
    size_t row_stride;
    uint32_t pixel_offset;
    uint16_t planes;

    if (!bytes || !info || size < 26u || bytes[0] != 'B' || bytes[1] != 'M') {
        return QW_STATUS_ERR_FORMAT;
    }
    memset(info, 0, sizeof(*info));
    info->bytes = bytes;
    info->size = size;
    pixel_offset = qw_read_u32_le(bytes + 10u);
    dib_size = qw_read_u32_le(bytes + 14u);
    if (dib_size > size - 14u) return QW_STATUS_ERR_FORMAT;
    info->pixel_offset = pixel_offset;

    if (dib_size == 12u) {
        if (size < 26u) return QW_STATUS_ERR_FORMAT;
        info->width = qw_read_u16_le(bytes + 18u);
        info->height = qw_read_u16_le(bytes + 20u);
        planes = qw_read_u16_le(bytes + 22u);
        info->bit_count = qw_read_u16_le(bytes + 24u);
        info->palette_entry_size = 3u;
        info->compression = BMP_BI_RGB;
        metadata_end = 26u;
        if (info->bit_count != 1u && info->bit_count != 4u &&
            info->bit_count != 8u && info->bit_count != 24u) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
    } else {
        int32_t width_signed;
        int32_t height_signed;
        uint64_t absolute_height;
        if (dib_size != 40u && dib_size != 52u && dib_size != 56u &&
            dib_size != 108u && dib_size != 124u) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if (dib_size < 40u) return QW_STATUS_ERR_FORMAT;
        width_signed = qw_read_i32_le(bytes + 18u);
        height_signed = qw_read_i32_le(bytes + 22u);
        planes = qw_read_u16_le(bytes + 26u);
        info->bit_count = qw_read_u16_le(bytes + 28u);
        info->compression = qw_read_u32_le(bytes + 30u);
        colors_used = qw_read_u32_le(bytes + 46u);
        if (width_signed <= 0 || height_signed == 0 ||
            height_signed == INT32_MIN) {
            return QW_STATUS_ERR_FORMAT;
        }
        absolute_height = height_signed < 0
            ? (uint64_t)(-(int64_t)height_signed)
            : (uint64_t)height_signed;
        if (absolute_height > UINT32_MAX) return QW_STATUS_ERR_FORMAT;
        info->width = (uint32_t)width_signed;
        info->height = (uint32_t)absolute_height;
        info->top_down = height_signed < 0;
        info->palette_entry_size = 4u;
        if (info->compression != BMP_BI_RGB &&
            info->compression != BMP_BI_BITFIELDS &&
            info->compression != BMP_BI_ALPHABITFIELDS) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if ((info->compression == BMP_BI_BITFIELDS ||
             info->compression == BMP_BI_ALPHABITFIELDS) &&
            info->bit_count != 16u && info->bit_count != 32u) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if (info->top_down && info->compression != BMP_BI_RGB &&
            info->compression != BMP_BI_BITFIELDS) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if (!bmp_load_bitfields(bytes, size, pixel_offset, dib_size,
                                info->compression, info, &metadata_end)) {
            return QW_STATUS_ERR_FORMAT;
        }
    }

    if (planes != 1u) return QW_STATUS_ERR_FORMAT;
    if (!bmp_bit_count_supported(info->bit_count)) return QW_STATUS_ERR_UNSUPPORTED;
    if (!qw_image_dimensions_within_limits(info->width, info->height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }

    if (info->bit_count <= 8u) {
        size_t maximum_entries = (size_t)1u << info->bit_count;
        info->palette_entries = colors_used ? (size_t)colors_used
                                            : maximum_entries;
        if (!info->palette_entries || info->palette_entries > maximum_entries ||
            !qw_checked_size_mul(info->palette_entries,
                                 info->palette_entry_size, &palette_bytes) ||
            metadata_end > pixel_offset ||
            palette_bytes > (size_t)pixel_offset - metadata_end) {
            return QW_STATUS_ERR_FORMAT;
        }
        info->palette = bytes + metadata_end;
        metadata_end += palette_bytes;
    }
    if (metadata_end > pixel_offset || (size_t)pixel_offset > size) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (info->bit_count == 16u || info->bit_count == 32u) {
        if (!bmp_masks_valid(info)) return QW_STATUS_ERR_FORMAT;
    }

    if (!qw_checked_size_mul((size_t)info->width, info->bit_count, &row_bits) ||
        row_bits > SIZE_MAX - 31u) {
        return QW_STATUS_ERR_FORMAT;
    }
    row_stride = ((row_bits + 31u) / 32u) * 4u;
    if (!row_stride ||
        !qw_checked_size_mul(row_stride, (size_t)info->height, &palette_bytes) ||
        palette_bytes > size - (size_t)pixel_offset) {
        return QW_STATUS_ERR_FORMAT;
    }
    info->row_stride = row_stride;
    return QW_STATUS_OK;
}

static uint8_t bmp_component(uint32_t pixel, uint32_t mask)
{
    unsigned shift;
    uint32_t maximum;
    uint32_t value;
    if (!bmp_mask_properties(mask, 32u, &shift, &maximum)) return 0u;
    value = (pixel & mask) >> shift;
    return (uint8_t)((value * 255u + maximum / 2u) / maximum);
}

static uint8_t bmp_composite_white(uint8_t color, uint8_t alpha)
{
    return (uint8_t)(((uint32_t)color * alpha +
                      255u * (255u - alpha) + 127u) / 255u);
}

static int bmp_decode_pixel(const QwBmpInfo *info,
                            const uint8_t *row,
                            uint32_t x,
                            uint8_t *rgb)
{
    uint8_t blue;
    uint8_t green;
    uint8_t red;
    uint8_t alpha = 255u;

    if (info->bit_count <= 8u) {
        uint32_t index;
        const uint8_t *entry;
        if (info->bit_count == 1u) {
            index = (row[x >> 3] >> (7u - (x & 7u))) & 1u;
        } else if (info->bit_count == 4u) {
            uint8_t byte = row[x >> 1];
            index = (x & 1u) ? (byte & 0x0fu) : (byte >> 4);
        } else {
            index = row[x];
        }
        if ((size_t)index >= info->palette_entries) return 0;
        entry = info->palette + (size_t)index * info->palette_entry_size;
        blue = entry[0];
        green = entry[1];
        red = entry[2];
    } else if (info->bit_count == 24u) {
        const uint8_t *pixel = row + (size_t)x * 3u;
        blue = pixel[0];
        green = pixel[1];
        red = pixel[2];
    } else {
        uint32_t packed = info->bit_count == 16u
            ? (uint32_t)qw_read_u16_le(row + (size_t)x * 2u)
            : qw_read_u32_le(row + (size_t)x * 4u);
        red = bmp_component(packed, info->red_mask);
        green = bmp_component(packed, info->green_mask);
        blue = bmp_component(packed, info->blue_mask);
        if (info->alpha_mask) alpha = bmp_component(packed, info->alpha_mask);
    }
    rgb[0] = bmp_composite_white(red, alpha);
    rgb[1] = bmp_composite_white(green, alpha);
    rgb[2] = bmp_composite_white(blue, alpha);
    return 1;
}

int qw_bmp_probe(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    return bytes && size >= 2u && bytes[0] == 'B' && bytes[1] == 'M';
}

int qw_bmp_decode_page(const void *data,
                       size_t size,
                       QwPageImage *out_page)
{
    QwBmpInfo info;
    uint8_t *pixels = NULL;
    size_t pixel_count;
    size_t output_size;
    int rc;

    if (!data || !size || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    rc = bmp_parse_header((const uint8_t *)data, size, &info);
    if (rc != QW_STATUS_OK) return rc;
    if (!qw_checked_size_mul((size_t)info.width, (size_t)info.height,
                             &pixel_count) ||
        !qw_checked_size_mul(pixel_count, 3u, &output_size)) {
        return QW_STATUS_ERR_FORMAT;
    }
    pixels = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!pixels) return QW_STATUS_ERR_OUT_OF_MEMORY;

    for (uint32_t y = 0u; y < info.height; ++y) {
        uint32_t source_y = info.top_down ? y : info.height - 1u - y;
        const uint8_t *row = info.bytes + info.pixel_offset +
                             (size_t)source_y * info.row_stride;
        uint8_t *destination = pixels + (size_t)y * info.width * 3u;
        for (uint32_t x = 0u; x < info.width; ++x) {
            if (!bmp_decode_pixel(&info, row, x,
                                  destination + (size_t)x * 3u)) {
                free(pixels);
                return QW_STATUS_ERR_FORMAT;
            }
        }
    }

    out_page->data = pixels;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = info.width;
    out_page->height = info.height;
    return QW_STATUS_OK;
}
