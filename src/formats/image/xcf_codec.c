#include "image_internal.h"

#include <quirewise/compression.h>

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define QW_XCF_MAX_VERSION 26u
#define QW_XCF_MAX_LAYERS 4096u
#define QW_XCF_MAX_OBJECTS 8192u
#define QW_XCF_TILE_EDGE 64u

typedef struct QwXcfReader {
    const uint8_t *data;
    size_t size;
    size_t offset;
    unsigned pointer_size;
} QwXcfReader;

typedef struct QwXcfImageInfo {
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t base_type;
    uint32_t precision;
    unsigned compression;
    uint8_t palette[256][3];
    size_t palette_count;
} QwXcfImageInfo;

typedef struct QwXcfLayer {
    uint32_t width;
    uint32_t height;
    uint32_t type;
    int32_t x_offset;
    int32_t y_offset;
    uint64_t hierarchy_offset;
    uint64_t mask_offset;
    double opacity;
    int visible;
    int group;
    int floating;
    int has_effects;
    int mode;
    int composite_mode;
    int has_composite_mode;
    int blend_space;
    int has_blend_space;
    int composite_space;
    int has_composite_space;
} QwXcfLayer;

static int xcf_add_size(size_t left, size_t right, size_t *out)
{
    if (!out || left > SIZE_MAX - right) return 0;
    *out = left + right;
    return 1;
}

static int xcf_read_u8(QwXcfReader *reader, uint8_t *out)
{
    if (!reader || !out || reader->offset >= reader->size) return 0;
    *out = reader->data[reader->offset++];
    return 1;
}

static int xcf_read_u32(QwXcfReader *reader, uint32_t *out)
{
    const uint8_t *p;
    if (!reader || !out || reader->size - reader->offset < 4u) return 0;
    p = reader->data + reader->offset;
    *out = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    reader->offset += 4u;
    return 1;
}

static int32_t xcf_signed_u32(uint32_t value)
{
    if (value <= INT32_MAX) return (int32_t)value;
    return (int32_t)((int64_t)value - INT64_C(4294967296));
}

static int xcf_read_i32(QwXcfReader *reader, int32_t *out)
{
    uint32_t value;
    if (!out || !xcf_read_u32(reader, &value)) return 0;
    *out = xcf_signed_u32(value);
    return 1;
}

static int xcf_read_pointer(QwXcfReader *reader, uint64_t *out)
{
    uint32_t high;
    uint32_t low;
    if (!reader || !out) return 0;
    if (reader->pointer_size == 4u) {
        if (!xcf_read_u32(reader, &low)) return 0;
        *out = low;
        return 1;
    }
    if (reader->pointer_size != 8u ||
        !xcf_read_u32(reader, &high) || !xcf_read_u32(reader, &low)) {
        return 0;
    }
    *out = ((uint64_t)high << 32) | low;
    return 1;
}

static int xcf_seek(QwXcfReader *reader, uint64_t offset)
{
    if (!reader || offset > (uint64_t)reader->size) return 0;
    reader->offset = (size_t)offset;
    return 1;
}

static int xcf_skip(QwXcfReader *reader, size_t count)
{
    size_t end;
    if (!reader || !xcf_add_size(reader->offset, count, &end) ||
        end > reader->size) return 0;
    reader->offset = end;
    return 1;
}

static int xcf_read_string(QwXcfReader *reader)
{
    uint32_t length;
    if (!xcf_read_u32(reader, &length)) return 0;
    if (length == 0u) return 1;
    if (length > reader->size - reader->offset ||
        reader->data[reader->offset + length - 1u] != 0u) return 0;
    return xcf_skip(reader, length);
}

static int xcf_parse_version(const uint8_t *data, size_t size,
                             uint32_t *out_version)
{
    uint32_t version;
    if (!data || !out_version || size < 14u ||
        memcmp(data, "gimp xcf ", 9u) != 0 || data[13] != 0u) return 0;
    if (memcmp(data + 9u, "file", 4u) == 0) {
        version = 0u;
    } else if (data[9] == 'v' && data[10] >= '0' && data[10] <= '9' &&
               data[11] >= '0' && data[11] <= '9' &&
               data[12] >= '0' && data[12] <= '9') {
        version = (uint32_t)(data[10] - '0') * 100u +
                  (uint32_t)(data[11] - '0') * 10u +
                  (uint32_t)(data[12] - '0');
    } else {
        return 0;
    }
    if (version > QW_XCF_MAX_VERSION || (version >= 4u && version <= 6u)) {
        return 0;
    }
    *out_version = version;
    return 1;
}

static int xcf_parse_header(QwXcfReader *reader, QwXcfImageInfo *info)
{
    if (!reader || !info || !xcf_parse_version(reader->data, reader->size,
                                               &info->version)) return 0;
    reader->offset = 14u;
    reader->pointer_size = info->version >= 11u ? 8u : 4u;
    if (!xcf_read_u32(reader, &info->width) ||
        !xcf_read_u32(reader, &info->height) ||
        !xcf_read_u32(reader, &info->base_type)) return 0;
    info->precision = 150u;
    if (info->version >= 4u && !xcf_read_u32(reader, &info->precision)) {
        return 0;
    }
    if (!qw_image_dimensions_within_limits(info->width, info->height) ||
        info->base_type > 2u ||
        (info->version >= 7u && info->precision != 150u) ||
        (info->base_type == 2u && info->version == 0u)) return 0;
    info->compression = 0u;
    info->palette_count = 0u;
    return 1;
}

static int xcf_parse_properties(QwXcfReader *reader, QwXcfImageInfo *image,
                                QwXcfLayer *layer, int image_properties)
{
    unsigned property_count = 0u;
    for (;;) {
        uint32_t type;
        uint32_t length;
        size_t payload_start;
        size_t payload_end;
        if (++property_count > 4096u || !xcf_read_u32(reader, &type) ||
            !xcf_read_u32(reader, &length)) return 0;
        if (type == 0u) return length == 0u;
        payload_start = reader->offset;
        if (!xcf_add_size(payload_start, (size_t)length, &payload_end) ||
            payload_end > reader->size) return 0;

        if (image_properties && type == 17u) {
            uint8_t compression;
            if (length != 1u || !xcf_read_u8(reader, &compression) ||
                compression > 2u) return 0;
            image->compression = compression;
        } else if (image_properties && type == 1u) {
            uint32_t colors;
            size_t palette_bytes;
            if (!xcf_read_u32(reader, &colors) || colors > 256u ||
                !qw_checked_size_mul((size_t)colors, 3u, &palette_bytes) ||
                palette_bytes > reader->size - reader->offset) return 0;
            for (uint32_t i = 0u; i < colors; ++i) {
                reader->offset += 3u;
                image->palette[i][0] = reader->data[reader->offset - 3u];
                image->palette[i][1] = reader->data[reader->offset - 2u];
                image->palette[i][2] = reader->data[reader->offset - 1u];
            }
            image->palette_count = (size_t)colors;
        } else if (!image_properties && layer &&
                   (type == 6u || type == 7u || type == 8u || type == 15u ||
                    type == 29u || type == 33u || type == 35u ||
                    type == 36u || type == 37u)) {
            uint32_t value;
            if (type == 29u) {
                if (length != 0u) return 0;
                layer->group = 1;
            } else if (type == 15u) {
                if (length != 8u ||
                    !xcf_read_i32(reader, &layer->x_offset) ||
                    !xcf_read_i32(reader, &layer->y_offset)) return 0;
            } else if (type == 33u) {
                float opacity;
                if (length != 4u || !xcf_read_u32(reader, &value)) return 0;
                memcpy(&opacity, &value, sizeof(opacity));
                if (!(opacity >= 0.0f && opacity <= 1.0f)) return 0;
                layer->opacity = (double)opacity;
            } else {
                if (length != 4u || !xcf_read_u32(reader, &value)) return 0;
                if (type == 6u) {
                    if (value > 255u) return 0;
                    layer->opacity = (double)value / 255.0;
                } else if (type == 7u) {
                    layer->mode = (int)value;
                } else if (type == 8u) {
                    layer->visible = value != 0u;
                } else if (type == 35u) {
                    layer->composite_mode = xcf_signed_u32(value);
                    layer->has_composite_mode = 1;
                } else if (type == 36u) {
                    layer->composite_space = xcf_signed_u32(value);
                    layer->has_composite_space = 1;
                } else if (type == 37u) {
                    layer->blend_space = xcf_signed_u32(value);
                    layer->has_blend_space = 1;
                }
            }
        }

        if (image_properties && type == 1u) {
            /* Historical GIMP versions wrote an incorrect colormap length. */
            continue;
        }
        if (reader->offset > payload_end) return 0;
        reader->offset = payload_end;
    }
}

static int xcf_read_layer_offsets(QwXcfReader *reader, uint64_t **out_offsets,
                                  size_t *out_count)
{
    uint64_t *offsets = NULL;
    size_t count = 0u;
    size_t capacity = 0u;
    for (;;) {
        uint64_t offset;
        if (!xcf_read_pointer(reader, &offset)) goto fail;
        if (offset == 0u) break;
        if (offset >= reader->size || count >= QW_XCF_MAX_LAYERS) goto fail;
        if (count == capacity) {
            size_t new_capacity = capacity ? capacity * 2u : 8u;
            uint64_t *next;
            if (new_capacity > QW_XCF_MAX_LAYERS) {
                new_capacity = QW_XCF_MAX_LAYERS;
            }
            if (new_capacity <= capacity ||
                new_capacity > SIZE_MAX / sizeof(*offsets)) goto fail;
            next = (uint64_t *)realloc(offsets,
                                       new_capacity * sizeof(*offsets));
            if (!next) goto fail;
            offsets = next;
            capacity = new_capacity;
        }
        offsets[count++] = offset;
    }
    *out_offsets = offsets;
    *out_count = count;
    return 1;

fail:
    free(offsets);
    return 0;
}

static int xcf_skip_pointer_list(QwXcfReader *reader)
{
    for (size_t i = 0u; i < QW_XCF_MAX_OBJECTS; ++i) {
        uint64_t offset;
        if (!xcf_read_pointer(reader, &offset)) return 0;
        if (offset == 0u) return 1;
        if (offset >= reader->size) return 0;
    }
    return 0;
}

static int xcf_parse_layer(QwXcfReader *reader, uint64_t offset,
                           QwXcfImageInfo *image, QwXcfLayer *layer)
{
    uint64_t effect_offset;
    uint32_t effect_count = 0u;
    memset(layer, 0, sizeof(*layer));
    layer->visible = 1;
    layer->opacity = 1.0;
    if (!xcf_seek(reader, offset) || !xcf_read_u32(reader, &layer->width) ||
        !xcf_read_u32(reader, &layer->height) ||
        !xcf_read_u32(reader, &layer->type) ||
        !qw_image_dimensions_within_limits(layer->width, layer->height) ||
        !xcf_read_string(reader) ||
        !xcf_parse_properties(reader, image, layer, 0) ||
        !xcf_read_pointer(reader, &layer->hierarchy_offset) ||
        !xcf_read_pointer(reader, &layer->mask_offset)) return 0;
    if (layer->hierarchy_offset == 0u) return 0;
    for (;;) {
        if (!xcf_read_pointer(reader, &effect_offset)) return 0;
        if (effect_offset == 0u) break;
        if (effect_offset >= reader->size ||
            ++effect_count > QW_XCF_MAX_OBJECTS) return 0;
        layer->has_effects = 1;
    }
    return 1;
}

static int xcf_layer_pixel_layout(const QwXcfImageInfo *image,
                                  const QwXcfLayer *layer,
                                  unsigned *out_channels,
                                  int *out_has_alpha)
{
    unsigned channels;
    int has_alpha;
    if (image->base_type == 0u) {
        if (layer->type == 0u) {
            channels = 3u;
            has_alpha = 0;
        } else if (layer->type == 1u) {
            channels = 4u;
            has_alpha = 1;
        } else return 0;
    } else if (image->base_type == 1u) {
        if (layer->type == 2u) {
            channels = 1u;
            has_alpha = 0;
        } else if (layer->type == 3u) {
            channels = 2u;
            has_alpha = 1;
        } else return 0;
    } else {
        if (layer->type == 4u) {
            channels = 1u;
            has_alpha = 0;
        } else if (layer->type == 5u) {
            channels = 2u;
            has_alpha = 1;
        } else return 0;
    }
    *out_channels = channels;
    *out_has_alpha = has_alpha;
    return 1;
}

static int xcf_layer_mode_supported(const QwXcfImageInfo *image,
                                   const QwXcfLayer *layer)
{
    int expected_space = image->version >= 23u ? 4 : 2;
    if (layer->mode != 0 && layer->mode != 28) return 0;
    if (layer->mode == 28 && image->version < 10u) return 0;
    if (layer->has_composite_mode && layer->composite_mode != 1 &&
        layer->composite_mode != -1) return 0;
    if (layer->has_blend_space &&
        (layer->blend_space != expected_space &&
         layer->blend_space != -expected_space)) return 0;
    if (layer->has_composite_space &&
        (layer->composite_space != expected_space &&
         layer->composite_space != -expected_space)) return 0;
    return 1;
}

static int xcf_decode_rle_tile(const uint8_t *input, size_t input_size,
                               uint8_t *output, size_t pixels,
                               unsigned channels)
{
    size_t input_offset = 0u;
    for (unsigned channel = 0u; channel < channels; ++channel) {
        size_t produced = 0u;
        while (produced < pixels) {
            uint8_t opcode;
            size_t run;
            int repeated;
            if (input_offset >= input_size) return 0;
            opcode = input[input_offset++];
            if (opcode <= 126u) {
                run = (size_t)opcode + 1u;
                repeated = 1;
            } else if (opcode == 127u || opcode == 128u) {
                uint8_t high;
                uint8_t low;
                if (input_size - input_offset < 2u) return 0;
                high = input[input_offset++];
                low = input[input_offset++];
                run = ((size_t)high << 8) | (size_t)low;
                repeated = opcode == 127u;
                if (run == 0u) return 0;
            } else {
                run = 256u - (size_t)opcode;
                repeated = 0;
            }
            if (run > pixels - produced) return 0;
            if (repeated) {
                uint8_t value;
                if (input_offset >= input_size) return 0;
                value = input[input_offset++];
                for (size_t i = 0u; i < run; ++i) {
                    output[(produced + i) * channels + channel] = value;
                }
            } else {
                if (run > input_size - input_offset) return 0;
                for (size_t i = 0u; i < run; ++i) {
                    output[(produced + i) * channels + channel] =
                        input[input_offset + i];
                }
                input_offset += run;
            }
            produced += run;
        }
    }
    return 1;
}

static int xcf_decode_tile(const QwXcfReader *reader, uint64_t tile_offset,
                           uint64_t end_offset, unsigned compression,
                           uint8_t *tile, size_t tile_size, size_t pixels,
                           unsigned channels)
{
    size_t input_size;
    const uint8_t *input;
    if (!reader || tile_offset >= end_offset ||
        end_offset > (uint64_t)reader->size ||
        tile_offset > (uint64_t)SIZE_MAX || end_offset > (uint64_t)SIZE_MAX) {
        return QW_STATUS_ERR_FORMAT;
    }
    input_size = (size_t)(end_offset - tile_offset);
    input = reader->data + (size_t)tile_offset;
    if (compression == 0u) {
        if (tile_size > input_size) return QW_STATUS_ERR_FORMAT;
        memcpy(tile, input, tile_size);
        return QW_STATUS_OK;
    }
    if (compression == 1u) {
        return xcf_decode_rle_tile(input, input_size, tile, pixels, channels)
            ? QW_STATUS_OK : QW_STATUS_ERR_FORMAT;
    }
    if (compression == 2u) {
        return qw_compression_inflate_zlib(input, input_size, tile, tile_size)
            ? QW_STATUS_OK : QW_STATUS_ERR_FORMAT;
    }
    return QW_STATUS_ERR_UNSUPPORTED;
}

static int xcf_read_tile_pointers(QwXcfReader *reader, uint64_t hierarchy,
                                  const QwXcfLayer *layer, unsigned channels,
                                  uint64_t **out_tiles, size_t *out_count)
{
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_pixel;
    uint64_t level;
    uint64_t next_level;
    uint64_t *tiles = NULL;
    size_t tile_columns;
    size_t tile_rows;
    size_t tile_count;
    size_t index;
    size_t tile_data_start;
    if (!xcf_seek(reader, hierarchy) ||
        !xcf_read_u32(reader, &width) || !xcf_read_u32(reader, &height) ||
        !xcf_read_u32(reader, &bytes_per_pixel) ||
        width != layer->width || height != layer->height ||
        bytes_per_pixel != channels ||
        !xcf_read_pointer(reader, &level) || level == 0u) return 0;
    for (unsigned i = 0u; i < 64u; ++i) {
        if (!xcf_read_pointer(reader, &next_level)) return 0;
        if (next_level == 0u) break;
        if (i == 63u) return 0;
    }
    if (!xcf_seek(reader, level) || !xcf_read_u32(reader, &width) ||
        !xcf_read_u32(reader, &height) ||
        width != layer->width || height != layer->height) return 0;
    tile_columns = ((size_t)width + QW_XCF_TILE_EDGE - 1u) /
                   QW_XCF_TILE_EDGE;
    tile_rows = ((size_t)height + QW_XCF_TILE_EDGE - 1u) /
                QW_XCF_TILE_EDGE;
    if (!qw_checked_size_mul(tile_columns, tile_rows, &tile_count) ||
        tile_count == 0u || tile_count > reader->size / reader->pointer_size) {
        return 0;
    }
    if (tile_count > SIZE_MAX / sizeof(*tiles)) return 0;
    tiles = (uint64_t *)malloc(tile_count * sizeof(*tiles));
    if (!tiles) return -1;
    for (index = 0u; index < tile_count; ++index) {
        if (!xcf_read_pointer(reader, &tiles[index]) || tiles[index] == 0u ||
            tiles[index] >= reader->size ||
            (index != 0u && tiles[index] <= tiles[index - 1u])) {
            free(tiles);
            return 0;
        }
    }
    if (!xcf_read_pointer(reader, &next_level) || next_level != 0u) {
        free(tiles);
        return 0;
    }
    tile_data_start = reader->offset;
    if (tiles[0] < (uint64_t)tile_data_start) {
        free(tiles);
        return 0;
    }
    *out_tiles = tiles;
    *out_count = tile_count;
    return 1;
}

static int xcf_composite_layer(QwXcfReader *reader, QwXcfImageInfo *image,
                               const QwXcfLayer *layer, uint8_t *canvas,
                               unsigned channels, int has_alpha)
{
    uint64_t *tiles = NULL;
    size_t tile_count = 0u;
    size_t layer_pixels;
    uint32_t columns;
    int pointer_result;
    int rc = QW_STATUS_ERR_FORMAT;
    if (!xcf_layer_mode_supported(image, layer) || layer->group ||
        layer->floating || layer->mask_offset != 0u || layer->has_effects) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (image->base_type == 2u && image->palette_count == 0u) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (!qw_checked_size_mul((size_t)layer->width, (size_t)layer->height,
                              &layer_pixels)) return QW_STATUS_ERR_FORMAT;
    pointer_result = xcf_read_tile_pointers(reader, layer->hierarchy_offset,
                                            layer, channels, &tiles,
                                            &tile_count);
    if (pointer_result < 0) return QW_STATUS_ERR_OUT_OF_MEMORY;
    if (pointer_result == 0) return QW_STATUS_ERR_FORMAT;
    columns = (layer->width + QW_XCF_TILE_EDGE - 1u) / QW_XCF_TILE_EDGE;
    for (size_t tile_index = 0u; tile_index < tile_count; ++tile_index) {
        size_t tile_x = tile_index % columns;
        size_t tile_y = tile_index / columns;
        size_t x0 = tile_x * QW_XCF_TILE_EDGE;
        size_t y0 = tile_y * QW_XCF_TILE_EDGE;
        size_t tile_width = layer->width - x0;
        size_t tile_height = layer->height - y0;
        size_t tile_pixels;
        size_t tile_size;
        uint64_t tile_end = tile_index + 1u < tile_count
            ? tiles[tile_index + 1u] : (uint64_t)reader->size;
        uint8_t *tile;
        if (tile_width > QW_XCF_TILE_EDGE) tile_width = QW_XCF_TILE_EDGE;
        if (tile_height > QW_XCF_TILE_EDGE) tile_height = QW_XCF_TILE_EDGE;
        if (!qw_checked_size_mul(tile_width, tile_height, &tile_pixels) ||
            !qw_checked_size_mul(tile_pixels, channels, &tile_size)) {
            rc = QW_STATUS_ERR_FORMAT;
            goto cleanup;
        }
        tile = (uint8_t *)malloc(tile_size ? tile_size : 1u);
        if (!tile) {
            rc = QW_STATUS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        rc = xcf_decode_tile(reader, tiles[tile_index], tile_end,
                             image->compression, tile, tile_size,
                             tile_pixels, channels);
        if (rc != QW_STATUS_OK) {
            free(tile);
            goto cleanup;
        }
        for (size_t ty = 0u; ty < tile_height; ++ty) {
            int64_t canvas_y = (int64_t)layer->y_offset + (int64_t)(y0 + ty);
            if (canvas_y < 0 || (uint64_t)canvas_y >= image->height) continue;
            for (size_t tx = 0u; tx < tile_width; ++tx) {
                int64_t canvas_x = (int64_t)layer->x_offset + (int64_t)(x0 + tx);
                size_t source_index;
                size_t destination_index;
                uint8_t source_rgb[3];
                uint32_t source_alpha;
                uint32_t destination_alpha;
                uint32_t destination_contribution;
                uint32_t denominator;
                uint8_t *destination;
                if (canvas_x < 0 || (uint64_t)canvas_x >= image->width) continue;
                source_index = (ty * tile_width + tx) * channels;
                destination_index =
                    ((size_t)canvas_y * image->width + (size_t)canvas_x) * 4u;
                destination = canvas + destination_index;
                if (image->base_type == 0u) {
                    source_rgb[0] = tile[source_index];
                    source_rgb[1] = tile[source_index + 1u];
                    source_rgb[2] = tile[source_index + 2u];
                } else if (image->base_type == 1u) {
                    source_rgb[0] = source_rgb[1] = source_rgb[2] = tile[source_index];
                } else {
                    uint8_t color_index = tile[source_index];
                    if ((size_t)color_index >= image->palette_count) {
                        free(tile);
                        rc = QW_STATUS_ERR_FORMAT;
                        goto cleanup;
                    }
                    source_rgb[0] = image->palette[color_index][0];
                    source_rgb[1] = image->palette[color_index][1];
                    source_rgb[2] = image->palette[color_index][2];
                }
                source_alpha = has_alpha ? tile[source_index + channels - 1u] : 255u;
                source_alpha = (source_alpha *
                    (uint32_t)(layer->opacity * 255.0 + 0.5) + 127u) / 255u;
                if (source_alpha == 0u) continue;
                destination_alpha = destination[3];
                destination_contribution = destination_alpha * (255u - source_alpha);
                denominator = source_alpha * 255u + destination_contribution;
                if (denominator == 0u) continue;
                for (unsigned c = 0u; c < 3u; ++c) {
                    uint32_t numerator =
                        (uint32_t)source_rgb[c] * source_alpha * 255u +
                        (uint32_t)destination[c] * destination_contribution;
                    destination[c] = (uint8_t)((numerator + denominator / 2u) /
                                               denominator);
                }
                destination[3] = (uint8_t)((denominator + 127u) / 255u);
            }
        }
        free(tile);
    }
    rc = QW_STATUS_OK;

cleanup:
    free(tiles);
    (void)layer_pixels;
    return rc;
}

int qw_xcf_probe(const void *data, size_t size)
{
    QwXcfReader reader;
    QwXcfImageInfo image;
    memset(&reader, 0, sizeof(reader));
    memset(&image, 0, sizeof(image));
    reader.data = (const uint8_t *)data;
    reader.size = size;
    return data && xcf_parse_header(&reader, &image);
}

int qw_xcf_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    QwXcfReader reader;
    QwXcfImageInfo image;
    uint64_t *layer_offsets = NULL;
    QwXcfLayer *layers = NULL;
    size_t layer_count = 0u;
    size_t pixel_count;
    size_t canvas_size;
    size_t output_size;
    uint8_t *canvas = NULL;
    uint8_t *output = NULL;
    int rc = QW_STATUS_ERR_FORMAT;
    if (!data || !size || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    memset(&reader, 0, sizeof(reader));
    memset(&image, 0, sizeof(image));
    reader.data = (const uint8_t *)data;
    reader.size = size;
    if (!xcf_parse_header(&reader, &image) ||
        !xcf_parse_properties(&reader, &image, NULL, 1) ||
        (image.base_type == 2u && image.palette_count == 0u) ||
        !xcf_read_layer_offsets(&reader, &layer_offsets, &layer_count) ||
        !xcf_skip_pointer_list(&reader) ||
        (image.version >= 2u && !xcf_skip_pointer_list(&reader))) {
        goto cleanup;
    }
    if (layer_count > SIZE_MAX / sizeof(*layers) ||
        !qw_checked_size_mul((size_t)image.width, (size_t)image.height,
                             &pixel_count) ||
        !qw_checked_size_mul(pixel_count, 4u, &canvas_size) ||
        !qw_checked_size_mul(pixel_count, 3u, &output_size)) {
        rc = QW_STATUS_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    layers = (QwXcfLayer *)calloc(layer_count ? layer_count : 1u,
                                  sizeof(*layers));
    canvas = (uint8_t *)calloc(canvas_size ? canvas_size : 1u, 1u);
    output = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!layers || !canvas || !output) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (size_t i = 0u; i < layer_count; ++i) {
        if (!xcf_parse_layer(&reader, layer_offsets[i], &image, &layers[i])) {
            rc = QW_STATUS_ERR_FORMAT;
            goto cleanup;
        }
    }
    for (size_t i = layer_count; i > 0u; --i) {
        QwXcfLayer *layer = &layers[i - 1u];
        unsigned channels;
        int has_alpha;
        if (!layer->visible || layer->opacity <= 0.0) continue;
        if (!xcf_layer_pixel_layout(&image, layer, &channels, &has_alpha)) {
            rc = QW_STATUS_ERR_UNSUPPORTED;
            goto cleanup;
        }
        rc = xcf_composite_layer(&reader, &image, layer, canvas,
                                 channels, has_alpha);
        if (rc != QW_STATUS_OK) goto cleanup;
    }
    for (size_t i = 0u; i < pixel_count; ++i) {
        uint32_t alpha = canvas[i * 4u + 3u];
        for (size_t c = 0u; c < 3u; ++c) {
            uint32_t color = canvas[i * 4u + c];
            output[i * 3u + c] = (uint8_t)((color * alpha + 255u *
                (255u - alpha) + 127u) / 255u);
        }
    }
    out_page->data = output;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = image.width;
    out_page->height = image.height;
    output = NULL;
    rc = QW_STATUS_OK;

cleanup:
    free(output);
    free(canvas);
    free(layers);
    free(layer_offsets);
    return rc;
}
