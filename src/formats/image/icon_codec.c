#include "image_internal.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct QwIconEntry {
    uint32_t width;
    uint32_t height;
    uint32_t image_size;
    uint32_t image_offset;
} QwIconEntry;

static uint8_t icon_composite_white(uint8_t color, uint8_t alpha)
{
    uint32_t value = (uint32_t)color * alpha +
                     255u * (255u - alpha) + 127u;
    return (uint8_t)(value / 255u);
}






static int icon_parse_directory(const void *data,
                                size_t size,
                                int expected_kind,
                                size_t *out_count)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t count;
    size_t directory_size;

    if (!bytes || size < 6u || (expected_kind != QW_ICON_CONTAINER_ICO &&
                                expected_kind != QW_ICON_CONTAINER_CUR) ||
        qw_read_u16_le(bytes) != 0u ||
        qw_read_u16_le(bytes + 2u) != (uint16_t)expected_kind) {
        return 0;
    }
    count = qw_read_u16_le(bytes + 4u);
    if (!count || count > (SIZE_MAX - 6u) / 16u) {
        return 0;
    }
    directory_size = 6u + count * 16u;
    if (directory_size > size) {
        return 0;
    }
    for (size_t i = 0u; i < count; ++i) {
        const uint8_t *entry = bytes + 6u + i * 16u;
        uint32_t image_size = qw_read_u32_le(entry + 8u);
        uint32_t image_offset = qw_read_u32_le(entry + 12u);
        if (entry[3] != 0u || !image_size || image_offset < directory_size ||
            (size_t)image_offset > size ||
            (size_t)image_size > size - (size_t)image_offset) {
            return 0;
        }
    }
    if (out_count) {
        *out_count = count;
    }
    return 1;
}

int qw_ico_probe(const void *data, size_t size)
{
    return icon_parse_directory(data, size, QW_ICON_CONTAINER_ICO, NULL);
}

int qw_cur_probe(const void *data, size_t size)
{
    return icon_parse_directory(data, size, QW_ICON_CONTAINER_CUR, NULL);
}

int qw_icon_page_count(const void *data,
                       size_t size,
                       int kind,
                       size_t *out_count)
{
    if (!out_count) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_count = 0u;
    if (!icon_parse_directory(data, size, kind, out_count)) {
        return QW_STATUS_ERR_FORMAT;
    }
    return QW_STATUS_OK;
}

static int icon_get_entry(const uint8_t *data,
                          size_t size,
                          int kind,
                          size_t index,
                          QwIconEntry *out_entry)
{
    size_t count;
    const uint8_t *entry;

    if (!out_entry || !icon_parse_directory(data, size, kind, &count) ||
        index >= count) {
        return 0;
    }
    entry = data + 6u + index * 16u;
    out_entry->width = entry[0] ? entry[0] : 256u;
    out_entry->height = entry[1] ? entry[1] : 256u;
    out_entry->image_size = qw_read_u32_le(entry + 8u);
    out_entry->image_offset = qw_read_u32_le(entry + 12u);
    return 1;
}

static int icon_decode_png(const uint8_t *data,
                           size_t size,
                           const QwIconEntry *entry,
                           QwPageImage *out_page)
{
    QwPageImage decoded = {0};
    uint8_t *rgb = NULL;
    size_t pixels;
    size_t output_size;
    int rc;

    rc = qw_png_decode_page(data, size, &decoded);
    if (rc != QW_STATUS_OK) return rc;
    if (decoded.width != entry->width || decoded.height != entry->height ||
        !qw_checked_size_mul((size_t)decoded.width, (size_t)decoded.height,
                             &pixels) ||
        !qw_checked_size_mul(pixels, 3u, &output_size)) {
        qw_image_release_page(&decoded);
        return QW_STATUS_ERR_DECODE;
    }
    if (decoded.kind == QW_PAGE_IMAGE_RGB24) {
        *out_page = decoded;
        return QW_STATUS_OK;
    }
    if (decoded.kind != QW_PAGE_IMAGE_GRAY8 || decoded.size < pixels) {
        qw_image_release_page(&decoded);
        return QW_STATUS_ERR_FORMAT;
    }
    rgb = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!rgb) {
        qw_image_release_page(&decoded);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0u; i < pixels; ++i) {
        rgb[i * 3u] = decoded.data[i];
        rgb[i * 3u + 1u] = decoded.data[i];
        rgb[i * 3u + 2u] = decoded.data[i];
    }
    out_page->width = decoded.width;
    out_page->height = decoded.height;
    qw_image_release_page(&decoded);
    out_page->data = rgb;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    return QW_STATUS_OK;
}

static uint8_t icon_and_mask_bit(const uint8_t *mask,
                                 size_t row_stride,
                                 uint32_t width,
                                 uint32_t height,
                                 uint32_t x,
                                 uint32_t y)
{
    if (!mask || !row_stride || !width || !height || x >= width || y >= height) {
        return 0u;
    }
    size_t row = (size_t)(height - 1u - y) * row_stride;
    return (uint8_t)((mask[row + x / 8u] >> (7u - (x & 7u))) & 1u);
}

static int icon_decode_dib(const uint8_t *data,
                           size_t size,
                           const QwIconEntry *entry,
                           QwPageImage *out_page)
{
    uint32_t header_size;
    int32_t width;
    int32_t stored_height;
    uint16_t planes;
    uint16_t bits_per_pixel;
    uint32_t compression;
    uint32_t colors_used;
    size_t palette_count = 0u;
    size_t palette_bytes;
    size_t pixel_count;
    size_t output_size;
    size_t row_bits;
    size_t xor_stride;
    size_t xor_size;
    size_t mask_stride;
    size_t mask_size;
    size_t xor_offset;
    size_t mask_offset;
    const uint8_t *mask = NULL;
    uint8_t *rgb = NULL;
    int any_alpha = 0;

    if (!data || !out_page || size < 40u) {
        return QW_STATUS_ERR_FORMAT;
    }
    header_size = qw_read_u32_le(data);
    width = (int32_t)qw_read_u32_le(data + 4u);
    stored_height = (int32_t)qw_read_u32_le(data + 8u);
    planes = qw_read_u16_le(data + 12u);
    bits_per_pixel = qw_read_u16_le(data + 14u);
    compression = qw_read_u32_le(data + 16u);
    colors_used = qw_read_u32_le(data + 32u);
    if (header_size < 40u || header_size > size || width <= 0 ||
        stored_height <= 0 || planes != 1u || compression != 0u ||
        (uint32_t)width != entry->width ||
        (uint32_t)stored_height != entry->height * 2u ||
        (bits_per_pixel != 1u && bits_per_pixel != 4u &&
         bits_per_pixel != 8u && bits_per_pixel != 24u &&
         bits_per_pixel != 32u)) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (bits_per_pixel <= 8u) {
        size_t maximum_palette = (size_t)1u << bits_per_pixel;
        palette_count = colors_used ? (size_t)colors_used : maximum_palette;
        if (!palette_count || palette_count > maximum_palette) {
            return QW_STATUS_ERR_FORMAT;
        }
    } else if (colors_used != 0u) {
        palette_count = (size_t)colors_used;
    }
    if (!qw_checked_size_mul(palette_count, 4u, &palette_bytes) ||
        header_size > size || palette_bytes > size - header_size) {
        return QW_STATUS_ERR_FORMAT;
    }
    xor_offset = (size_t)header_size + palette_bytes;
    if (!qw_checked_size_mul((size_t)entry->width,
                               (size_t)bits_per_pixel, &row_bits) ||
        row_bits > SIZE_MAX - 31u) {
        return QW_STATUS_ERR_FORMAT;
    }
    xor_stride = ((row_bits + 31u) / 32u) * 4u;
    if (!qw_checked_size_mul(xor_stride, (size_t)entry->height,
                               &xor_size) ||
        xor_offset > size || xor_size > size - xor_offset) {
        return QW_STATUS_ERR_FORMAT;
    }
    mask_offset = xor_offset + xor_size;
    mask_stride = (((size_t)entry->width + 31u) / 32u) * 4u;
    if (!qw_checked_size_mul(mask_stride, (size_t)entry->height,
                               &mask_size)) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (mask_size <= size - mask_offset) {
        mask = data + mask_offset;
    } else if (bits_per_pixel != 32u || size != mask_offset) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (!qw_checked_size_mul((size_t)entry->width,
                               (size_t)entry->height, &pixel_count) ||
        !qw_checked_size_mul(pixel_count, 3u, &output_size)) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (bits_per_pixel == 32u) {
        for (uint32_t y = 0u; y < entry->height && !any_alpha; ++y) {
            const uint8_t *row = data + xor_offset +
                (size_t)(entry->height - 1u - y) * xor_stride;
            for (uint32_t x = 0u; x < entry->width; ++x) {
                if (row[(size_t)x * 4u + 3u] != 0u) {
                    any_alpha = 1;
                    break;
                }
            }
        }
    }
    rgb = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!rgb) {
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    for (uint32_t y = 0u; y < entry->height; ++y) {
        const uint8_t *row = data + xor_offset +
            (size_t)(entry->height - 1u - y) * xor_stride;
        for (uint32_t x = 0u; x < entry->width; ++x) {
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            uint8_t alpha = 255u;
            if (bits_per_pixel == 1u || bits_per_pixel == 4u ||
                bits_per_pixel == 8u) {
                size_t palette_index;
                if (bits_per_pixel == 1u) {
                    palette_index = (row[x / 8u] >> (7u - (x & 7u))) & 1u;
                } else if (bits_per_pixel == 4u) {
                    uint8_t packed = row[x / 2u];
                    palette_index = (x & 1u) ? (packed & 0x0fu)
                                                   : (packed >> 4);
                } else {
                    palette_index = row[x];
                }
                if (palette_index >= palette_count) {
                    free(rgb);
                    return QW_STATUS_ERR_FORMAT;
                }
                {
                    const uint8_t *color = data + header_size +
                                           palette_index * 4u;
                    blue = color[0];
                    green = color[1];
                    red = color[2];
                }
            } else {
                size_t bytes_per_pixel = bits_per_pixel / 8u;
                const uint8_t *color = row + (size_t)x * bytes_per_pixel;
                blue = color[0];
                green = color[1];
                red = color[2];
                if (bits_per_pixel == 32u) {
                    alpha = any_alpha ? color[3] : 255u;
                }
            }
            if (!any_alpha && mask &&
                icon_and_mask_bit(mask, mask_stride, entry->width,
                                  entry->height, x, y)) {
                alpha = 0u;
            }
            {
                size_t output = ((size_t)y * entry->width + x) * 3u;
                rgb[output] = icon_composite_white(red, alpha);
                rgb[output + 1u] = icon_composite_white(green, alpha);
                rgb[output + 2u] = icon_composite_white(blue, alpha);
            }
        }
    }
    out_page->data = rgb;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = entry->width;
    out_page->height = entry->height;
    return QW_STATUS_OK;
}

int qw_icon_decode_page(const void *data,
                        size_t size,
                        int kind,
                        size_t page_index,
                        QwPageImage *out_page)
{
    static const uint8_t png_signature[8] = {
        137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u
    };
    const uint8_t *bytes = (const uint8_t *)data;
    QwIconEntry entry;
    const uint8_t *image;
    int rc;

    if (!data || !out_page) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    memset(out_page, 0, sizeof(*out_page));
    if (!icon_get_entry(bytes, size, kind, page_index, &entry)) {
        return QW_STATUS_ERR_FORMAT;
    }
    image = bytes + entry.image_offset;
    if (entry.image_size >= sizeof(png_signature) &&
        memcmp(image, png_signature, sizeof(png_signature)) == 0) {
        rc = icon_decode_png(image, entry.image_size, &entry, out_page);
    } else {
        rc = icon_decode_dib(image, entry.image_size, &entry, out_page);
    }
    return rc;
}
