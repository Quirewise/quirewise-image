#include "image_internal.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define EXR_HEADER_SIZE 8u
#define EXR_MAX_PIXELS 20000000u
#define EXR_MAX_BLOCK_BYTES 268435456u

typedef struct QwExrInfo {
    uint32_t width;
    uint32_t height;
    int32_t x_min;
    int32_t y_min;
    uint8_t compression;
    size_t pixel_count;
    size_t header_end;
    size_t chunk_count;
    size_t lines_per_chunk;
    size_t channel_output[3];
} QwExrInfo;



static int32_t exr_lei32(const uint8_t *data)
{
    uint32_t value = qw_read_u32_le(data);
    int32_t result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

static int exr_read_cstring(const uint8_t *bytes,
                            size_t size,
                            size_t *position,
                            char *out_string,
                            size_t capacity)
{
    size_t start = *position;
    size_t length;
    while (*position < size && bytes[*position] != 0u) ++*position;
    if (*position >= size) return 0;
    length = *position - start;
    if (length >= capacity) return 0;
    memcpy(out_string, bytes + start, length);
    out_string[length] = '\0';
    ++*position;
    return 1;
}

static int exr_parse_channels(const uint8_t *bytes,
                              size_t start,
                              size_t length,
                              size_t output_indices[3])
{
    size_t position = start;
    size_t end = start + length;
    size_t channel_count = 0u;
    int have_r = 0;
    int have_g = 0;
    int have_b = 0;
    while (position < end && bytes[position] != 0u) {
        char name[256];
        uint32_t pixel_type;
        uint32_t x_sampling;
        uint32_t y_sampling;
        size_t channel_index = channel_count;
        if (!exr_read_cstring(bytes, end, &position, name, sizeof(name)) ||
            end - position < 16u) return 0;
        pixel_type = qw_read_u32_le(bytes + position);
        position += 4u;
        if (bytes[position] > 1u || bytes[position + 1u] != 0u ||
            bytes[position + 2u] != 0u || bytes[position + 3u] != 0u) {
            return 0;
        }
        position += 4u;
        x_sampling = qw_read_u32_le(bytes + position);
        y_sampling = qw_read_u32_le(bytes + position + 4u);
        position += 8u;
        if (channel_count >= 3u || pixel_type != 2u ||
            x_sampling != 1u || y_sampling != 1u) return 0;
        if (strcmp(name, "R") == 0) {
            if (have_r) return 0;
            output_indices[0] = channel_index;
            have_r = 1;
        } else if (strcmp(name, "G") == 0) {
            if (have_g) return 0;
            output_indices[1] = channel_index;
            have_g = 1;
        } else if (strcmp(name, "B") == 0) {
            if (have_b) return 0;
            output_indices[2] = channel_index;
            have_b = 1;
        } else {
            return 0;
        }
        ++channel_count;
    }
    return position + 1u == end && bytes[position] == 0u &&
           channel_count == 3u && have_r && have_g && have_b;
}

static int exr_read_info(const void *data, size_t size, QwExrInfo *info)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t position = EXR_HEADER_SIZE;
    uint32_t version;
    uint32_t compression = UINT32_MAX;
    uint32_t line_order = UINT32_MAX;
    int have_data_window = 0;
    int have_channels = 0;
    int32_t x_max = 0;
    int32_t y_max = 0;
    uint64_t pixels;

    if (!bytes || !info || size < EXR_HEADER_SIZE ||
        qw_read_u32_le(bytes) != UINT32_C(20000630) ||
        sizeof(float) != 4u || FLT_RADIX != 2 || FLT_MANT_DIG != 24 ||
        FLT_MAX_EXP != 128) {
        return 0;
    }
    version = qw_read_u32_le(bytes + 4u);
    if (version != 1u && version != 2u) return 0;
    memset(info, 0, sizeof(*info));
    while (position < size) {
        char name[256];
        char type[256];
        uint32_t attribute_size;
        size_t value_offset;
        if (!exr_read_cstring(bytes, size, &position, name, sizeof(name))) return 0;
        if (!name[0]) break;
        if (!exr_read_cstring(bytes, size, &position, type, sizeof(type)) ||
            size - position < 4u) return 0;
        attribute_size = qw_read_u32_le(bytes + position);
        position += 4u;
        value_offset = position;
        if ((size_t)attribute_size > size - position) return 0;
        position += attribute_size;
        if (strcmp(name, "compression") == 0) {
            if (strcmp(type, "compression") != 0 || attribute_size != 1u) return 0;
            compression = bytes[value_offset];
        } else if (strcmp(name, "lineOrder") == 0) {
            if (strcmp(type, "lineOrder") != 0 || attribute_size != 1u) return 0;
            line_order = bytes[value_offset];
        } else if (strcmp(name, "dataWindow") == 0) {
            if (strcmp(type, "box2i") != 0 || attribute_size != 16u) return 0;
            info->x_min = exr_lei32(bytes + value_offset);
            info->y_min = exr_lei32(bytes + value_offset + 4u);
            x_max = exr_lei32(bytes + value_offset + 8u);
            y_max = exr_lei32(bytes + value_offset + 12u);
            have_data_window = 1;
        } else if (strcmp(name, "channels") == 0) {
            if (strcmp(type, "chlist") != 0 || !attribute_size ||
                !exr_parse_channels(bytes, value_offset, attribute_size,
                                    info->channel_output)) return 0;
            have_channels = 1;
        } else if (strcmp(name, "type") == 0) {
            if (strcmp(type, "string") != 0 || attribute_size != 13u ||
                memcmp(bytes + value_offset, "scanlineimage", 13u) != 0) return 0;
        }
    }
    if (position >= size || bytes[position - 1u] != 0u || !have_channels ||
        !have_data_window || (compression != 0u && compression != 3u) ||
        line_order != 0u || x_max < info->x_min || y_max < info->y_min) {
        return 0;
    }
    {
        uint64_t width = (uint64_t)((int64_t)x_max - info->x_min + 1);
        uint64_t height = (uint64_t)((int64_t)y_max - info->y_min + 1);
        if (!width || !height || width > EXR_MAX_PIXELS ||
            height > EXR_MAX_PIXELS || width > EXR_MAX_PIXELS / height) {
            return 0;
        }
        pixels = width * height;
        if (width > UINT32_MAX || height > UINT32_MAX ||
            pixels > SIZE_MAX / 3u) return 0;
        info->width = (uint32_t)width;
        info->height = (uint32_t)height;
        info->pixel_count = (size_t)pixels;
    }
    info->compression = (uint8_t)compression;
    info->header_end = position;
    info->lines_per_chunk = compression == 3u ? 16u : 1u;
    info->chunk_count = ((size_t)info->height + info->lines_per_chunk - 1u) /
                        info->lines_per_chunk;
    if (info->chunk_count > (SIZE_MAX - position) / 8u ||
        position + info->chunk_count * 8u > size) return 0;
    for (size_t chunk_index = 0u; chunk_index < info->chunk_count; ++chunk_index) {
        uint64_t offset64 = qw_read_u64_le(bytes + position + chunk_index * 8u);
        size_t offset;
        size_t first_row = chunk_index * info->lines_per_chunk;
        size_t line_count = info->height - first_row;
        size_t expected_size;
        size_t chunk_size;
        if (line_count > info->lines_per_chunk) line_count = info->lines_per_chunk;
        if (offset64 > SIZE_MAX) return 0;
        offset = (size_t)offset64;
        if (offset < position + info->chunk_count * 8u || offset > size ||
            size - offset < 8u) return 0;
        if ((int64_t)exr_lei32(bytes + offset) !=
            (int64_t)info->y_min + (int64_t)first_row) return 0;
        chunk_size = qw_read_u32_le(bytes + offset + 4u);
        if ((uint64_t)info->width * line_count * 3u * sizeof(float) > SIZE_MAX) {
            return 0;
        }
        expected_size = (size_t)info->width * line_count * 3u * sizeof(float);
        if (expected_size > EXR_MAX_BLOCK_BYTES || chunk_size != expected_size ||
            chunk_size > size - offset - 8u) return 0;
    }
    return 1;
}

int qw_exr_probe(const void *data, size_t size)
{
    QwExrInfo info;
    return exr_read_info(data, size, &info);
}

int qw_exr_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwExrInfo info;
    uint8_t *pixels;
    size_t output_size;
    size_t offset_table;
    if (!out_page || !exr_read_info(data, size, &info)) return QW_STATUS_ERR_FORMAT;
    memset(out_page, 0, sizeof(*out_page));
    output_size = info.pixel_count * 3u;
    pixels = (uint8_t *)malloc(output_size);
    if (!pixels) return QW_STATUS_ERR_OUT_OF_MEMORY;
    offset_table = info.header_end;
    for (size_t chunk_index = 0u; chunk_index < info.chunk_count; ++chunk_index) {
        size_t chunk_offset = (size_t)qw_read_u64_le(bytes + offset_table + chunk_index * 8u);
        size_t first_row = chunk_index * info.lines_per_chunk;
        size_t line_count = info.height - first_row;
        size_t source_offset = chunk_offset + 8u;
        if (line_count > info.lines_per_chunk) line_count = info.lines_per_chunk;
        for (size_t row = 0u; row < line_count; ++row) {
            size_t output_y = first_row + row;
            for (size_t channel = 0u; channel < 3u; ++channel) {
                size_t output_channel = 0u;
                for (; output_channel < 3u; ++output_channel) {
                    if (info.channel_output[output_channel] == channel) break;
                }
                if (output_channel == 3u) {
                    free(pixels);
                    return QW_STATUS_ERR_FORMAT;
                }
                for (size_t x = 0u; x < info.width; ++x) {
                    uint32_t float_bits = qw_read_u32_le(bytes + source_offset);
                    float sample;
                    uint8_t value;
                    size_t output = (output_y * info.width + x) * 3u +
                                    output_channel;
                    memcpy(&sample, &float_bits, sizeof(sample));
                    if (!isfinite(sample)) {
                        free(pixels);
                        return QW_STATUS_ERR_FORMAT;
                    }
                    if (sample <= 0.0f) value = 0u;
                    else if (sample >= 1.0f) value = 255u;
                    else value = (uint8_t)(sample * 255.0f + 0.5f);
                    pixels[output] = value;
                    source_offset += sizeof(float);
                }
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
