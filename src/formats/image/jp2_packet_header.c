#include "jp2_packet_header.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct QwJp2PacketTagTreeLevel {
    uint32_t width;
    uint32_t height;
    size_t offset;
} QwJp2PacketTagTreeLevel;

typedef struct QwJp2PacketTagTreeNode {
    uint32_t value;
    uint32_t low;
} QwJp2PacketTagTreeNode;

struct QwJp2PacketTagTree {
    uint32_t width;
    uint32_t height;
    size_t node_count;
    uint32_t level_count;
    QwJp2PacketTagTreeLevel *levels;
    QwJp2PacketTagTreeNode *nodes;
};

int qw_jp2_packet_bit_reader_init(QwJp2PacketBitReader *reader,
                                  const uint8_t *data,
                                  size_t size)
{
    if (!reader || (!data && size != 0u)) return QW_JP2_PACKET_ERR_ARGUMENT;
    memset(reader, 0, sizeof(*reader));
    reader->data = data;
    reader->size = size;
    return QW_JP2_PACKET_OK;
}

int qw_jp2_packet_read_bit(QwJp2PacketBitReader *reader, uint8_t *out_bit)
{
    QwJp2PacketBitReader next;
    uint8_t bit;

    if (!reader || !out_bit) return QW_JP2_PACKET_ERR_ARGUMENT;
    next = *reader;
    if (next.bits_remaining == 0u) {
        if (next.byte_offset >= next.size) {
            return QW_JP2_PACKET_ERR_FORMAT;
        }
        next.current_byte = next.data[next.byte_offset++];
        if (next.previous_was_ff) {
            if ((next.current_byte & 0x80u) != 0u) {
                return QW_JP2_PACKET_ERR_FORMAT;
            }
            next.bits_remaining = 7u;
        } else {
            next.bits_remaining = 8u;
        }
    }

    if (next.bits_remaining > 8u) return QW_JP2_PACKET_ERR_FORMAT;
    bit = (uint8_t)((next.current_byte >> (next.bits_remaining - 1u)) & 1u);
    --next.bits_remaining;
    if (next.bits_remaining == 0u) {
        next.previous_was_ff = next.current_byte == 0xffu;
    }
    *reader = next;
    *out_bit = bit;
    return QW_JP2_PACKET_OK;
}

int qw_jp2_packet_read_bits(QwJp2PacketBitReader *reader,
                            uint8_t bit_count,
                            uint32_t *out_value)
{
    QwJp2PacketBitReader next;
    uint32_t value = 0u;

    if (!reader || !out_value || bit_count > 32u) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }
    next = *reader;
    for (uint8_t i = 0u; i < bit_count; ++i) {
        uint8_t bit;
        if (qw_jp2_packet_read_bit(&next, &bit) != QW_JP2_PACKET_OK) {
            return QW_JP2_PACKET_ERR_FORMAT;
        }
        value = (value << 1) | (uint32_t)bit;
    }
    *reader = next;
    *out_value = value;
    return QW_JP2_PACKET_OK;
}

int qw_jp2_packet_read_coding_passes(QwJp2PacketBitReader *reader,
                                     uint16_t *out_passes)
{
    QwJp2PacketBitReader next;
    uint8_t bit;
    uint32_t value;
    uint16_t passes;

    if (!reader || !out_passes) return QW_JP2_PACKET_ERR_ARGUMENT;
    next = *reader;
    if (qw_jp2_packet_read_bit(&next, &bit) != QW_JP2_PACKET_OK) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    if (!bit) {
        passes = 1u;
    } else if (qw_jp2_packet_read_bit(&next, &bit) != QW_JP2_PACKET_OK) {
        return QW_JP2_PACKET_ERR_FORMAT;
    } else if (!bit) {
        passes = 2u;
    } else if (qw_jp2_packet_read_bit(&next, &bit) != QW_JP2_PACKET_OK) {
        return QW_JP2_PACKET_ERR_FORMAT;
    } else if (!bit) {
        if (qw_jp2_packet_read_bit(&next, &bit) != QW_JP2_PACKET_OK) {
            return QW_JP2_PACKET_ERR_FORMAT;
        }
        passes = (uint16_t)(3u + bit);
    } else if (qw_jp2_packet_read_bit(&next, &bit) != QW_JP2_PACKET_OK) {
        return QW_JP2_PACKET_ERR_FORMAT;
    } else if (!bit) {
        passes = 5u;
    } else {
        if (qw_jp2_packet_read_bits(&next, 5u, &value) != QW_JP2_PACKET_OK) {
            return QW_JP2_PACKET_ERR_FORMAT;
        }
        if (value < 31u) {
            passes = (uint16_t)(6u + value);
        } else {
            if (qw_jp2_packet_read_bits(&next, 7u, &value) !=
                QW_JP2_PACKET_OK) {
                return QW_JP2_PACKET_ERR_FORMAT;
            }
            passes = (uint16_t)(37u + value);
        }
    }

    if (passes == 0u || passes > QW_JP2_PACKET_MAX_PASSES) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    *reader = next;
    *out_passes = passes;
    return QW_JP2_PACKET_OK;
}

static uint8_t qw_jp2_packet_floor_log2(uint16_t value)
{
    uint8_t result = 0u;
    while (value > 1u) {
        value >>= 1;
        ++result;
    }
    return result;
}

int qw_jp2_packet_read_segment_lengths(
    QwJp2PacketBitReader *reader,
    uint8_t *lblock,
    const uint16_t *pass_counts,
    size_t segment_count,
    uint32_t *lengths,
    size_t length_capacity)
{
    QwJp2PacketBitReader next;
    uint32_t parsed_lengths[QW_JP2_PACKET_MAX_PASSES];
    uint32_t total_passes = 0u;
    uint8_t next_lblock;
    size_t index;

    if (!reader || !lblock || !pass_counts || !lengths ||
        segment_count == 0u ||
        segment_count > QW_JP2_PACKET_MAX_PASSES ||
        length_capacity < segment_count || *lblock < 3u ||
        *lblock > QW_JP2_PACKET_MAX_LENGTH_BITS) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }

    next = *reader;
    next_lblock = *lblock;
    for (;;) {
        uint8_t increment;
        if (qw_jp2_packet_read_bit(&next, &increment) != QW_JP2_PACKET_OK) {
            return QW_JP2_PACKET_ERR_FORMAT;
        }
        if (!increment) break;
        if (next_lblock == QW_JP2_PACKET_MAX_LENGTH_BITS) {
            return QW_JP2_PACKET_ERR_FORMAT;
        }
        ++next_lblock;
    }

    for (index = 0u; index < segment_count; ++index) {
        uint8_t bit_count;
        uint16_t passes = pass_counts[index];
        if (passes == 0u || passes > QW_JP2_PACKET_MAX_PASSES ||
            total_passes + passes > QW_JP2_PACKET_MAX_PASSES) {
            return QW_JP2_PACKET_ERR_FORMAT;
        }
        total_passes += passes;
        bit_count = (uint8_t)(next_lblock +
                              qw_jp2_packet_floor_log2(passes));
        if (bit_count > QW_JP2_PACKET_MAX_LENGTH_BITS ||
            qw_jp2_packet_read_bits(&next, bit_count,
                                   &parsed_lengths[index]) !=
                QW_JP2_PACKET_OK) {
            return QW_JP2_PACKET_ERR_FORMAT;
        }
    }

    *reader = next;
    *lblock = next_lblock;
    memcpy(lengths, parsed_lengths, segment_count * sizeof(*lengths));
    return QW_JP2_PACKET_OK;
}

int qw_jp2_packet_header_align(QwJp2PacketBitReader *reader)
{
    QwJp2PacketBitReader next;
    int trailing_ff;

    if (!reader) return QW_JP2_PACKET_ERR_ARGUMENT;
    next = *reader;
    if (next.byte_offset > next.size || next.bits_remaining > 8u ||
        (!next.data && next.size != 0u)) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    trailing_ff = next.bits_remaining == 0u
                      ? next.previous_was_ff != 0u
                      : next.current_byte == 0xffu;
    if (trailing_ff) {
        if (next.byte_offset >= next.size ||
            (next.data[next.byte_offset] & 0x80u) != 0u) {
            return QW_JP2_PACKET_ERR_FORMAT;
        }
        ++next.byte_offset;
    }
    next.bits_remaining = 0u;
    next.previous_was_ff = 0u;
    *reader = next;
    return QW_JP2_PACKET_OK;
}

size_t qw_jp2_packet_header_bytes_consumed(
    const QwJp2PacketBitReader *reader)
{
    return reader ? reader->byte_offset : 0u;
}

int qw_jp2_packet_tag_tree_create(uint32_t width,
                                  uint32_t height,
                                  QwJp2PacketTagTree **out_tree)
{
    QwJp2PacketTagTree *tree;
    uint32_t level_width = width;
    uint32_t level_height = height;
    size_t node_count = 0u;
    uint32_t level_count = 0u;

    if (out_tree) *out_tree = NULL;
    if (!out_tree || width == 0u || height == 0u) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }

    while (1) {
        size_t level_nodes;
        if (level_count >= QW_JP2_PACKET_MAX_TAG_TREE_LEVELS ||
            (size_t)level_width > SIZE_MAX / (size_t)level_height) {
            return QW_JP2_PACKET_ERR_RESOURCE;
        }
        level_nodes = (size_t)level_width * (size_t)level_height;
        if (level_nodes > SIZE_MAX - node_count) {
            return QW_JP2_PACKET_ERR_RESOURCE;
        }
        node_count += level_nodes;
        if (node_count > QW_JP2_PACKET_MAX_TAG_TREE_NODES) {
            return QW_JP2_PACKET_ERR_RESOURCE;
        }
        ++level_count;
        if (level_width == 1u && level_height == 1u) break;
        level_width = level_width / 2u + level_width % 2u;
        level_height = level_height / 2u + level_height % 2u;
    }
    if (node_count > SIZE_MAX / sizeof(QwJp2PacketTagTreeNode) ||
        (size_t)level_count > SIZE_MAX / sizeof(QwJp2PacketTagTreeLevel)) {
        return QW_JP2_PACKET_ERR_RESOURCE;
    }

    tree = (QwJp2PacketTagTree *)calloc(1u, sizeof(*tree));
    if (!tree) return QW_JP2_PACKET_ERR_MEMORY;
    tree->levels = (QwJp2PacketTagTreeLevel *)calloc(
        (size_t)level_count, sizeof(*tree->levels));
    tree->nodes = (QwJp2PacketTagTreeNode *)malloc(
        node_count * sizeof(*tree->nodes));
    if (!tree->levels || !tree->nodes) {
        qw_jp2_packet_tag_tree_free(tree);
        return QW_JP2_PACKET_ERR_MEMORY;
    }

    tree->width = width;
    tree->height = height;
    tree->node_count = node_count;
    tree->level_count = level_count;
    level_width = width;
    level_height = height;
    node_count = 0u;
    for (uint32_t level = 0u; level < level_count; ++level) {
        size_t level_nodes = (size_t)level_width * (size_t)level_height;
        tree->levels[level].width = level_width;
        tree->levels[level].height = level_height;
        tree->levels[level].offset = node_count;
        node_count += level_nodes;
        if (level + 1u < level_count) {
            level_width = level_width / 2u + level_width % 2u;
            level_height = level_height / 2u + level_height % 2u;
        }
    }
    for (size_t i = 0u; i < tree->node_count; ++i) {
        tree->nodes[i].value = UINT32_MAX;
        tree->nodes[i].low = 0u;
    }

    *out_tree = tree;
    return QW_JP2_PACKET_OK;
}

void qw_jp2_packet_tag_tree_free(QwJp2PacketTagTree *tree)
{
    if (!tree) return;
    free(tree->nodes);
    free(tree->levels);
    free(tree);
}

int qw_jp2_packet_tag_tree_decode(QwJp2PacketBitReader *reader,
                                  QwJp2PacketTagTree *tree,
                                  uint32_t x,
                                  uint32_t y,
                                  uint32_t threshold,
                                  uint8_t *out_below_threshold)
{
    QwJp2PacketTagTreeNode path[QW_JP2_PACKET_MAX_TAG_TREE_LEVELS];
    size_t path_indices[QW_JP2_PACKET_MAX_TAG_TREE_LEVELS];
    QwJp2PacketBitReader next;
    uint32_t low = 0u;

    if (!reader || !tree || !out_below_threshold ||
        x >= tree->width || y >= tree->height ||
        threshold > QW_JP2_PACKET_MAX_TAG_TREE_THRESHOLD) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }
    if (threshold == 0u) {
        *out_below_threshold = 0u;
        return QW_JP2_PACKET_OK;
    }

    next = *reader;
    for (uint32_t reverse = tree->level_count; reverse > 0u; --reverse) {
        uint32_t level = reverse - 1u;
        const QwJp2PacketTagTreeLevel *dimensions = &tree->levels[level];
        uint32_t leaf_x = level < sizeof(uint32_t) * CHAR_BIT
                              ? x >> level
                              : 0u;
        uint32_t leaf_y = level < sizeof(uint32_t) * CHAR_BIT
                              ? y >> level
                              : 0u;
        size_t index = dimensions->offset +
                       (size_t)leaf_y * dimensions->width + (size_t)leaf_x;
        QwJp2PacketTagTreeNode node = tree->nodes[index];

        path_indices[level] = index;
        if (node.value < low) low = node.value;
        if (node.low > low) low = node.low;
        while (low < threshold && low < node.value) {
            uint8_t bit;
            if (qw_jp2_packet_read_bit(&next, &bit) != QW_JP2_PACKET_OK) {
                return QW_JP2_PACKET_ERR_FORMAT;
            }
            if (bit) {
                node.value = low;
            } else {
                ++low;
            }
        }
        node.low = low;
        path[level] = node;
    }

    for (uint32_t level = 0u; level < tree->level_count; ++level) {
        tree->nodes[path_indices[level]] = path[level];
    }
    *reader = next;
    *out_below_threshold =
        (uint8_t)(path[0].value < threshold ? 1u : 0u);
    return QW_JP2_PACKET_OK;
}

typedef struct QwJp2PacketTagTreeSnapshot {
    size_t indices[QW_JP2_PACKET_MAX_TAG_TREE_LEVELS];
    QwJp2PacketTagTreeNode nodes[QW_JP2_PACKET_MAX_TAG_TREE_LEVELS];
    uint32_t level_count;
} QwJp2PacketTagTreeSnapshot;

static int qw_jp2_packet_tag_tree_snapshot(
    const QwJp2PacketTagTree *tree,
    uint32_t x,
    uint32_t y,
    QwJp2PacketTagTreeSnapshot *snapshot)
{
    if (!tree || !snapshot || x >= tree->width || y >= tree->height ||
        tree->level_count > QW_JP2_PACKET_MAX_TAG_TREE_LEVELS) {
        return 0;
    }
    snapshot->level_count = tree->level_count;
    for (uint32_t level = 0u; level < tree->level_count; ++level) {
        const QwJp2PacketTagTreeLevel *dimensions = &tree->levels[level];
        uint32_t leaf_x = level < sizeof(uint32_t) * CHAR_BIT
                              ? x >> level
                              : 0u;
        uint32_t leaf_y = level < sizeof(uint32_t) * CHAR_BIT
                              ? y >> level
                              : 0u;
        size_t index = dimensions->offset +
                       (size_t)leaf_y * dimensions->width + (size_t)leaf_x;
        snapshot->indices[level] = index;
        snapshot->nodes[level] = tree->nodes[index];
    }
    return 1;
}

static void qw_jp2_packet_tag_tree_restore(
    QwJp2PacketTagTree *tree,
    const QwJp2PacketTagTreeSnapshot *snapshot)
{
    if (!tree || !snapshot) return;
    for (uint32_t level = 0u; level < snapshot->level_count; ++level) {
        tree->nodes[snapshot->indices[level]] = snapshot->nodes[level];
    }
}

int qw_jp2_packet_codeblock_state_init(
    QwJp2PacketCodeBlockState *state,
    uint8_t max_bitplanes)
{
    if (!state || max_bitplanes == 0u ||
        max_bitplanes > QW_JP2_PACKET_MAX_BIT_PLANES) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }
    memset(state, 0, sizeof(*state));
    state->max_bitplanes = max_bitplanes;
    state->lblock = 3u;
    state->initialized = 1u;
    return QW_JP2_PACKET_OK;
}

static uint16_t qw_jp2_packet_max_codeblock_passes(uint8_t bitplanes)
{
    return bitplanes ? (uint16_t)(3u * (uint16_t)bitplanes - 2u) : 0u;
}

int qw_jp2_packet_read_codeblock_header_with_segments(
    QwJp2PacketBitReader *reader,
    QwJp2PacketTagTree *inclusion_tree,
    QwJp2PacketTagTree *zero_bitplane_tree,
    uint32_t x,
    uint32_t y,
    uint32_t layer_index,
    uint8_t codeblock_style,
    QwJp2PacketCodeBlockState *state,
    QwJp2PacketCodeBlockContribution *out_contribution,
    uint32_t *segment_lengths,
    size_t segment_length_capacity,
    size_t *inout_segment_length_count)
{
    QwJp2PacketTagTreeSnapshot inclusion_snapshot;
    QwJp2PacketTagTreeSnapshot zero_bitplane_snapshot;
    QwJp2PacketCodeBlockState next_state;
    QwJp2PacketCodeBlockContribution contribution;
    QwJp2PacketBitReader next;
    uint16_t pass_count;
    uint16_t pass_counts[QW_JP2_PACKET_MAX_PASSES];
    uint32_t lengths[QW_JP2_PACKET_MAX_PASSES];
    size_t segment_count;
    uint64_t total_length = 0u;
    uint8_t lblock;
    uint8_t included;
    uint8_t zero_bitplanes = 0u;
    uint8_t first_inclusion = 0u;
    uint8_t found_zero_bitplanes = 0u;
    uint32_t maximum_passes;
    uint32_t actual_bitplanes;
    int rc;

    if (!reader || !inclusion_tree || !zero_bitplane_tree || !state ||
        !out_contribution || state->initialized != 1u ||
        (!segment_lengths &&
         (segment_length_capacity != 0u ||
          inout_segment_length_count != NULL)) ||
        (segment_lengths &&
         (!inout_segment_length_count ||
          *inout_segment_length_count > segment_length_capacity)) ||
        state->max_bitplanes == 0u ||
        state->max_bitplanes > QW_JP2_PACKET_MAX_BIT_PLANES ||
        state->included > 1u ||
        state->lblock < 3u ||
        state->lblock > QW_JP2_PACKET_MAX_LENGTH_BITS ||
        layer_index >= QW_JP2_PACKET_MAX_TAG_TREE_THRESHOLD ||
        x >= inclusion_tree->width || y >= inclusion_tree->height ||
        x >= zero_bitplane_tree->width || y >= zero_bitplane_tree->height) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }
    memset(&contribution, 0, sizeof(contribution));
    if (codeblock_style & 0x01u) return QW_JP2_PACKET_ERR_UNSUPPORTED;
    if (codeblock_style & 0xc0u) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    if ((!state->included && state->coding_passes != 0u) ||
        (!state->included &&
         (state->zero_bitplanes != 0u || state->lblock != 3u)) ||
        (state->included &&
         (state->zero_bitplanes >= state->max_bitplanes ||
          state->coding_passes == 0u))) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    if (state->included) {
        uint8_t actual_bitplanes = (uint8_t)(state->max_bitplanes -
                                             state->zero_bitplanes);
        if (state->coding_passes >
            qw_jp2_packet_max_codeblock_passes(actual_bitplanes)) {
            return QW_JP2_PACKET_ERR_FORMAT;
        }
    }

    if (!qw_jp2_packet_tag_tree_snapshot(inclusion_tree, x, y,
                                         &inclusion_snapshot) ||
        !qw_jp2_packet_tag_tree_snapshot(zero_bitplane_tree, x, y,
                                         &zero_bitplane_snapshot)) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }
    next = *reader;
    next_state = *state;

    if (!state->included) {
        rc = qw_jp2_packet_tag_tree_decode(
            &next, inclusion_tree, x, y, layer_index + 1u, &included);
        if (rc != QW_JP2_PACKET_OK) {
            goto rollback;
        }
        if (!included) {
            *reader = next;
            *out_contribution = contribution;
            return QW_JP2_PACKET_OK;
        }
        first_inclusion = 1u;
        for (uint32_t threshold = 1u;
             threshold <= state->max_bitplanes; ++threshold) {
            rc = qw_jp2_packet_tag_tree_decode(
                &next, zero_bitplane_tree, x, y, threshold,
                &found_zero_bitplanes);
            if (rc != QW_JP2_PACKET_OK) {
                goto rollback;
            }
            if (found_zero_bitplanes) {
                zero_bitplanes = (uint8_t)(threshold - 1u);
                break;
            }
        }
        if (!found_zero_bitplanes) {
            rc = QW_JP2_PACKET_ERR_FORMAT;
            goto rollback;
        }
    } else {
        if (qw_jp2_packet_read_bit(&next, &included) != QW_JP2_PACKET_OK) {
            return QW_JP2_PACKET_ERR_FORMAT;
        }
        if (!included) {
            *reader = next;
            *out_contribution = contribution;
            return QW_JP2_PACKET_OK;
        }
        zero_bitplanes = state->zero_bitplanes;
    }

    if (qw_jp2_packet_read_coding_passes(&next, &pass_count) !=
        QW_JP2_PACKET_OK) {
        rc = QW_JP2_PACKET_ERR_FORMAT;
        goto rollback;
    }
    actual_bitplanes = (uint32_t)state->max_bitplanes - zero_bitplanes;
    maximum_passes = qw_jp2_packet_max_codeblock_passes(
        (uint8_t)actual_bitplanes);
    if (actual_bitplanes == 0u ||
        state->coding_passes > maximum_passes ||
        pass_count > maximum_passes - state->coding_passes) {
        rc = QW_JP2_PACKET_ERR_FORMAT;
        goto rollback;
    }

    segment_count = (codeblock_style & 0x04u) != 0u
                        ? (size_t)pass_count : 1u;
    if (segment_count > QW_JP2_PACKET_MAX_PASSES) {
        rc = QW_JP2_PACKET_ERR_RESOURCE;
        goto rollback;
    }
    if (segment_count == 1u) {
        pass_counts[0] = pass_count;
    } else {
        for (size_t i = 0u; i < segment_count; ++i) {
            pass_counts[i] = 1u;
        }
    }
    lblock = state->lblock;
    if (qw_jp2_packet_read_segment_lengths(
            &next, &lblock, pass_counts, segment_count, lengths,
            QW_JP2_PACKET_MAX_PASSES) !=
        QW_JP2_PACKET_OK) {
        rc = QW_JP2_PACKET_ERR_FORMAT;
        goto rollback;
    }
    for (size_t i = 0u; i < segment_count; ++i) {
        total_length += lengths[i];
        if (total_length > UINT32_MAX) {
            rc = QW_JP2_PACKET_ERR_FORMAT;
            goto rollback;
        }
    }
    if (segment_lengths) {
        if (segment_count >
            segment_length_capacity - *inout_segment_length_count) {
            rc = QW_JP2_PACKET_ERR_RESOURCE;
            goto rollback;
        }
        memcpy(segment_lengths + *inout_segment_length_count, lengths,
               segment_count * sizeof(*segment_lengths));
        contribution.segment_offset = *inout_segment_length_count;
    }

    next_state.included = 1u;
    next_state.zero_bitplanes = zero_bitplanes;
    next_state.coding_passes =
        (uint16_t)(state->coding_passes + pass_count);
    next_state.lblock = lblock;
    contribution.included = 1u;
    contribution.first_inclusion = first_inclusion;
    contribution.zero_bitplanes = zero_bitplanes;
    contribution.coding_passes = pass_count;
    contribution.segment_count = (uint16_t)segment_count;
    contribution.codeword_segment_length = (uint32_t)total_length;
    *reader = next;
    *state = next_state;
    *out_contribution = contribution;
    if (segment_lengths) {
        *inout_segment_length_count += segment_count;
    }
    return QW_JP2_PACKET_OK;

rollback:
    qw_jp2_packet_tag_tree_restore(inclusion_tree, &inclusion_snapshot);
    qw_jp2_packet_tag_tree_restore(zero_bitplane_tree,
                                   &zero_bitplane_snapshot);
    return rc;
}

int qw_jp2_packet_read_codeblock_header(
    QwJp2PacketBitReader *reader,
    QwJp2PacketTagTree *inclusion_tree,
    QwJp2PacketTagTree *zero_bitplane_tree,
    uint32_t x,
    uint32_t y,
    uint32_t layer_index,
    uint8_t codeblock_style,
    QwJp2PacketCodeBlockState *state,
    QwJp2PacketCodeBlockContribution *out_contribution)
{
    return qw_jp2_packet_read_codeblock_header_with_segments(
        reader, inclusion_tree, zero_bitplane_tree, x, y, layer_index,
        codeblock_style, state, out_contribution, NULL, 0u, NULL);
}

int qw_jp2_packet_read_header_with_segments(
    QwJp2PacketBitReader *reader,
    QwJp2PacketSubbandHeaderState *subbands,
    size_t subband_count,
    uint32_t layer_index,
    uint8_t codeblock_style,
    QwJp2PacketCodeBlockContribution *contributions,
    size_t contribution_capacity,
    size_t *out_contribution_count,
    uint8_t *out_is_empty,
    size_t *out_header_bytes,
    size_t *out_body_bytes,
    uint32_t *segment_lengths,
    size_t segment_length_capacity,
    size_t *out_segment_length_count)
{
    QwJp2PacketBitReader next;
    size_t header_start;
    size_t contribution_count = 0u;
    size_t body_bytes = 0u;
    size_t segment_length_count = 0u;
    uint8_t has_contributions;

    if (!reader || (!subbands && subband_count != 0u) ||
        subband_count > QW_JP2_PACKET_MAX_SUBBANDS ||
        (!contributions && contribution_capacity != 0u) ||
        (!segment_lengths &&
         (segment_length_capacity != 0u ||
          out_segment_length_count != NULL)) ||
        (segment_lengths && !out_segment_length_count) ||
        !out_contribution_count || !out_is_empty || !out_header_bytes ||
        !out_body_bytes ||
        reader->bits_remaining != 0u || reader->byte_offset > reader->size ||
        (!reader->data && reader->size != 0u)) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }
    if (codeblock_style & 0x01u) return QW_JP2_PACKET_ERR_UNSUPPORTED;
    if (codeblock_style & 0xc0u) return QW_JP2_PACKET_ERR_FORMAT;

    header_start = reader->byte_offset;
    next = *reader;
    if (qw_jp2_packet_read_bit(&next, &has_contributions) !=
        QW_JP2_PACKET_OK) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    if (has_contributions) {
        if (layer_index >= QW_JP2_PACKET_MAX_TAG_TREE_THRESHOLD) {
            return QW_JP2_PACKET_ERR_ARGUMENT;
        }
        for (size_t band = 0u; band < subband_count; ++band) {
            const QwJp2PacketSubbandHeaderState *subband = &subbands[band];
            size_t band_codeblock_count;

            if (subband->codeblocks_wide == 0u ||
                subband->codeblocks_high == 0u) {
                if (subband->codeblocks_wide != 0u ||
                    subband->codeblocks_high != 0u) {
                    return QW_JP2_PACKET_ERR_FORMAT;
                }
                continue;
            }
            if ((size_t)subband->codeblocks_wide >
                    SIZE_MAX / (size_t)subband->codeblocks_high ||
                !subband->inclusion_tree || !subband->zero_bitplane_tree ||
                !subband->codeblocks) {
                return QW_JP2_PACKET_ERR_ARGUMENT;
            }
            band_codeblock_count =
                (size_t)subband->codeblocks_wide *
                (size_t)subband->codeblocks_high;
            if (subband->inclusion_tree->width !=
                    subband->codeblocks_wide ||
                subband->inclusion_tree->height !=
                    subband->codeblocks_high ||
                subband->zero_bitplane_tree->width !=
                    subband->codeblocks_wide ||
                subband->zero_bitplane_tree->height !=
                    subband->codeblocks_high ||
                band_codeblock_count > subband->codeblock_state_capacity ||
                band_codeblock_count >
                    (size_t)QW_JP2_PACKET_MAX_TAG_TREE_NODES -
                        contribution_count) {
                return QW_JP2_PACKET_ERR_RESOURCE;
            }
            contribution_count += band_codeblock_count;
        }
        if (contribution_count == 0u) return QW_JP2_PACKET_ERR_FORMAT;
        if (contributions && contribution_count > contribution_capacity) {
            return QW_JP2_PACKET_ERR_RESOURCE;
        }
    }

    {
        size_t output_index = 0u;
        for (size_t band = 0u; has_contributions && band < subband_count;
             ++band) {
            QwJp2PacketSubbandHeaderState *subband = &subbands[band];
            for (uint32_t y = 0u; y < subband->codeblocks_high; ++y) {
                for (uint32_t x = 0u; x < subband->codeblocks_wide; ++x) {
                    size_t state_index = (size_t)y *
                                             subband->codeblocks_wide +
                                         x;
                    QwJp2PacketCodeBlockContribution skipped_contribution;
                    QwJp2PacketCodeBlockContribution *contribution =
                        contributions ? &contributions[output_index]
                                      : &skipped_contribution;
                    int rc = segment_lengths
                        ? qw_jp2_packet_read_codeblock_header_with_segments(
                              &next, subband->inclusion_tree,
                              subband->zero_bitplane_tree, x, y, layer_index,
                              codeblock_style,
                              &subband->codeblocks[state_index], contribution,
                              segment_lengths, segment_length_capacity,
                              &segment_length_count)
                        : qw_jp2_packet_read_codeblock_header(
                              &next, subband->inclusion_tree,
                              subband->zero_bitplane_tree, x, y, layer_index,
                              codeblock_style,
                              &subband->codeblocks[state_index], contribution);
                    if (rc != QW_JP2_PACKET_OK) {
                        return rc;
                    }
                    if ((size_t)contribution->codeword_segment_length >
                        SIZE_MAX - body_bytes) {
                        return QW_JP2_PACKET_ERR_RESOURCE;
                    }
                    body_bytes +=
                        (size_t)contribution->codeword_segment_length;
                    ++output_index;
                }
            }
        }
    }

    if (qw_jp2_packet_header_align(&next) != QW_JP2_PACKET_OK) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    *reader = next;
    *out_contribution_count = has_contributions ? contribution_count : 0u;
    *out_is_empty = (uint8_t)!has_contributions;
    *out_header_bytes = next.byte_offset - header_start;
    *out_body_bytes = body_bytes;
    if (out_segment_length_count) {
        *out_segment_length_count = segment_length_count;
    }
    return QW_JP2_PACKET_OK;
}

int qw_jp2_packet_read_header(
    QwJp2PacketBitReader *reader,
    QwJp2PacketSubbandHeaderState *subbands,
    size_t subband_count,
    uint32_t layer_index,
    uint8_t codeblock_style,
    QwJp2PacketCodeBlockContribution *contributions,
    size_t contribution_capacity,
    size_t *out_contribution_count,
    uint8_t *out_is_empty,
    size_t *out_header_bytes,
    size_t *out_body_bytes)
{
    return qw_jp2_packet_read_header_with_segments(
        reader, subbands, subband_count, layer_index, codeblock_style,
        contributions, contribution_capacity, out_contribution_count,
        out_is_empty, out_header_bytes, out_body_bytes, NULL, 0u, NULL);
}
