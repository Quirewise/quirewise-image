#include "image_internal.h"

#include "../../core/byte_utils.h"

#include <stdlib.h>
#include <string.h>

enum {
    PIC_HEADER_SIZE = 104,
    PIC_PACKET_LIMIT = 10,
    PIC_CHANNEL_MASK = 0xf0
};

typedef struct QwPicPacket {
    uint8_t compression;
    uint8_t channels;
} QwPicPacket;

typedef struct QwPicLayout {
    QwPicPacket packets[PIC_PACKET_LIMIT];
    size_t packet_count;
    size_t data_offset;
    size_t output_size;
    size_t row_size;
    uint32_t width;
    uint32_t height;
    int has_alpha;
} QwPicLayout;

static int pic_signature(const uint8_t *bytes, size_t size)
{
    static const uint8_t magic[4] = {0x53u, 0x80u, 0xf6u, 0x34u};
    return bytes && size >= sizeof(magic) &&
           memcmp(bytes, magic, sizeof(magic)) == 0;
}

static int pic_read_channels(const uint8_t *bytes,
                             size_t size,
                             size_t *position,
                             uint8_t channels,
                             uint8_t values[4])
{
    for (size_t component = 0u; component < 4u; ++component) {
        uint8_t bit = (uint8_t)(0x80u >> component);
        if (channels & bit) {
            if (*position >= size) return 0;
            values[component] = bytes[(*position)++];
        }
    }
    return 1;
}

static void pic_apply_channels(uint8_t destination[4],
                               uint8_t channels,
                               const uint8_t values[4])
{
    for (size_t component = 0u; component < 4u; ++component) {
        uint8_t bit = (uint8_t)(0x80u >> component);
        if (channels & bit) destination[component] = values[component];
    }
}

static uint8_t pic_composite_white(uint8_t color, uint8_t alpha)
{
    return (uint8_t)(((uint32_t)color * alpha +
                      255u * (255u - alpha) + 127u) / 255u);
}

static int pic_parse_layout(const uint8_t *bytes,
                            size_t size,
                            QwPicLayout *layout)
{
    size_t position = PIC_HEADER_SIZE;
    size_t pixel_count;

    if (!bytes || !layout || !pic_signature(bytes, size) ||
        size < PIC_HEADER_SIZE ||
        memcmp(bytes + 88u, "PICT", 4u) != 0) {
        return QW_STATUS_ERR_FORMAT;
    }
    memset(layout, 0, sizeof(*layout));
    layout->width = qw_read_u16_be(bytes + 92u);
    layout->height = qw_read_u16_be(bytes + 94u);
    if (!layout->width || !layout->height) return QW_STATUS_ERR_FORMAT;
    if (!qw_image_dimensions_within_limits(layout->width, layout->height) ||
        !qw_checked_size_mul((size_t)layout->width,
                             (size_t)layout->height, &pixel_count) ||
        !qw_checked_size_mul(pixel_count, 3u, &layout->output_size) ||
        !qw_checked_size_mul((size_t)layout->width, 4u,
                             &layout->row_size)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }

    for (;;) {
        uint8_t chained;
        uint8_t sample_size;
        uint8_t compression;
        uint8_t channels;
        if (layout->packet_count == PIC_PACKET_LIMIT ||
            size - position < 4u) {
            return QW_STATUS_ERR_FORMAT;
        }
        chained = bytes[position++];
        sample_size = bytes[position++];
        compression = bytes[position++];
        channels = bytes[position++];
        if (sample_size != 8u || compression > 2u || !channels ||
            (channels & (uint8_t)~PIC_CHANNEL_MASK) != 0u) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        layout->packets[layout->packet_count].compression = compression;
        layout->packets[layout->packet_count].channels = channels;
        ++layout->packet_count;
        layout->has_alpha |= (channels & 0x10u) != 0u;
        if (!chained) break;
    }
    layout->data_offset = position;
    return QW_STATUS_OK;
}

static int pic_decode_pixels(const uint8_t *bytes,
                             size_t size,
                             const QwPicLayout *layout,
                             QwPageImage *out_page)
{
    size_t position = layout->data_offset;
    uint8_t *pixels = NULL;
    uint8_t *row = NULL;
    int rc = QW_STATUS_ERR_FORMAT;

    if (out_page) {
        pixels = (uint8_t *)malloc(layout->output_size ?
                                   layout->output_size : 1u);
        row = (uint8_t *)malloc(layout->row_size ? layout->row_size : 1u);
        if (!pixels || !row) {
            rc = QW_STATUS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
    }

    for (uint32_t y = 0u; y < layout->height; ++y) {
        if (row) memset(row, 0xff, layout->row_size);
        for (size_t packet_index = 0u;
             packet_index < layout->packet_count;
             ++packet_index) {
            const QwPicPacket *packet = &layout->packets[packet_index];
            size_t x = 0u;
            while (x < layout->width) {
                uint8_t count_byte;
                size_t count;
                int repeated = 0;
                uint8_t values[4] = {255u, 255u, 255u, 255u};

                if (packet->compression == 0u) {
                    count = 1u;
                } else {
                    if (position >= size) goto cleanup;
                    count_byte = bytes[position++];
                    if (packet->compression == 1u) {
                        count = count_byte;
                        repeated = 1;
                    } else if (count_byte >= 128u) {
                        repeated = 1;
                        if (count_byte == 128u) {
                            if (size - position < 2u) goto cleanup;
                            count = qw_read_u16_be(bytes + position);
                            position += 2u;
                        } else {
                            count = (size_t)count_byte - 127u;
                        }
                    } else {
                        count = (size_t)count_byte + 1u;
                    }
                    if (!count || count > (size_t)layout->width - x)
                        goto cleanup;
                }

                if (repeated) {
                    if (!pic_read_channels(bytes, size, &position,
                                           packet->channels, values)) {
                        goto cleanup;
                    }
                    if (row) {
                        for (size_t i = 0u; i < count; ++i) {
                            pic_apply_channels(row + (x + i) * 4u,
                                               packet->channels, values);
                        }
                    }
                    x += count;
                } else {
                    for (size_t i = 0u; i < count; ++i) {
                        memset(values, 0xff, sizeof(values));
                        if (!pic_read_channels(bytes, size, &position,
                                               packet->channels, values)) {
                            goto cleanup;
                        }
                        if (row) {
                            pic_apply_channels(row + (x + i) * 4u,
                                               packet->channels, values);
                        }
                    }
                    x += count;
                }
            }
        }

        if (row && pixels) {
            for (uint32_t x = 0u; x < layout->width; ++x) {
                const uint8_t *source = row + (size_t)x * 4u;
                uint8_t alpha = layout->has_alpha ? source[3] : 255u;
                uint8_t *destination = pixels +
                    ((size_t)y * layout->width + x) * 3u;
                destination[0] = pic_composite_white(source[0], alpha);
                destination[1] = pic_composite_white(source[1], alpha);
                destination[2] = pic_composite_white(source[2], alpha);
            }
        }
    }

    if (out_page) {
        out_page->data = pixels;
        out_page->size = layout->output_size;
        out_page->kind = QW_PAGE_IMAGE_RGB24;
        out_page->width = layout->width;
        out_page->height = layout->height;
        pixels = NULL;
    }
    rc = QW_STATUS_OK;

cleanup:
    free(row);
    free(pixels);
    return rc;
}

int qw_pic_probe(const void *data, size_t size)
{
    return pic_signature((const uint8_t *)data, size);
}

int qw_pic_decode_page(const void *data,
                       size_t size,
                       QwPageImage *out_page)
{
    if (!data || !size || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    {
        QwPicLayout layout;
        int rc = pic_parse_layout((const uint8_t *)data, size, &layout);
        if (rc != QW_STATUS_OK) return rc;
        return pic_decode_pixels((const uint8_t *)data, size, &layout,
                                 out_page);
    }
}
