#include "image_internal.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PSD_PSB_HEADER_SIZE 26u
#define PSD_PSB_MAX_PIXELS 20000000u

typedef struct QwPsImageHeader {
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t compression;
    size_t pixel_count;
    size_t composite_offset;
    size_t row_table_offset;
    size_t row_table_count;
    size_t row_length_size;
    size_t row_data_offset;
} QwPsImageHeader;




static int ps_skip_section(const uint8_t *bytes,
                           size_t size,
                           size_t *position,
                           int is_64_bit)
{
    uint64_t length;
    size_t width = is_64_bit ? 8u : 4u;
    if (*position > size || width > size - *position) return 0;
    length = is_64_bit ? qw_read_u64_be(bytes + *position)
                       : qw_read_u32_be(bytes + *position);
    *position += width;
    if (length > SIZE_MAX || (size_t)length > size - *position) return 0;
    *position += (size_t)length;
    return 1;
}

static int ps_read_header(const void *data,
                          size_t size,
                          uint16_t expected_version,
                          QwPsImageHeader *header)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t position = PSD_PSB_HEADER_SIZE;
    uint16_t version;
    uint16_t depth;
    uint16_t color_mode;
    uint64_t pixels;
    if (!bytes || !header || size < PSD_PSB_HEADER_SIZE ||
        memcmp(bytes, "8BPS", 4u) != 0) {
        return 0;
    }
    memset(header, 0, sizeof(*header));
    version = qw_read_u16_be(bytes + 4u);
    header->channels = qw_read_u16_be(bytes + 12u);
    header->height = qw_read_u32_be(bytes + 14u);
    header->width = qw_read_u32_be(bytes + 18u);
    depth = qw_read_u16_be(bytes + 22u);
    color_mode = qw_read_u16_be(bytes + 24u);
    if (version != expected_version || header->channels < 3u ||
        header->channels > 4u ||
        depth != 8u || color_mode != 3u || !header->width || !header->height) {
        return 0;
    }
    pixels = (uint64_t)header->width * header->height;
    if (!pixels || pixels > PSD_PSB_MAX_PIXELS || pixels > SIZE_MAX / 3u) return 0;
    header->pixel_count = (size_t)pixels;
    if (!ps_skip_section(bytes, size, &position, 0) ||
        !ps_skip_section(bytes, size, &position, 0) ||
        !ps_skip_section(bytes, size, &position, version == 2u) ||
        position > size || size - position < 2u) {
        return 0;
    }
    header->composite_offset = position;
    header->compression = qw_read_u16_be(bytes + position);
    position += 2u;
    if (header->compression == 0u) {
        size_t expected;
        if (header->pixel_count > SIZE_MAX / header->channels) return 0;
        expected = header->pixel_count * header->channels;
        return expected == size - position;
    }
    if (header->compression != 1u ||
        header->height > SIZE_MAX / header->channels) {
        return 0;
    }
    header->row_table_count = (size_t)header->height * header->channels;
    header->row_length_size = version == 2u ? 4u : 2u;
    if (header->row_table_count >
        (SIZE_MAX - position) / header->row_length_size) {
        return 0;
    }
    header->row_table_offset = position;
    header->row_data_offset =
        position + header->row_table_count * header->row_length_size;
    if (header->row_data_offset > size) return 0;
    {
        size_t data_position = header->row_data_offset;
        for (size_t i = 0u; i < header->row_table_count; ++i) {
            const uint8_t *entry = bytes + header->row_table_offset +
                                   i * header->row_length_size;
            size_t row_length = header->row_length_size == 2u
                ? qw_read_u16_be(entry) : qw_read_u32_be(entry);
            if (!row_length || (size_t)row_length > size - data_position) return 0;
            data_position += row_length;
        }
        return data_position == size;
    }
}

static int ps_image_signature(const void *data,
                              size_t size,
                              uint16_t version)
{
    const uint8_t *bytes = (const uint8_t *)data;
    return bytes && size >= 6u && memcmp(bytes, "8BPS", 4u) == 0 &&
           qw_read_u16_be(bytes + 4u) == version;
}

int qw_psb_probe(const void *data, size_t size)
{
    return ps_image_signature(data, size, 2u);
}

int qw_psd_probe(const void *data, size_t size)
{
    return ps_image_signature(data, size, 1u);
}

static int ps_decode_packbits(const uint8_t *source,
                              size_t source_size,
                              uint8_t *destination,
                              size_t width)
{
    size_t input = 0u;
    size_t output = 0u;
    while (input < source_size) {
        uint8_t encoded_control = source[input++];
        int control = encoded_control < 0x80u
                          ? (int)encoded_control
                          : (int)encoded_control - 256;
        if (control >= 0) {
            size_t count = (size_t)control + 1u;
            if (count > source_size - input || count > width - output) return 0;
            memcpy(destination + output, source + input, count);
            input += count;
            output += count;
        } else if (control != -128) {
            size_t count = (size_t)(1 - control);
            uint8_t value;
            if (input >= source_size || count > width - output) return 0;
            value = source[input++];
            memset(destination + output, value, count);
            output += count;
        }
    }
    return output == width;
}

static int ps_get_row(const uint8_t *bytes,
                      const QwPsImageHeader *header,
                      size_t channel,
                      size_t row,
                      size_t *data_offset,
                      uint8_t *scratch,
                      const uint8_t **out_row)
{
    if (header->compression == 0u) {
        size_t row_index = channel * header->height + row;
        *out_row = bytes + header->composite_offset + 2u +
                   row_index * header->width;
        return 1;
    }
    {
        size_t table_index = channel * header->height + row;
        const uint8_t *entry = bytes + header->row_table_offset +
                               table_index * header->row_length_size;
        size_t row_length = header->row_length_size == 2u
            ? qw_read_u16_be(entry) : qw_read_u32_be(entry);
        const uint8_t *source = bytes + *data_offset;
        *data_offset += row_length;
        if (!ps_decode_packbits(source, row_length, scratch, header->width)) {
            return 0;
        }
        *out_row = scratch;
        return 1;
    }
}

static int ps_decode_page(const void *data,
                          size_t size,
                          uint16_t version,
                          QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwPsImageHeader header;
    uint8_t *pixels = NULL;
    uint8_t *row_buffer = NULL;
    size_t output_size;
    size_t rle_data_offset;
    int rc = QW_STATUS_ERR_FORMAT;
    if (!out_page || !ps_read_header(data, size, version, &header)) {
        return QW_STATUS_ERR_FORMAT;
    }
    memset(out_page, 0, sizeof(*out_page));
    output_size = header.pixel_count * 3u;
    pixels = (uint8_t *)malloc(output_size);
    if (!pixels) return QW_STATUS_ERR_OUT_OF_MEMORY;
    if (header.compression == 1u) {
        row_buffer = (uint8_t *)malloc(header.width);
        if (!row_buffer) {
            rc = QW_STATUS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
    }
    rle_data_offset = header.row_data_offset;
    for (size_t channel = 0u; channel < header.channels; ++channel) {
        for (size_t y = 0u; y < header.height; ++y) {
            const uint8_t *row;
            if (!ps_get_row(bytes, &header, channel, y, &rle_data_offset,
                            row_buffer, &row)) {
                goto cleanup;
            }
            if (channel < 3u) {
                for (size_t x = 0u; x < header.width; ++x) {
                    pixels[(y * header.width + x) * 3u + channel] = row[x];
                }
            } else {
                for (size_t x = 0u; x < header.width; ++x) {
                    uint32_t alpha = row[x];
                    for (size_t color = 0u; color < 3u; ++color) {
                        size_t output = (y * header.width + x) * 3u + color;
                        pixels[output] = (uint8_t)(
                            (pixels[output] * alpha + 255u * (255u - alpha) + 127u) /
                            255u);
                    }
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

int qw_psd_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    return ps_decode_page(data, size, 1u, out_page);
}

int qw_psb_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    return ps_decode_page(data, size, 2u, out_page);
}
