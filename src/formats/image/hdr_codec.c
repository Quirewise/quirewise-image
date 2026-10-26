#include "image_internal.h"

#include "../../core/byte_utils.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    HDR_HEADER_LIMIT = 1024 * 1024,
    HDR_LINE_CAPACITY = 1024
};

typedef struct QwHdrInfo {
    size_t data_offset;
    uint32_t width;
    uint32_t height;
    uint32_t scan_count;
    uint32_t scan_length;
    char first_axis;
    char second_axis;
    int first_positive;
    int second_positive;
} QwHdrInfo;

static int hdr_signature(const uint8_t *bytes, size_t size)
{
    return bytes &&
           ((size >= 10u && memcmp(bytes, "#?RADIANCE", 10u) == 0) ||
            (size >= 6u && memcmp(bytes, "#?RGBE", 6u) == 0));
}

static int hdr_read_line(const uint8_t *bytes,
                         size_t size,
                         size_t *position,
                         char *line,
                         size_t capacity,
                         size_t *line_length)
{
    size_t written = 0u;
    int overflow = 0;
    if (!bytes || !position || !line || capacity == 0u || !line_length) {
        return 0;
    }
    while (*position < size && bytes[*position] != (uint8_t)'\n') {
        uint8_t value = bytes[(*position)++];
        if (written + 1u < capacity) {
            line[written++] = (char)value;
        } else {
            overflow = 1;
        }
    }
    if (*position >= size) return 0;
    ++*position;
    if (written && line[written - 1u] == '\r') --written;
    line[written] = '\0';
    *line_length = written;
    return overflow ? 2 : 1;
}

static int hdr_parse_unsigned(const char *token,
                              size_t length,
                              uint32_t *out_value)
{
    uint32_t value = 0u;
    if (!token || !length || !out_value) return 0;
    for (size_t i = 0u; i < length; ++i) {
        unsigned digit;
        if (token[i] < '0' || token[i] > '9') return 0;
        digit = (unsigned)(token[i] - '0');
        if (value > (UINT32_MAX - digit) / 10u) return 0;
        value = value * 10u + digit;
    }
    if (!value) return 0;
    *out_value = value;
    return 1;
}

static int hdr_parse_resolution(const char *line,
                                size_t length,
                                QwHdrInfo *info)
{
    const char *tokens[4];
    size_t token_lengths[4];
    size_t count = 0u;
    size_t position = 0u;
    uint32_t first_count;
    uint32_t second_count;

    while (position < length) {
        while (position < length &&
               qw_ascii_isspace((unsigned char)line[position])) {
            ++position;
        }
        if (position == length) break;
        if (count == 4u) return 0;
        tokens[count] = line + position;
        while (position < length &&
               !qw_ascii_isspace((unsigned char)line[position])) {
            ++position;
        }
        token_lengths[count] = (size_t)(line + position - tokens[count]);
        ++count;
    }
    if (count != 4u || token_lengths[0] != 2u || token_lengths[2] != 2u ||
        (tokens[0][0] != '+' && tokens[0][0] != '-') ||
        (tokens[2][0] != '+' && tokens[2][0] != '-') ||
        (tokens[0][1] != 'X' && tokens[0][1] != 'Y') ||
        (tokens[2][1] != 'X' && tokens[2][1] != 'Y') ||
        tokens[0][1] == tokens[2][1] ||
        !hdr_parse_unsigned(tokens[1], token_lengths[1], &first_count) ||
        !hdr_parse_unsigned(tokens[3], token_lengths[3], &second_count)) {
        return 0;
    }
    info->first_axis = tokens[0][1];
    info->second_axis = tokens[2][1];
    info->first_positive = tokens[0][0] == '+';
    info->second_positive = tokens[2][0] == '+';
    info->scan_count = first_count;
    info->scan_length = second_count;
    if (info->first_axis == 'X') {
        info->width = first_count;
        info->height = second_count;
    } else {
        info->height = first_count;
        info->width = second_count;
    }
    return qw_image_dimensions_within_limits(info->width, info->height);
}

static int hdr_parse_header(const uint8_t *bytes, size_t size, QwHdrInfo *info)
{
    char line[HDR_LINE_CAPACITY];
    size_t line_length;
    size_t position = 0u;
    int format_seen = 0;
    int header_ended = 0;
    int rc;

    if (!info || !hdr_signature(bytes, size)) return QW_STATUS_ERR_FORMAT;
    rc = hdr_read_line(bytes, size, &position, line, sizeof(line),
                       &line_length);
    if (rc != 1 ||
        (strcmp(line, "#?RADIANCE") != 0 && strcmp(line, "#?RGBE") != 0)) {
        return QW_STATUS_ERR_FORMAT;
    }
    while (position <= size && position <= HDR_HEADER_LIMIT) {
        rc = hdr_read_line(bytes, size, &position, line, sizeof(line),
                           &line_length);
        if (rc == 0) return QW_STATUS_ERR_FORMAT;
        if (rc == 2) continue;
        if (!line_length) {
            header_ended = 1;
            break;
        }
        if (strncmp(line, "FORMAT=", 7u) == 0) {
            if (format_seen) return QW_STATUS_ERR_FORMAT;
            format_seen = 1;
            if (strcmp(line + 7u, "32-bit_rle_rgbe") != 0) {
                return QW_STATUS_ERR_UNSUPPORTED;
            }
        }
    }
    if (!header_ended || !format_seen || position > size ||
        position > HDR_HEADER_LIMIT) {
        return QW_STATUS_ERR_FORMAT;
    }
    rc = hdr_read_line(bytes, size, &position, line, sizeof(line),
                       &line_length);
    if (rc != 1 || rc == 2 ||
        !hdr_parse_resolution(line, line_length, info)) {
        return QW_STATUS_ERR_FORMAT;
    }
    info->data_offset = position;
    return QW_STATUS_OK;
}

static uint32_t hdr_oriented_coordinate(uint32_t coordinate,
                                        uint32_t length,
                                        char axis,
                                        int positive)
{
    if (axis == 'X') return positive ? coordinate : length - 1u - coordinate;
    return positive ? length - 1u - coordinate : coordinate;
}

static uint8_t hdr_linear_to_srgb(uint8_t mantissa, uint8_t exponent)
{
    float linear;
    float encoded;
    if (!exponent) return 0u;
    linear = ldexpf((float)mantissa + 0.5f, (int)exponent - 136);
    if (!(linear > 0.0f)) return 0u;
    if (linear >= 1.0f) return 255u;
    encoded = powf(linear, 1.0f / 2.2f) * 255.0f + 0.5f;
    return encoded >= 255.0f ? 255u : (uint8_t)encoded;
}

static void hdr_store_rgbe(const QwHdrInfo *info,
                           size_t file_pixel_index,
                           const uint8_t rgbe[4],
                           uint8_t *pixels)
{
    uint32_t first_coordinate =
        (uint32_t)(file_pixel_index / info->scan_length);
    uint32_t second_coordinate =
        (uint32_t)(file_pixel_index % info->scan_length);
    uint32_t first_length = info->first_axis == 'X'
        ? info->width : info->height;
    uint32_t second_length = info->second_axis == 'X'
        ? info->width : info->height;
    uint32_t first = hdr_oriented_coordinate(
        first_coordinate, first_length, info->first_axis,
        info->first_positive);
    uint32_t second = hdr_oriented_coordinate(
        second_coordinate, second_length, info->second_axis,
        info->second_positive);
    uint32_t x = info->first_axis == 'X' ? first : second;
    uint32_t y = info->first_axis == 'Y' ? first : second;
    uint8_t *destination = pixels + ((size_t)y * info->width + x) * 3u;

    destination[0] = hdr_linear_to_srgb(rgbe[0], rgbe[3]);
    destination[1] = hdr_linear_to_srgb(rgbe[1], rgbe[3]);
    destination[2] = hdr_linear_to_srgb(rgbe[2], rgbe[3]);
}

static int hdr_read_new_scanline(const uint8_t *bytes,
                                 size_t size,
                                 size_t *position,
                                 size_t scan_length,
                                 uint8_t *scanline)
{
    for (size_t channel = 0u; channel < 4u; ++channel) {
        size_t output = 0u;
        while (output < scan_length) {
            uint8_t packet;
            size_t count;
            if (*position >= size) return QW_STATUS_ERR_FORMAT;
            packet = bytes[(*position)++];
            if (packet > 128u) {
                uint8_t value;
                count = (size_t)packet - 128u;
                if (!count || count > scan_length - output ||
                    *position >= size) {
                    return QW_STATUS_ERR_FORMAT;
                }
                value = bytes[(*position)++];
                for (size_t i = 0u; i < count; ++i) {
                    scanline[(output + i) * 4u + channel] = value;
                }
            } else {
                count = packet;
                if (!count || count > scan_length - output ||
                    count > size - *position) {
                    return QW_STATUS_ERR_FORMAT;
                }
                for (size_t i = 0u; i < count; ++i) {
                    scanline[(output + i) * 4u + channel] =
                        bytes[(*position)++];
                }
            }
            output += count;
        }
    }
    return QW_STATUS_OK;
}

static int hdr_decode_pixels(const uint8_t *bytes,
                             size_t size,
                             const QwHdrInfo *info,
                             uint8_t *pixels)
{
    size_t pixel_count;
    size_t file_pixel_index = 0u;
    size_t position = info->data_offset;
    size_t scanline_bytes;
    uint8_t *scanline = NULL;
    uint8_t previous[4] = {0u, 0u, 0u, 0u};
    unsigned repeat_shift = 0u;
    int previous_valid = 0;
    int rc = QW_STATUS_ERR_FORMAT;

    if (!qw_checked_size_mul((size_t)info->width, (size_t)info->height,
                             &pixel_count) ||
        !qw_checked_size_mul((size_t)info->scan_length, 4u,
                             &scanline_bytes)) {
        return QW_STATUS_ERR_FORMAT;
    }
    scanline = (uint8_t *)malloc(scanline_bytes ? scanline_bytes : 1u);
    if (!scanline) return QW_STATUS_ERR_OUT_OF_MEMORY;

    while (file_pixel_index < pixel_count) {
        size_t scan_position = file_pixel_index % info->scan_length;
        uint8_t rgbe[4];
        int has_sample = 0;

        if (scan_position == 0u && info->scan_length >= 8u &&
            info->scan_length < 32768u) {
            uint16_t encoded_length;
            if (size - position < 4u) goto cleanup;
            rgbe[0] = bytes[position++];
            rgbe[1] = bytes[position++];
            rgbe[2] = bytes[position++];
            rgbe[3] = bytes[position++];
            encoded_length = (uint16_t)(((uint16_t)rgbe[2] << 8) | rgbe[3]);
            if (rgbe[0] == 2u && rgbe[1] == 2u &&
                (rgbe[2] & 0x80u) == 0u &&
                encoded_length == info->scan_length) {
                rc = hdr_read_new_scanline(bytes, size, &position,
                                           info->scan_length, scanline);
                if (rc != QW_STATUS_OK) goto cleanup;
                for (size_t i = 0u; i < info->scan_length; ++i) {
                    const uint8_t *sample = scanline + i * 4u;
                    hdr_store_rgbe(info, file_pixel_index + i, sample, pixels);
                    memcpy(previous, sample, sizeof(previous));
                }
                file_pixel_index += info->scan_length;
                previous_valid = 1;
                repeat_shift = 0u;
                continue;
            }
            has_sample = 1;
        }

        if (!has_sample) {
            if (size - position < sizeof(rgbe)) goto cleanup;
            memcpy(rgbe, bytes + position, sizeof(rgbe));
            position += sizeof(rgbe);
        }
        if (rgbe[0] == 1u && rgbe[1] == 1u && rgbe[2] == 1u) {
            uint64_t repeat_count;
            if (!previous_valid || repeat_shift >= 32u) goto cleanup;
            repeat_count = (uint64_t)rgbe[3] << repeat_shift;
            if (!repeat_count || repeat_count > pixel_count - file_pixel_index) {
                goto cleanup;
            }
            for (size_t i = 0u; i < (size_t)repeat_count; ++i) {
                hdr_store_rgbe(info, file_pixel_index++, previous, pixels);
            }
            if (repeat_shift > 24u) goto cleanup;
            repeat_shift += 8u;
        } else {
            hdr_store_rgbe(info, file_pixel_index++, rgbe, pixels);
            memcpy(previous, rgbe, sizeof(previous));
            previous_valid = 1;
            repeat_shift = 0u;
        }
    }
    rc = QW_STATUS_OK;

cleanup:
    free(scanline);
    return rc;
}

int qw_hdr_probe(const void *data, size_t size)
{
    return hdr_signature((const uint8_t *)data, size);
}

int qw_hdr_decode_page(const void *data,
                       size_t size,
                       QwPageImage *out_page)
{
    QwHdrInfo info;
    uint8_t *pixels;
    size_t pixel_count;
    size_t output_size;
    int rc;

    if (!data || !size || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    rc = hdr_parse_header((const uint8_t *)data, size, &info);
    if (rc != QW_STATUS_OK) return rc;
    if (!qw_checked_size_mul((size_t)info.width, (size_t)info.height,
                             &pixel_count) ||
        !qw_checked_size_mul(pixel_count, 3u, &output_size)) {
        return QW_STATUS_ERR_FORMAT;
    }
    pixels = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!pixels) return QW_STATUS_ERR_OUT_OF_MEMORY;
    rc = hdr_decode_pixels((const uint8_t *)data, size, &info, pixels);
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
