#include "image_internal.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SGI_HEADER_SIZE 512u
#define SGI_MAX_PIXELS 20000000u

typedef struct QwSgiHeader {
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t storage;
    uint32_t colormap;
    size_t pixel_count;
    size_t row_bytes;
    size_t table_count;
} QwSgiHeader;



static int sgi_read_header(const void *data, size_t size, QwSgiHeader *header)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t dimension;
    uint32_t bytes_per_channel;
    uint64_t pixels;

    if (!bytes || !header || size < SGI_HEADER_SIZE ||
        qw_read_u16_be(bytes) != 474u) {
        return 0;
    }
    header->storage = bytes[2];
    bytes_per_channel = bytes[3];
    dimension = qw_read_u16_be(bytes + 4u);
    header->width = qw_read_u16_be(bytes + 6u);
    header->height = qw_read_u16_be(bytes + 8u);
    header->channels = qw_read_u16_be(bytes + 10u);
    header->colormap = qw_read_u32_be(bytes + 104u);

    if ((header->storage != 0u && header->storage != 1u) ||
        bytes_per_channel != 1u || dimension < 1u || dimension > 3u ||
        !header->width || !header->height || !header->channels ||
        header->channels > 4u || (header->colormap != 0u &&
                                  header->colormap != 1u) ||
        (header->colormap == 1u && header->channels != 1u) ||
        (dimension == 1u && (header->height != 1u || header->channels != 1u)) ||
        (dimension == 2u && header->channels != 1u)) {
        return 0;
    }
    pixels = (uint64_t)header->width * header->height;
    if (pixels == 0u || pixels > SGI_MAX_PIXELS ||
        pixels > SIZE_MAX / 3u ||
        (size_t)header->width > SIZE_MAX / header->channels) {
        return 0;
    }
    header->pixel_count = (size_t)pixels;
    header->row_bytes = (size_t)header->width;
    header->table_count = (size_t)header->height * header->channels;

    if (header->storage == 0u) {
        size_t image_bytes;
        if (header->pixel_count > SIZE_MAX / header->channels) return 0;
        image_bytes = header->pixel_count * header->channels;
        return size == SGI_HEADER_SIZE + image_bytes;
    }
    if (header->table_count > (SIZE_MAX - SGI_HEADER_SIZE) / 8u) return 0;
    {
        size_t table_end = SGI_HEADER_SIZE + header->table_count * 8u;
        if (size < table_end) return 0;
        for (size_t i = 0u; i < header->table_count; ++i) {
            uint32_t offset = qw_read_u32_be(bytes + SGI_HEADER_SIZE + i * 4u);
            uint32_t length = qw_read_u32_be(bytes + SGI_HEADER_SIZE +
                                       header->table_count * 4u + i * 4u);
            if ((size_t)offset < table_end || !length ||
                (size_t)offset > size || (size_t)length > size - offset) {
                return 0;
            }
        }
    }
    return 1;
}

int qw_sgi_probe(const void *data, size_t size)
{
    QwSgiHeader header;
    return sgi_read_header(data, size, &header);
}

static int sgi_decode_rle_row(const uint8_t *source,
                              size_t source_size,
                              uint8_t *destination,
                              size_t width)
{
    size_t source_position = 0u;
    size_t output_position = 0u;
    int terminated = 0;
    while (source_position < source_size) {
        uint8_t control = source[source_position++];
        size_t count = control & 0x7fu;
        if (!count) {
            terminated = 1;
            break;
        }
        if (count > width - output_position) return 0;
        if (control & 0x80u) {
            if (count > source_size - source_position) return 0;
            memcpy(destination + output_position, source + source_position, count);
            source_position += count;
            output_position += count;
        } else {
            uint8_t value;
            if (source_position >= source_size) return 0;
            value = source[source_position++];
            memset(destination + output_position, value, count);
            output_position += count;
        }
    }
    return terminated && output_position == width && source_position == source_size;
}

static const uint8_t *sgi_get_row(const uint8_t *bytes,
                                  size_t size,
                                  const QwSgiHeader *header,
                                  size_t channel,
                                  size_t row,
                                  uint8_t *scratch)
{
    if (header->storage == 0u) {
        size_t row_index = channel * header->height + row;
        size_t offset = SGI_HEADER_SIZE + row_index * header->row_bytes;
        return offset <= size && header->row_bytes <= size - offset
                   ? bytes + offset : NULL;
    }
    {
        size_t table_index = channel * header->height + row;
        size_t table_count = header->table_count;
        size_t offset = qw_read_u32_be(bytes + SGI_HEADER_SIZE + table_index * 4u);
        size_t length = qw_read_u32_be(bytes + SGI_HEADER_SIZE +
                                 table_count * 4u + table_index * 4u);
        if (offset > size || length > size - offset ||
            !sgi_decode_rle_row(bytes + offset, length, scratch,
                                header->width)) {
            return NULL;
        }
        return scratch;
    }
}

int qw_sgi_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwSgiHeader header;
    uint8_t *pixels = NULL;
    uint8_t *row_buffer = NULL;
    size_t output_size;
    int rc = QW_STATUS_ERR_FORMAT;

    if (!out_page || !sgi_read_header(data, size, &header)) {
        return QW_STATUS_ERR_FORMAT;
    }
    memset(out_page, 0, sizeof(*out_page));
    output_size = header.pixel_count * 3u;
    pixels = (uint8_t *)malloc(output_size);
    if (!pixels) return QW_STATUS_ERR_OUT_OF_MEMORY;
    if (header.storage == 1u) {
        row_buffer = (uint8_t *)malloc(header.row_bytes);
        if (!row_buffer) {
            rc = QW_STATUS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
    }

    for (size_t source_y = 0u; source_y < header.height; ++source_y) {
        size_t y = (size_t)header.height - 1u - source_y;
        uint8_t *destination = pixels + y * header.width * 3u;
        size_t color_channels = header.channels == 1u || header.channels == 2u
                                    ? 1u : 3u;
        for (size_t channel = 0u; channel < color_channels; ++channel) {
            const uint8_t *row = sgi_get_row(bytes, size, &header, channel,
                                             source_y, row_buffer);
            if (!row) goto cleanup;
            for (size_t x = 0u; x < header.width; ++x) {
                uint8_t value = row[x];
                if (header.colormap == 1u) {
                    uint8_t r = (uint8_t)(((value & 0x07u) * 255u + 3u) / 7u);
                    uint8_t g = (uint8_t)((((value >> 3) & 0x07u) * 255u + 3u) / 7u);
                    uint8_t b = (uint8_t)(((value >> 6) * 255u + 1u) / 3u);
                    destination[x * 3u] = r;
                    destination[x * 3u + 1u] = g;
                    destination[x * 3u + 2u] = b;
                    continue;
                }
                if (color_channels == 1u) {
                    destination[x * 3u] = value;
                    destination[x * 3u + 1u] = value;
                    destination[x * 3u + 2u] = value;
                } else {
                    destination[x * 3u + channel] = value;
                }
            }
        }
        if (header.channels == 2u || header.channels == 4u) {
            size_t alpha_channel = (size_t)header.channels - 1u;
            const uint8_t *alpha = sgi_get_row(bytes, size, &header,
                                               alpha_channel, source_y,
                                               row_buffer);
            if (!alpha) goto cleanup;
            for (size_t x = 0u; x < header.width; ++x) {
                uint32_t a = alpha[x];
                for (size_t channel = 0u; channel < 3u; ++channel) {
                    uint32_t c = destination[x * 3u + channel];
                    destination[x * 3u + channel] = (uint8_t)(
                        (c * a + 255u * (255u - a) + 127u) / 255u);
                }
            }
        }
    }
    free(row_buffer);
    out_page->data = pixels;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = header.width;
    out_page->height = header.height;
    return QW_STATUS_OK;

cleanup:
    free(row_buffer);
    free(pixels);
    return rc;
}
