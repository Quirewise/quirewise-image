#include "image_internal.h"
#include "webp_vp8_probs.h"

#include <stdlib.h>
#include <string.h>

typedef struct QwWebPVP8BoolReader {
    const uint8_t *data;
    size_t size;
    size_t position;
    uint32_t range;
    uint32_t value;
    uint8_t bit_count;
    uint8_t failed;
} QwWebPVP8BoolReader;

static int webp_vp8_bool_reader_init(QwWebPVP8BoolReader *reader,
                                     const uint8_t *data,
                                     size_t size)
{
    if (!reader || !data || size < 2u) return 0;
    reader->data = data;
    reader->size = size;
    reader->position = 2u;
    reader->range = 255u;
    reader->value = ((uint32_t)data[0] << 8) | data[1];
    reader->bit_count = 0u;
    reader->failed = 0u;
    return 1;
}

static int webp_vp8_read_bool(QwWebPVP8BoolReader *reader,
                              uint8_t probability,
                              uint32_t *out_value)
{
    uint32_t split;
    uint32_t result;

    if (!reader || !out_value || reader->failed) return 0;
    split = 1u + (((reader->range - 1u) * probability) >> 8);
    if (reader->value >= (split << 8)) {
        result = 1u;
        reader->range -= split;
        reader->value -= split << 8;
    } else {
        result = 0u;
        reader->range = split;
    }
    while (reader->range < 128u) {
        reader->value <<= 1;
        reader->range <<= 1;
        if (++reader->bit_count == 8u) {
            reader->bit_count = 0u;
            if (reader->position >= reader->size) {
                reader->failed = 1u;
                return 0;
            }
            reader->value |= reader->data[reader->position++];
        }
    }
    *out_value = result;
    return 1;
}

static int webp_vp8_read_literal(QwWebPVP8BoolReader *reader,
                                 uint32_t bit_count,
                                 uint32_t *out_value)
{
    uint32_t value = 0u;
    if (!out_value || bit_count > 24u) return 0;
    for (uint32_t i = 0u; i < bit_count; ++i) {
        uint32_t bit;
        if (!webp_vp8_read_bool(reader, 128u, &bit)) return 0;
        value = (value << 1) | bit;
    }
    *out_value = value;
    return 1;
}

static int webp_vp8_read_maybe_signed(QwWebPVP8BoolReader *reader,
                                      uint32_t magnitude_bits,
                                      int8_t *out_value)
{
    uint32_t present;
    uint32_t magnitude;
    uint32_t negative;
    if (!out_value || magnitude_bits > 7u ||
        !webp_vp8_read_bool(reader, 128u, &present)) {
        return 0;
    }
    if (!present) {
        *out_value = 0;
        return 1;
    }
    if (!webp_vp8_read_literal(reader, magnitude_bits, &magnitude) ||
        !webp_vp8_read_bool(reader, 128u, &negative)) {
        return 0;
    }
    *out_value = (int8_t)(negative ? -(int32_t)magnitude : (int32_t)magnitude);
    return 1;
}

int qw_webp_vp8_parse_keyframe_header(const uint8_t *data,
                                      size_t size,
                                      uint32_t expected_width,
                                      uint32_t expected_height,
                                      QwWebPVP8Header *out_header)
{
    uint32_t frame_tag;
    uint32_t partition_size;
    uint32_t width_field;
    uint32_t height_field;
    uint32_t width;
    uint32_t height;
    QwWebPVP8BoolReader bool_reader;
    uint32_t color_space;
    uint32_t clamp_type;
    uint32_t bit_value;
    uint32_t literal;
    uint32_t partition_bits;
    QwWebPVP8Header parsed = {0};

    if (!out_header) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_header, 0, sizeof(*out_header));
    if (!data || !expected_width || !expected_height)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (size < 10u)
        return QW_STATUS_ERR_FORMAT;

    frame_tag = (uint32_t)data[0] |
                ((uint32_t)data[1] << 8) |
                ((uint32_t)data[2] << 16);
    if ((frame_tag & 1u) != 0u) return QW_STATUS_ERR_FORMAT;
    parsed.version = (uint8_t)((frame_tag >> 1) & 7u);
    if (parsed.version > 3u) return QW_STATUS_ERR_FORMAT;
    parsed.show_frame = (uint8_t)((frame_tag >> 4) & 1u);
    partition_size = frame_tag >> 5;

    if (data[3] != 0x9du || data[4] != 0x01u || data[5] != 0x2au)
        return QW_STATUS_ERR_FORMAT;

    width_field = (uint32_t)data[6] | ((uint32_t)data[7] << 8);
    height_field = (uint32_t)data[8] | ((uint32_t)data[9] << 8);
    width = width_field & UINT32_C(0x3fff);
    height = height_field & UINT32_C(0x3fff);
    if (!width || !height || width != expected_width ||
        height != expected_height) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (!qw_image_dimensions_within_limits(width, height))
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    if (partition_size < 2u || partition_size > size - 10u)
        return QW_STATUS_ERR_FORMAT;
    if (!webp_vp8_bool_reader_init(&bool_reader, data + 10u,
                                  (size_t)partition_size) ||
        !webp_vp8_read_bool(&bool_reader, 128u, &color_space) ||
        !webp_vp8_read_bool(&bool_reader, 128u, &clamp_type)) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (color_space != 0u) return QW_STATUS_ERR_FORMAT;

    if (!webp_vp8_read_bool(&bool_reader, 128u, &bit_value))
        return QW_STATUS_ERR_FORMAT;
    parsed.segmentation_enabled = (uint8_t)bit_value;
    if (parsed.segmentation_enabled) {
        if (!webp_vp8_read_bool(&bool_reader, 128u, &bit_value))
            return QW_STATUS_ERR_FORMAT;
        parsed.segmentation_map_updated = (uint8_t)bit_value;
        if (!webp_vp8_read_bool(&bool_reader, 128u, &bit_value))
            return QW_STATUS_ERR_FORMAT;
        parsed.segmentation_data_updated = (uint8_t)bit_value;
        if (parsed.segmentation_data_updated) {
            if (!webp_vp8_read_bool(&bool_reader, 128u, &bit_value))
                return QW_STATUS_ERR_FORMAT;
            parsed.segmentation_feature_mode = (uint8_t)bit_value;
            for (size_t i = 0u; i < 4u; ++i) {
                if (!webp_vp8_read_maybe_signed(
                        &bool_reader, 7u, &parsed.segment_quantizer_delta[i])) {
                    return QW_STATUS_ERR_FORMAT;
                }
            }
            for (size_t i = 0u; i < 4u; ++i) {
                if (!webp_vp8_read_maybe_signed(
                        &bool_reader, 6u, &parsed.segment_filter_delta[i])) {
                    return QW_STATUS_ERR_FORMAT;
                }
            }
        }
        if (parsed.segmentation_map_updated) {
            for (size_t i = 0u; i < 3u; ++i) {
                if (!webp_vp8_read_bool(&bool_reader, 128u, &bit_value))
                    return QW_STATUS_ERR_FORMAT;
                if (bit_value) {
                    if (!webp_vp8_read_literal(&bool_reader, 8u, &literal))
                        return QW_STATUS_ERR_FORMAT;
                    parsed.segment_map_probability[i] = (uint8_t)literal;
                } else {
                    parsed.segment_map_probability[i] = 255u;
                }
            }
        }
    }

    if (!webp_vp8_read_bool(&bool_reader, 128u, &bit_value))
        return QW_STATUS_ERR_FORMAT;
    parsed.loop_filter_type = (uint8_t)bit_value;
    if (!webp_vp8_read_literal(&bool_reader, 6u, &literal))
        return QW_STATUS_ERR_FORMAT;
    parsed.loop_filter_level = (uint8_t)literal;
    if (!webp_vp8_read_literal(&bool_reader, 3u, &literal))
        return QW_STATUS_ERR_FORMAT;
    parsed.loop_filter_sharpness = (uint8_t)literal;
    if (!webp_vp8_read_bool(&bool_reader, 128u, &bit_value))
        return QW_STATUS_ERR_FORMAT;
    parsed.loop_filter_delta_enabled = (uint8_t)bit_value;
    if (parsed.loop_filter_delta_enabled) {
        if (!webp_vp8_read_bool(&bool_reader, 128u, &bit_value))
            return QW_STATUS_ERR_FORMAT;
        if (bit_value) {
            for (size_t i = 0u; i < 4u; ++i) {
                if (!webp_vp8_read_maybe_signed(
                        &bool_reader, 6u, &parsed.reference_filter_delta[i])) {
                    return QW_STATUS_ERR_FORMAT;
                }
            }
            for (size_t i = 0u; i < 4u; ++i) {
                if (!webp_vp8_read_maybe_signed(
                        &bool_reader, 6u, &parsed.mode_filter_delta[i])) {
                    return QW_STATUS_ERR_FORMAT;
                }
            }
        }
    }

    if (!webp_vp8_read_literal(&bool_reader, 2u, &partition_bits))
        return QW_STATUS_ERR_FORMAT;
    parsed.token_partition_count = (uint8_t)(1u << partition_bits);
    if (!webp_vp8_read_literal(&bool_reader, 7u, &literal))
        return QW_STATUS_ERR_FORMAT;
    parsed.base_quantizer = (uint8_t)literal;
    for (size_t i = 0u; i < 5u; ++i) {
        if (!webp_vp8_read_maybe_signed(
                &bool_reader, 4u, &parsed.quantizer_delta[i])) {
            return QW_STATUS_ERR_FORMAT;
        }
    }

    /* The VP8 keyframe header carries update_proba before coefficient updates. */
    if (!webp_vp8_read_bool(&bool_reader, 128u, &bit_value))
        return QW_STATUS_ERR_FORMAT;

    memcpy(parsed.coefficient_probabilities,
           qw_webp_vp8_coeff_default_probability,
           sizeof(parsed.coefficient_probabilities));
    for (size_t type = 0u; type < 4u; ++type) {
        for (size_t band = 0u; band < 8u; ++band) {
            for (size_t context = 0u; context < 3u; ++context) {
                for (size_t token = 0u; token < 11u; ++token) {
                    if (!webp_vp8_read_bool(
                            &bool_reader,
                            qw_webp_vp8_coeff_update_probability
                                [type][band][context][token],
                            &bit_value)) {
                        return QW_STATUS_ERR_FORMAT;
                    }
                    if (bit_value) {
                        if (!webp_vp8_read_literal(
                                &bool_reader, 8u, &literal)) {
                            return QW_STATUS_ERR_FORMAT;
                        }
                        parsed.coefficient_probabilities
                            [type][band][context][token] = (uint8_t)literal;
                    }
                }
            }
        }
    }
    if (!webp_vp8_read_bool(&bool_reader, 128u, &bit_value))
        return QW_STATUS_ERR_FORMAT;
    parsed.skip_coeff_enabled = (uint8_t)bit_value;
    if (parsed.skip_coeff_enabled) {
        if (!webp_vp8_read_literal(&bool_reader, 8u, &literal))
            return QW_STATUS_ERR_FORMAT;
        parsed.skip_coeff_probability = (uint8_t)literal;
    }

    parsed.width = width;
    parsed.height = height;
    parsed.first_partition_size = partition_size;
    parsed.first_partition_offset = 10u + (size_t)partition_size;
    parsed.horizontal_scale = (uint8_t)(width_field >> 14);
    parsed.vertical_scale = (uint8_t)(height_field >> 14);
    parsed.color_space = (uint8_t)color_space;
    parsed.clamp_type = (uint8_t)clamp_type;
    parsed.first_partition_position = bool_reader.position;
    parsed.first_partition_range = bool_reader.range;
    parsed.first_partition_value = bool_reader.value;
    parsed.first_partition_bit_count = bool_reader.bit_count;
    *out_header = parsed;
    return QW_STATUS_OK;
}

static int webp_vp8_read_tree(QwWebPVP8BoolReader *reader,
                              const int8_t *tree,
                              size_t tree_size,
                              const uint8_t *probabilities,
                              size_t start_node,
                              uint32_t *out_value)
{
    size_t node = start_node;
    if (!reader || !tree || !probabilities || !out_value ||
        tree_size < 2u || (tree_size & 1u) != 0u ||
        node + 1u >= tree_size || (node & 1u) != 0u) {
        return 0;
    }
    for (size_t depth = 0u; depth < tree_size; ++depth) {
        uint32_t bit;
        int32_t child;
        if (node + 1u >= tree_size ||
            !webp_vp8_read_bool(reader, probabilities[node >> 1], &bit)) {
            return 0;
        }
        child = tree[node + bit];
        if (child <= 0) {
            *out_value = (uint32_t)-child;
            return 1;
        }
        if ((child & 1) != 0 || (size_t)child + 1u >= tree_size)
            return 0;
        node = (size_t)child;
    }
    return 0;
}

int qw_webp_vp8_decode_macroblock_modes(
    const uint8_t *data,
    size_t size,
    const QwWebPVP8Header *header,
    QwWebPVP8Macroblock **out_macroblocks,
    size_t *out_macroblock_count)
{
    static const int8_t segment_tree[6] = {2, 4, 0, -1, -2, -3};
    static const int8_t keyframe_y_tree[8] = {-4, 2, 4, 6, 0, -1, -2, -3};
    static const int8_t uv_mode_tree[6] = {0, 2, -1, 4, -2, -3};
    static const int8_t b_mode_tree[18] = {
        0, 2, -1, 4, -2, 6, 8, 12, -3, 10, -5, -6, -4, 14, -7, 16, -8, -9
    };
    static const uint8_t keyframe_y_probability[4] = {145u, 156u, 163u, 128u};
    static const uint8_t uv_mode_probability[3] = {142u, 114u, 183u};
    static const uint8_t coarse_mode_to_b_mode[4] = {0u, 2u, 3u, 1u};
    QwWebPVP8BoolReader reader;
    QwWebPVP8Macroblock *macroblocks = NULL;
    uint8_t *above_modes = NULL;
    uint8_t left_modes[4] = {0u};
    size_t macroblock_columns;
    size_t macroblock_rows;
    size_t macroblock_count;
    size_t above_mode_count;

    if (!out_macroblocks || !out_macroblock_count)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_macroblocks = NULL;
    *out_macroblock_count = 0u;
    if (!data || !header || size < 10u ||
        header->first_partition_size < 2u ||
        header->first_partition_size > size - 10u ||
        header->first_partition_position > header->first_partition_size ||
        header->first_partition_range < 128u ||
        header->first_partition_range > 255u ||
        header->first_partition_bit_count > 7u ||
        !header->width || !header->height) {
        return QW_STATUS_ERR_FORMAT;
    }
    macroblock_columns = ((size_t)header->width + 15u) / 16u;
    macroblock_rows = ((size_t)header->height + 15u) / 16u;
    if (!qw_checked_size_mul(macroblock_columns, macroblock_rows,
                             &macroblock_count) ||
        !qw_checked_size_mul(macroblock_columns, 4u, &above_mode_count) ||
        macroblock_count > SIZE_MAX / sizeof(*macroblocks)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    macroblocks = (QwWebPVP8Macroblock *)calloc(
        macroblock_count, sizeof(*macroblocks));
    above_modes = (uint8_t *)calloc(above_mode_count, sizeof(*above_modes));
    if (!macroblocks || !above_modes) {
        free(macroblocks);
        free(above_modes);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    reader.data = data + 10u;
    reader.size = header->first_partition_size;
    reader.position = header->first_partition_position;
    reader.range = header->first_partition_range;
    reader.value = header->first_partition_value;
    reader.bit_count = header->first_partition_bit_count;
    reader.failed = 0u;

    for (size_t mb_y = 0u; mb_y < macroblock_rows; ++mb_y) {
        memset(left_modes, 0, sizeof(left_modes));
        for (size_t mb_x = 0u; mb_x < macroblock_columns; ++mb_x) {
            size_t mb_index = mb_y * macroblock_columns + mb_x;
            QwWebPVP8Macroblock *macroblock = &macroblocks[mb_index];
            uint32_t value;

            if (header->segmentation_enabled &&
                header->segmentation_map_updated) {
                if (!webp_vp8_read_tree(
                        &reader, segment_tree, sizeof(segment_tree),
                        header->segment_map_probability, 0u, &value)) {
                    goto format_error;
                }
                macroblock->segment_id = (uint8_t)value;
            }
            if (header->skip_coeff_enabled) {
                if (!webp_vp8_read_bool(&reader,
                        header->skip_coeff_probability, &value)) {
                    goto format_error;
                }
                macroblock->skip_coeff = (uint8_t)value;
            }
            if (!webp_vp8_read_tree(&reader, keyframe_y_tree,
                                    sizeof(keyframe_y_tree),
                                    keyframe_y_probability, 0u, &value)) {
                goto format_error;
            }
            macroblock->y_mode = (uint8_t)value;
            if (macroblock->y_mode == 4u) {
                for (size_t block_y = 0u; block_y < 4u; ++block_y) {
                    for (size_t block_x = 0u; block_x < 4u; ++block_x) {
                        size_t block = block_y * 4u + block_x;
                        uint8_t above_mode = block_y
                            ? macroblock->b_mode[block - 4u]
                            : above_modes[mb_x * 4u + block_x];
                        uint8_t left_mode = block_x
                            ? macroblock->b_mode[block - 1u]
                            : left_modes[block_y];
                        if (above_mode >= 10u || left_mode >= 10u ||
                            !webp_vp8_read_tree(
                                &reader, b_mode_tree, sizeof(b_mode_tree),
                                qw_webp_vp8_keyframe_bmode_probability
                                    [above_mode][left_mode], 0u, &value)) {
                            goto format_error;
                        }
                        macroblock->b_mode[block] = (uint8_t)value;
                    }
                }
                for (size_t block_x = 0u; block_x < 4u; ++block_x)
                    above_modes[mb_x * 4u + block_x] =
                        macroblock->b_mode[12u + block_x];
                for (size_t block_y = 0u; block_y < 4u; ++block_y)
                    left_modes[block_y] = macroblock->b_mode[block_y * 4u + 3u];
            } else {
                uint8_t block_mode = coarse_mode_to_b_mode[macroblock->y_mode];
                for (size_t block_x = 0u; block_x < 4u; ++block_x)
                    above_modes[mb_x * 4u + block_x] = block_mode;
                for (size_t block_y = 0u; block_y < 4u; ++block_y)
                    left_modes[block_y] = block_mode;
                memset(macroblock->b_mode, block_mode,
                       sizeof(macroblock->b_mode));
            }
            if (!webp_vp8_read_tree(&reader, uv_mode_tree,
                                    sizeof(uv_mode_tree),
                                    uv_mode_probability, 0u, &value)) {
                goto format_error;
            }
            macroblock->uv_mode = (uint8_t)value;
        }
    }

    free(above_modes);
    *out_macroblocks = macroblocks;
    *out_macroblock_count = macroblock_count;
    return QW_STATUS_OK;

format_error:
    free(above_modes);
    free(macroblocks);
    return QW_STATUS_ERR_FORMAT;
}

static int webp_vp8_decode_mb_coefficients(
    QwWebPVP8BoolReader *reader,
    const uint8_t probabilities[4][8][3][11],
    uint8_t y_mode,
    uint8_t left_context[9],
    uint8_t above_context[9],
    int16_t coefficients[25][16])
{
    static const int8_t coefficient_tree[22] = {
        -11, 2, 0, 4, -1, 6, 8, 12, -2, 10, -3, -4,
        14, 16, -5, -6, 18, 20, -7, -8, -9, -10
    };
    static const uint8_t coefficient_band[16] = {
        0u, 1u, 2u, 3u, 6u, 4u, 5u, 6u,
        6u, 6u, 6u, 6u, 6u, 6u, 6u, 7u
    };
    static const uint8_t zigzag[16] = {
        0u, 1u, 4u, 8u, 5u, 2u, 3u, 6u,
        9u, 12u, 13u, 10u, 7u, 11u, 14u, 15u
    };
    static const uint8_t left_context_index[25] = {
        0u, 0u, 0u, 0u, 1u, 1u, 1u, 1u,
        2u, 2u, 2u, 2u, 3u, 3u, 3u, 3u,
        4u, 4u, 5u, 5u, 6u, 6u, 7u, 7u, 8u
    };
    static const uint8_t above_context_index[25] = {
        0u, 1u, 2u, 3u, 0u, 1u, 2u, 3u,
        0u, 1u, 2u, 3u, 0u, 1u, 2u, 3u,
        4u, 5u, 4u, 5u, 6u, 7u, 6u, 7u, 8u
    };
    static const uint16_t category_base[6] = {5u, 7u, 11u, 19u, 35u, 67u};
    static const uint8_t category_length[6] = {1u, 2u, 3u, 4u, 5u, 11u};
    static const uint8_t category_probability[6][11] = {
        {159u},
        {145u, 165u},
        {140u, 148u, 173u},
        {135u, 140u, 155u, 176u},
        {130u, 134u, 141u, 157u, 180u},
        {129u, 130u, 133u, 140u, 153u, 177u, 196u, 230u, 243u, 254u, 254u}
    };
    size_t block_order[25];
    size_t block_count = 0u;

    if (!reader || !probabilities || !left_context || !above_context ||
        !coefficients || y_mode > 4u) {
        return 0;
    }
    memset(coefficients, 0, 25u * 16u * sizeof(coefficients[0][0]));
    if (y_mode != 4u) block_order[block_count++] = 24u;
    for (size_t block = 0u; block < 16u; ++block)
        block_order[block_count++] = block;
    for (size_t block = 16u; block < 24u; ++block)
        block_order[block_count++] = block;

    for (size_t order = 0u; order < block_count; ++order) {
        size_t block = block_order[order];
        size_t context;
        size_t coefficient;
        size_t start_coefficient;
        size_t token_type;
        uint8_t block_has_coefficients = 0u;
        uint8_t previous_coefficient_was_zero = 0u;

        if (block == 24u) {
            token_type = 1u;
            start_coefficient = 0u;
        } else if (block < 16u) {
            token_type = y_mode == 4u ? 3u : 0u;
            start_coefficient = y_mode == 4u ? 0u : 1u;
        } else {
            token_type = 2u;
            /* UV blocks include coefficient zero, like B_PRED luma blocks. */
            start_coefficient = 0u;
        }

        context = (size_t)left_context[left_context_index[block]] +
                  above_context[above_context_index[block]];
        if (context > 2u) return 0;
        coefficient = start_coefficient;
        while (coefficient < 16u) {
            const uint8_t *node_probabilities = probabilities[token_type]
                [coefficient_band[coefficient]][context];
            uint32_t token;
            uint32_t magnitude;
            uint32_t negative;
            size_t start_node = previous_coefficient_was_zero ? 2u : 0u;

            if (!webp_vp8_read_tree(reader, coefficient_tree,
                                    sizeof(coefficient_tree),
                                    node_probabilities, start_node, &token) ||
                token > 11u) {
                return 0;
            }
            if (token == 11u) break;

            if (token == 0u) {
                magnitude = 0u;
            } else if (token <= 4u) {
                magnitude = token;
            } else {
                size_t category = (size_t)token - 5u;
                uint8_t length = category_length[category];
                magnitude = category_base[category];
                for (size_t bit_index = length; bit_index > 0u; --bit_index) {
                    size_t bit_position = bit_index - 1u;
                    uint32_t bit;
                    if (!webp_vp8_read_bool(
                            reader,
                            category_probability[category][bit_position],
                            &bit)) {
                        return 0;
                    }
                    magnitude += bit << bit_position;
                }
            }
            if (magnitude) {
                if (!webp_vp8_read_bool(reader, 128u, &negative)) return 0;
                coefficients[block][zigzag[coefficient]] = (int16_t)(
                    negative ? -(int32_t)magnitude : (int32_t)magnitude);
                block_has_coefficients = 1u;
            }
            context = magnitude == 0u ? 0u : (magnitude == 1u ? 1u : 2u);
            previous_coefficient_was_zero = (uint8_t)(magnitude == 0u);
            ++coefficient;
        }
        {
            left_context[left_context_index[block]] = block_has_coefficients;
            above_context[above_context_index[block]] = block_has_coefficients;
        }
    }
    return 1;
}

int qw_webp_vp8_decode_coefficients(
    const uint8_t *data,
    size_t size,
    const QwWebPVP8Header *header,
    const QwWebPVP8Macroblock *macroblocks,
    size_t macroblock_count,
    QwWebPVP8MacroblockConsumer consumer,
    void *consumer_context)
{
    QwWebPVP8TokenPartition partitions[8] = {0};
    QwWebPVP8BoolReader readers[8];
    uint8_t left_context[8][9] = {{0}};
    uint8_t *above_context = NULL;
    size_t partition_count = 0u;
    size_t macroblock_columns;
    size_t macroblock_rows;
    size_t expected_count;
    size_t above_context_count;
    int rc;

    if (!data || !header || !macroblocks)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (!qw_image_dimensions_within_limits(header->width, header->height))
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    macroblock_columns = ((size_t)header->width + 15u) / 16u;
    macroblock_rows = ((size_t)header->height + 15u) / 16u;
    if (!qw_checked_size_mul(macroblock_columns, macroblock_rows,
                             &expected_count) ||
        expected_count != macroblock_count ||
        !qw_checked_size_mul(macroblock_columns, 9u,
                             &above_context_count)) {
        return QW_STATUS_ERR_FORMAT;
    }
    rc = qw_webp_vp8_parse_token_partitions(
        data, size, header, partitions, &partition_count);
    if (rc != QW_STATUS_OK) return rc;
    for (size_t i = 0u; i < partition_count; ++i) {
        if (!webp_vp8_bool_reader_init(&readers[i], partitions[i].data,
                                       partitions[i].size)) {
            return QW_STATUS_ERR_FORMAT;
        }
    }
    above_context = (uint8_t *)calloc(above_context_count, sizeof(*above_context));
    if (!above_context) return QW_STATUS_ERR_OUT_OF_MEMORY;

    for (size_t mb_y = 0u; mb_y < macroblock_rows; ++mb_y) {
        size_t partition = mb_y % partition_count;
        uint8_t *left = left_context[partition];
        memset(left, 0, 9u);
        for (size_t mb_x = 0u; mb_x < macroblock_columns; ++mb_x) {
            const QwWebPVP8Macroblock *macroblock =
                &macroblocks[mb_y * macroblock_columns + mb_x];
            uint8_t *above = above_context + mb_x * 9u;
            int16_t coefficients[25][16];
            if (macroblock->y_mode > 4u || macroblock->segment_id > 3u ||
                macroblock->skip_coeff > 1u) {
                rc = QW_STATUS_ERR_FORMAT;
                goto done;
            }
            if (macroblock->skip_coeff) {
                memset(left, 0, 8u);
                memset(above, 0, 8u);
                if (macroblock->y_mode != 4u) {
                    left[8] = 0u;
                    above[8] = 0u;
                }
                memset(coefficients, 0, sizeof(coefficients));
                if (consumer) {
                    rc = consumer(consumer_context, mb_x, mb_y,
                                  macroblock, coefficients);
                    if (rc != QW_STATUS_OK) goto done;
                }
                continue;
            }
            if (!webp_vp8_decode_mb_coefficients(
                    &readers[partition], header->coefficient_probabilities,
                    macroblock->y_mode, left, above, coefficients)) {
                rc = QW_STATUS_ERR_FORMAT;
                goto done;
            }
            if (consumer) {
                rc = consumer(consumer_context, mb_x, mb_y,
                              macroblock, coefficients);
                if (rc != QW_STATUS_OK) goto done;
            }
        }
    }
    rc = QW_STATUS_OK;

done:
    free(above_context);
    return rc;
}

int qw_webp_vp8_decode_token_coefficients(
    const uint8_t *data,
    size_t size,
    const QwWebPVP8Header *header,
    const QwWebPVP8Macroblock *macroblocks,
    size_t macroblock_count)
{
    return qw_webp_vp8_decode_coefficients(
        data, size, header, macroblocks, macroblock_count, NULL, NULL);
}

int qw_webp_vp8_parse_token_partitions(
    const uint8_t *data,
    size_t size,
    const QwWebPVP8Header *header,
    QwWebPVP8TokenPartition out_partitions[8],
    size_t *out_partition_count)
{
    size_t count;
    size_t table_size;
    size_t table_position;
    size_t data_position;
    size_t remaining;

    if (!out_partitions || !out_partition_count)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_partitions, 0, 8u * sizeof(*out_partitions));
    *out_partition_count = 0u;
    if (!data || !header || size < 10u ||
        header->first_partition_offset > size ||
        header->token_partition_count == 0u ||
        header->token_partition_count > 8u) {
        return QW_STATUS_ERR_FORMAT;
    }
    count = header->token_partition_count;
    table_size = (count - 1u) * 3u;
    remaining = size - header->first_partition_offset;
    if (table_size > remaining) return QW_STATUS_ERR_FORMAT;
    table_position = header->first_partition_offset;
    data_position = table_position + table_size;
    remaining -= table_size;

    for (size_t i = 0u; i < count; ++i) {
        size_t partition_size;
        if (i + 1u == count) {
            partition_size = remaining;
        } else {
            const uint8_t *encoded_size = data + table_position + i * 3u;
            partition_size = (size_t)encoded_size[0] |
                ((size_t)encoded_size[1] << 8) |
                ((size_t)encoded_size[2] << 16);
            if (partition_size > remaining) return QW_STATUS_ERR_FORMAT;
        }
        if (partition_size < 2u) return QW_STATUS_ERR_FORMAT;
        out_partitions[i].data = data + data_position;
        out_partitions[i].size = partition_size;
        data_position += partition_size;
        remaining -= partition_size;
    }
    if (remaining != 0u || data_position != size)
        return QW_STATUS_ERR_FORMAT;
    *out_partition_count = count;
    return QW_STATUS_OK;
}
