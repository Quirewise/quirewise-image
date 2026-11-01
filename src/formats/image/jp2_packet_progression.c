#include "jp2_packet_progression.h"

#include <stdlib.h>
#include <string.h>

typedef struct QwJp2PacketOrderSortEntry {
    QwJp2PacketOrderEntry packet;
    size_t original_index;
    uint8_t progression_order;
} QwJp2PacketOrderSortEntry;

static int qw_jp2_packet_order_compare_u64(uint64_t left, uint64_t right)
{
    if (left < right) return -1;
    if (left > right) return 1;
    return 0;
}

#define QW_JP2_PACKET_COMPARE_FIELD(field)                                  \
    do {                                                                     \
        int field_order = qw_jp2_packet_order_compare_u64(                   \
            (uint64_t)left->packet.field, (uint64_t)right->packet.field);    \
        if (field_order != 0) return field_order;                            \
    } while (0)

static int qw_jp2_packet_order_compare(const void *left_pointer,
                                      const void *right_pointer)
{
    const QwJp2PacketOrderSortEntry *left =
        (const QwJp2PacketOrderSortEntry *)left_pointer;
    const QwJp2PacketOrderSortEntry *right =
        (const QwJp2PacketOrderSortEntry *)right_pointer;

    switch (left->progression_order) {
    case QW_JP2_PROGRESSION_LRCP:
        QW_JP2_PACKET_COMPARE_FIELD(layer_index);
        QW_JP2_PACKET_COMPARE_FIELD(resolution_level);
        QW_JP2_PACKET_COMPARE_FIELD(component_index);
        QW_JP2_PACKET_COMPARE_FIELD(precinct_index);
        break;
    case QW_JP2_PROGRESSION_RLCP:
        QW_JP2_PACKET_COMPARE_FIELD(resolution_level);
        QW_JP2_PACKET_COMPARE_FIELD(layer_index);
        QW_JP2_PACKET_COMPARE_FIELD(component_index);
        QW_JP2_PACKET_COMPARE_FIELD(precinct_index);
        break;
    case QW_JP2_PROGRESSION_RPCL:
        QW_JP2_PACKET_COMPARE_FIELD(resolution_level);
        QW_JP2_PACKET_COMPARE_FIELD(reference_y);
        QW_JP2_PACKET_COMPARE_FIELD(reference_x);
        QW_JP2_PACKET_COMPARE_FIELD(component_index);
        QW_JP2_PACKET_COMPARE_FIELD(precinct_index);
        QW_JP2_PACKET_COMPARE_FIELD(layer_index);
        break;
    case QW_JP2_PROGRESSION_PCRL:
        QW_JP2_PACKET_COMPARE_FIELD(reference_y);
        QW_JP2_PACKET_COMPARE_FIELD(reference_x);
        QW_JP2_PACKET_COMPARE_FIELD(component_index);
        QW_JP2_PACKET_COMPARE_FIELD(resolution_level);
        QW_JP2_PACKET_COMPARE_FIELD(precinct_index);
        QW_JP2_PACKET_COMPARE_FIELD(layer_index);
        break;
    case QW_JP2_PROGRESSION_CPRL:
        QW_JP2_PACKET_COMPARE_FIELD(component_index);
        QW_JP2_PACKET_COMPARE_FIELD(reference_y);
        QW_JP2_PACKET_COMPARE_FIELD(reference_x);
        QW_JP2_PACKET_COMPARE_FIELD(resolution_level);
        QW_JP2_PACKET_COMPARE_FIELD(precinct_index);
        QW_JP2_PACKET_COMPARE_FIELD(layer_index);
        break;
    default:
        break;
    }

    if (left->original_index < right->original_index) return -1;
    if (left->original_index > right->original_index) return 1;
    return 0;
}

#undef QW_JP2_PACKET_COMPARE_FIELD

static uint16_t qw_jp2_packet_progression_read_u16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

static uint16_t qw_jp2_packet_progression_read_component(
    const uint8_t *data,
    uint8_t component_width)
{
    if (component_width == 1u) return data[0];
    return qw_jp2_packet_progression_read_u16(data);
}

static void qw_jp2_packet_progression_read_volume(
    const uint8_t *entry,
    uint8_t component_width,
    QwJp2PacketProgressionVolume *out_volume)
{
    size_t component_start_offset = 1u;
    size_t layer_end_offset = component_start_offset + component_width;
    size_t resolution_end_offset = layer_end_offset + 2u;
    size_t component_end_offset = resolution_end_offset + 1u;
    size_t progression_order_offset = component_end_offset + component_width;
    uint16_t component_end = qw_jp2_packet_progression_read_component(
        entry + component_end_offset, component_width);

    out_volume->resolution_start = entry[0];
    out_volume->component_start =
        qw_jp2_packet_progression_read_component(
            entry + component_start_offset, component_width);
    out_volume->layer_end =
        qw_jp2_packet_progression_read_u16(entry + layer_end_offset);
    out_volume->resolution_end = entry[resolution_end_offset];
    out_volume->component_end = component_end
                                    ? component_end
                                    : (component_width == 1u ? 256u : 16384u);
    out_volume->progression_order = entry[progression_order_offset];
}

int qw_jp2_packet_progression_parse_poc(
    const uint8_t *payload,
    size_t payload_size,
    uint16_t component_count,
    uint16_t layer_count,
    const uint8_t *component_resolution_counts,
    QwJp2PacketProgressionVolume *volumes,
    size_t volume_capacity,
    size_t *out_volume_count)
{
    uint8_t component_width;
    size_t entry_size;
    size_t entry_count;

    if (!out_volume_count || component_count == 0u ||
        component_count > QW_JP2_PACKET_PROGRESSION_MAX_COMPONENTS ||
        (!payload && payload_size != 0u) ||
        (!volumes && volume_capacity != 0u)) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }
    if (!payload || payload_size == 0u) return QW_JP2_PACKET_ERR_FORMAT;

    component_width = component_count < 257u ? 1u : 2u;
    entry_size = 5u + 2u * (size_t)component_width;
    if (payload_size % entry_size != 0u) return QW_JP2_PACKET_ERR_FORMAT;
    entry_count = payload_size / entry_size;
    if (entry_count == 0u) return QW_JP2_PACKET_ERR_FORMAT;
    if (entry_count > (size_t)QW_JP2_PACKET_PROGRESSION_MAX_VOLUMES) {
        return QW_JP2_PACKET_ERR_RESOURCE;
    }

    for (size_t i = 0u; i < entry_count; ++i) {
        QwJp2PacketProgressionVolume volume;
        const uint8_t *entry = payload + i * entry_size;
        qw_jp2_packet_progression_read_volume(entry, component_width,
                                              &volume);
        if (volume.resolution_start > 32u ||
            volume.resolution_end <= volume.resolution_start ||
            volume.resolution_end > 33u ||
            volume.component_start >= component_count ||
            volume.component_end <= volume.component_start ||
            volume.layer_end == 0u ||
            volume.progression_order > QW_JP2_PROGRESSION_CPRL) {
            return QW_JP2_PACKET_ERR_FORMAT;
        }
        if (layer_count != 0u && volume.layer_end > layer_count) {
            volume.layer_end = layer_count;
        }
        if (volume.component_end > component_count) {
            volume.component_end = component_count;
        }
        if (component_resolution_counts) {
            for (uint16_t component = volume.component_start;
                 component < volume.component_end; ++component) {
                uint8_t resolution_count =
                    component_resolution_counts[component];
                if (resolution_count == 0u ||
                    resolution_count >
                        QW_JP2_PACKET_MAX_RESOLUTION_LEVELS) {
                    return QW_JP2_PACKET_ERR_FORMAT;
                }
            }
        }
    }

    if (volumes && volume_capacity < entry_count) {
        return QW_JP2_PACKET_ERR_RESOURCE;
    }
    if (volumes) {
        for (size_t i = 0u; i < entry_count; ++i) {
            qw_jp2_packet_progression_read_volume(
                payload + i * entry_size, component_width, &volumes[i]);
            if (volumes[i].component_end > component_count) {
                volumes[i].component_end = component_count;
            }
            if (layer_count != 0u && volumes[i].layer_end > layer_count) {
                volumes[i].layer_end = layer_count;
            }
        }
    }
    *out_volume_count = entry_count;
    return QW_JP2_PACKET_OK;
}

int qw_jp2_packet_progression_sort(QwJp2PacketOrderEntry *entries,
                                   size_t entry_count,
                                   uint8_t progression_order)
{
    QwJp2PacketOrderSortEntry *sorted;
    size_t allocation_size;

    if (progression_order > QW_JP2_PROGRESSION_CPRL) {
        return QW_JP2_PACKET_ERR_UNSUPPORTED;
    }
    if (!entries && entry_count != 0u) return QW_JP2_PACKET_ERR_ARGUMENT;
    if (entry_count > (size_t)QW_JP2_PACKET_PROGRESSION_MAX_PACKETS) {
        return QW_JP2_PACKET_ERR_RESOURCE;
    }
    if (entry_count < 2u) return QW_JP2_PACKET_OK;
    if (entry_count > SIZE_MAX / sizeof(*sorted)) {
        return QW_JP2_PACKET_ERR_RESOURCE;
    }
    allocation_size = entry_count * sizeof(*sorted);
    sorted = (QwJp2PacketOrderSortEntry *)malloc(allocation_size);
    if (!sorted) return QW_JP2_PACKET_ERR_MEMORY;

    for (size_t i = 0u; i < entry_count; ++i) {
        sorted[i].packet = entries[i];
        sorted[i].original_index = i;
        sorted[i].progression_order = progression_order;
    }
    qsort(sorted, entry_count, sizeof(*sorted), qw_jp2_packet_order_compare);
    for (size_t i = 0u; i < entry_count; ++i) {
        entries[i] = sorted[i].packet;
    }
    free(sorted);
    return QW_JP2_PACKET_OK;
}

static uint64_t qw_jp2_packet_ceil_div_u64(uint64_t numerator,
                                          uint64_t denominator)
{
    return numerator / denominator + (numerator % denominator != 0u);
}

static int qw_jp2_packet_multiply_u64(uint64_t left,
                                      uint64_t right,
                                      uint64_t *out_product)
{
    if (!out_product || (right && left > UINT64_MAX / right)) return 0;
    *out_product = left * right;
    return 1;
}

int qw_jp2_packet_precinct_grid_compute(
    const QwJp2PacketTileGeometry *tile,
    const QwJp2PacketComponentGeometry *component,
    uint8_t resolution_level,
    QwJp2PacketPrecinctGrid *out_grid)
{
    QwJp2PacketPrecinctGrid grid = {0};
    uint64_t tiles_x;
    uint64_t tiles_y;
    uint64_t tile_count;
    uint64_t tile_column;
    uint64_t tile_row;
    uint64_t tile_x0;
    uint64_t tile_y0;
    uint64_t tile_x1;
    uint64_t tile_y1;
    uint64_t component_x0;
    uint64_t component_y0;
    uint64_t component_x1;
    uint64_t component_y1;
    uint64_t resolution_scale;
    uint64_t precinct_width;
    uint64_t precinct_height;
    uint64_t precinct_x1;
    uint64_t precinct_y1;
    uint64_t precincts_wide;
    uint64_t precincts_high;
    uint64_t precinct_count;
    uint64_t reference_x_step;
    uint64_t reference_y_step;
    uint8_t precinct_x_exponent;
    uint8_t precinct_y_exponent;

    if (!tile || !component || !out_grid || !tile->tile_width ||
        !tile->tile_height || !component->x_sampling ||
        !component->y_sampling) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }
    if (component->decomposition_levels > 32u ||
        resolution_level > component->decomposition_levels) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    if (tile->x_size <= tile->x_origin || tile->y_size <= tile->y_origin ||
        tile->tile_x_origin > tile->x_origin ||
        tile->tile_y_origin > tile->y_origin ||
        (uint64_t)tile->tile_x_origin + tile->tile_width <= tile->x_origin ||
        (uint64_t)tile->tile_y_origin + tile->tile_height <= tile->y_origin) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }

    tiles_x = qw_jp2_packet_ceil_div_u64(
        (uint64_t)tile->x_size - tile->tile_x_origin, tile->tile_width);
    tiles_y = qw_jp2_packet_ceil_div_u64(
        (uint64_t)tile->y_size - tile->tile_y_origin, tile->tile_height);
    if (!tiles_x || !tiles_y || tiles_x > UINT32_MAX ||
        tiles_y > UINT32_MAX ||
        !qw_jp2_packet_multiply_u64(tiles_x, tiles_y, &tile_count) ||
        tile_count > UINT32_MAX || tile->tile_index >= tile_count) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }

    tile_column = tile->tile_index % tiles_x;
    tile_row = tile->tile_index / tiles_x;
    tile_x0 = (uint64_t)tile->tile_x_origin +
              tile_column * (uint64_t)tile->tile_width;
    tile_y0 = (uint64_t)tile->tile_y_origin +
              tile_row * (uint64_t)tile->tile_height;
    tile_x1 = tile_x0 + tile->tile_width;
    tile_y1 = tile_y0 + tile->tile_height;
    if (tile_x0 < tile->x_origin) tile_x0 = tile->x_origin;
    if (tile_y0 < tile->y_origin) tile_y0 = tile->y_origin;
    if (tile_x1 > tile->x_size) tile_x1 = tile->x_size;
    if (tile_y1 > tile->y_size) tile_y1 = tile->y_size;
    if (tile_x0 > tile_x1 || tile_y0 > tile_y1) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }

    component_x0 = qw_jp2_packet_ceil_div_u64(
        tile_x0, component->x_sampling);
    component_y0 = qw_jp2_packet_ceil_div_u64(
        tile_y0, component->y_sampling);
    component_x1 = qw_jp2_packet_ceil_div_u64(
        tile_x1, component->x_sampling);
    component_y1 = qw_jp2_packet_ceil_div_u64(
        tile_y1, component->y_sampling);
    resolution_scale = UINT64_C(1) <<
        (component->decomposition_levels - resolution_level);
    grid.tile_component_x0 = (uint32_t)component_x0;
    grid.tile_component_y0 = (uint32_t)component_y0;
    grid.tile_component_x1 = (uint32_t)component_x1;
    grid.tile_component_y1 = (uint32_t)component_y1;
    grid.resolution_x0 = (uint32_t)qw_jp2_packet_ceil_div_u64(
        component_x0, resolution_scale);
    grid.resolution_y0 = (uint32_t)qw_jp2_packet_ceil_div_u64(
        component_y0, resolution_scale);
    grid.resolution_x1 = (uint32_t)qw_jp2_packet_ceil_div_u64(
        component_x1, resolution_scale);
    grid.resolution_y1 = (uint32_t)qw_jp2_packet_ceil_div_u64(
        component_y1, resolution_scale);

    precinct_x_exponent = component->precinct_x_exponents[resolution_level];
    precinct_y_exponent = component->precinct_y_exponents[resolution_level];
    if (precinct_x_exponent > 15u || precinct_y_exponent > 15u ||
        (resolution_level != 0u &&
         (!precinct_x_exponent || !precinct_y_exponent))) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    precinct_width = UINT64_C(1) << precinct_x_exponent;
    precinct_height = UINT64_C(1) << precinct_y_exponent;
    grid.precinct_x0 = (uint32_t)(grid.resolution_x0 / precinct_width);
    grid.precinct_y0 = (uint32_t)(grid.resolution_y0 / precinct_height);
    if (grid.resolution_x0 >= grid.resolution_x1 ||
        grid.resolution_y0 >= grid.resolution_y1) {
        *out_grid = grid;
        return QW_JP2_PACKET_OK;
    }

    precinct_x1 = qw_jp2_packet_ceil_div_u64(
        grid.resolution_x1, precinct_width);
    precinct_y1 = qw_jp2_packet_ceil_div_u64(
        grid.resolution_y1, precinct_height);
    if (precinct_x1 < grid.precinct_x0 ||
        precinct_y1 < grid.precinct_y0) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    precincts_wide = precinct_x1 - grid.precinct_x0;
    precincts_high = precinct_y1 - grid.precinct_y0;
    if (precincts_wide > UINT32_MAX || precincts_high > UINT32_MAX ||
        !qw_jp2_packet_multiply_u64(precincts_wide, precincts_high,
                                    &precinct_count) ||
        precinct_count > QW_JP2_PACKET_PROGRESSION_MAX_PACKETS ||
        !qw_jp2_packet_multiply_u64(component->x_sampling,
                                    resolution_scale,
                                    &reference_x_step) ||
        !qw_jp2_packet_multiply_u64(reference_x_step, precinct_width,
                                    &reference_x_step) ||
        !qw_jp2_packet_multiply_u64(component->y_sampling,
                                    resolution_scale,
                                    &reference_y_step) ||
        !qw_jp2_packet_multiply_u64(reference_y_step, precinct_height,
                                    &reference_y_step)) {
        return QW_JP2_PACKET_ERR_RESOURCE;
    }

    grid.precincts_wide = (uint32_t)precincts_wide;
    grid.precincts_high = (uint32_t)precincts_high;
    grid.precinct_count = (uint32_t)precinct_count;
    grid.reference_x_step = reference_x_step;
    grid.reference_y_step = reference_y_step;
    *out_grid = grid;
    return QW_JP2_PACKET_OK;
}

static uint64_t qw_jp2_packet_ceil_subband_coordinate(uint64_t coordinate,
                                                       uint64_t phase,
                                                       uint64_t scale)
{
    if (coordinate <= phase) return 0u;
    return qw_jp2_packet_ceil_div_u64(coordinate - phase, scale);
}

static uint8_t qw_jp2_packet_min_u8(uint8_t left, uint8_t right)
{
    return left < right ? left : right;
}

int qw_jp2_packet_precinct_codeblock_grid_compute(
    const QwJp2PacketTileGeometry *tile,
    const QwJp2PacketComponentGeometry *component,
    const QwJp2PacketOrderEntry *packet,
    uint8_t subband_orientation,
    QwJp2PacketPrecinctCodeBlockGrid *out_grid)
{
    QwJp2PacketPrecinctGrid precinct_grid;
    QwJp2PacketPrecinctCodeBlockGrid result = {0};
    uint64_t precinct_x;
    uint64_t precinct_y;
    uint64_t precinct_x0;
    uint64_t precinct_y0;
    uint64_t precinct_x1;
    uint64_t precinct_y1;
    uint64_t band_x0;
    uint64_t band_y0;
    uint64_t band_x1;
    uint64_t band_y1;
    uint64_t precinct_band_x0;
    uint64_t precinct_band_y0;
    uint64_t precinct_band_x1;
    uint64_t precinct_band_y1;
    uint64_t codeblock_width;
    uint64_t codeblock_height;
    uint64_t codeblock_x1;
    uint64_t codeblock_y1;
    uint64_t codeblock_count;
    uint64_t reference_x;
    uint64_t reference_y;
    uint8_t precinct_x_exponent;
    uint8_t precinct_y_exponent;
    uint8_t codeblock_width_exponent;
    uint8_t codeblock_height_exponent;
    int geometry_rc;

    if (!tile || !component || !packet || !out_grid) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }
    if (subband_orientation > QW_JP2_SUBBAND_HH ||
        (packet->resolution_level == 0u &&
         subband_orientation != QW_JP2_SUBBAND_LL) ||
        (packet->resolution_level != 0u &&
         subband_orientation == QW_JP2_SUBBAND_LL) ||
        component->codeblock_width_exponent > 8u ||
        component->codeblock_height_exponent > 8u ||
        (unsigned)component->codeblock_width_exponent +
                (unsigned)component->codeblock_height_exponent + 4u >
            12u) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }

    geometry_rc = qw_jp2_packet_precinct_grid_compute(
        tile, component, packet->resolution_level, &precinct_grid);
    if (geometry_rc != QW_JP2_PACKET_OK) return geometry_rc;
    if (packet->precinct_index >= precinct_grid.precinct_count ||
        !precinct_grid.precincts_wide) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    precinct_x = packet->precinct_index % precinct_grid.precincts_wide;
    precinct_y = packet->precinct_index / precinct_grid.precincts_wide;
    if (!qw_jp2_packet_multiply_u64(
            (uint64_t)precinct_grid.precinct_x0 + precinct_x,
            precinct_grid.reference_x_step, &reference_x) ||
        !qw_jp2_packet_multiply_u64(
            (uint64_t)precinct_grid.precinct_y0 + precinct_y,
            precinct_grid.reference_y_step, &reference_y)) {
        return QW_JP2_PACKET_ERR_RESOURCE;
    }
    if (reference_x != packet->reference_x ||
        reference_y != packet->reference_y) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }

    precinct_x += precinct_grid.precinct_x0;
    precinct_y += precinct_grid.precinct_y0;
    precinct_x_exponent =
        component->precinct_x_exponents[packet->resolution_level];
    precinct_y_exponent =
        component->precinct_y_exponents[packet->resolution_level];
    precinct_x0 = precinct_x << precinct_x_exponent;
    precinct_y0 = precinct_y << precinct_y_exponent;
    precinct_x1 = precinct_x0 + (UINT64_C(1) << precinct_x_exponent);
    precinct_y1 = precinct_y0 + (UINT64_C(1) << precinct_y_exponent);

    if (packet->resolution_level == 0u) {
        band_x0 = precinct_grid.resolution_x0;
        band_y0 = precinct_grid.resolution_y0;
        band_x1 = precinct_grid.resolution_x1;
        band_y1 = precinct_grid.resolution_y1;
        precinct_band_x0 = precinct_x0;
        precinct_band_y0 = precinct_y0;
        precinct_band_x1 = precinct_x1;
        precinct_band_y1 = precinct_y1;
        codeblock_width_exponent = qw_jp2_packet_min_u8(
            (uint8_t)(component->codeblock_width_exponent + 2u),
            precinct_x_exponent);
        codeblock_height_exponent = qw_jp2_packet_min_u8(
            (uint8_t)(component->codeblock_height_exponent + 2u),
            precinct_y_exponent);
    } else {
        uint8_t decomposition_level = (uint8_t)(
            component->decomposition_levels - packet->resolution_level + 1u);
        uint64_t subband_scale = UINT64_C(1) << decomposition_level;
        uint64_t subband_phase_x =
            (subband_orientation & 0x01u) ? subband_scale >> 1u : 0u;
        uint64_t subband_phase_y =
            (subband_orientation & 0x02u) ? subband_scale >> 1u : 0u;
        uint64_t precinct_phase_x =
            (subband_orientation & 0x01u) ? 1u : 0u;
        uint64_t precinct_phase_y =
            (subband_orientation & 0x02u) ? 1u : 0u;

        band_x0 = qw_jp2_packet_ceil_subband_coordinate(
            precinct_grid.tile_component_x0, subband_phase_x, subband_scale);
        band_y0 = qw_jp2_packet_ceil_subband_coordinate(
            precinct_grid.tile_component_y0, subband_phase_y, subband_scale);
        band_x1 = qw_jp2_packet_ceil_subband_coordinate(
            precinct_grid.tile_component_x1, subband_phase_x, subband_scale);
        band_y1 = qw_jp2_packet_ceil_subband_coordinate(
            precinct_grid.tile_component_y1, subband_phase_y, subband_scale);
        precinct_band_x0 = qw_jp2_packet_ceil_subband_coordinate(
            precinct_x0, precinct_phase_x, 2u);
        precinct_band_y0 = qw_jp2_packet_ceil_subband_coordinate(
            precinct_y0, precinct_phase_y, 2u);
        precinct_band_x1 = qw_jp2_packet_ceil_subband_coordinate(
            precinct_x1, precinct_phase_x, 2u);
        precinct_band_y1 = qw_jp2_packet_ceil_subband_coordinate(
            precinct_y1, precinct_phase_y, 2u);
        codeblock_width_exponent = qw_jp2_packet_min_u8(
            (uint8_t)(component->codeblock_width_exponent + 2u),
            (uint8_t)(precinct_x_exponent - 1u));
        codeblock_height_exponent = qw_jp2_packet_min_u8(
            (uint8_t)(component->codeblock_height_exponent + 2u),
            (uint8_t)(precinct_y_exponent - 1u));
    }

    if (precinct_band_x1 <= band_x0 || precinct_band_x0 >= band_x1) {
        uint64_t empty_x = precinct_band_x0 < band_x0
                               ? band_x0
                               : precinct_band_x0 > band_x1
                                     ? band_x1
                                     : precinct_band_x0;
        precinct_band_x0 = empty_x;
        precinct_band_x1 = empty_x;
    } else {
        if (precinct_band_x0 < band_x0) precinct_band_x0 = band_x0;
        if (precinct_band_x1 > band_x1) precinct_band_x1 = band_x1;
    }
    if (precinct_band_y1 <= band_y0 || precinct_band_y0 >= band_y1) {
        uint64_t empty_y = precinct_band_y0 < band_y0
                               ? band_y0
                               : precinct_band_y0 > band_y1
                                     ? band_y1
                                     : precinct_band_y0;
        precinct_band_y0 = empty_y;
        precinct_band_y1 = empty_y;
    } else {
        if (precinct_band_y0 < band_y0) precinct_band_y0 = band_y0;
        if (precinct_band_y1 > band_y1) precinct_band_y1 = band_y1;
    }

    codeblock_width = UINT64_C(1) << codeblock_width_exponent;
    codeblock_height = UINT64_C(1) << codeblock_height_exponent;
    result.subband_x0 = (uint32_t)precinct_band_x0;
    result.subband_y0 = (uint32_t)precinct_band_y0;
    result.subband_x1 = (uint32_t)precinct_band_x1;
    result.subband_y1 = (uint32_t)precinct_band_y1;
    result.codeblock_x0 = (uint32_t)(precinct_band_x0 / codeblock_width);
    result.codeblock_y0 = (uint32_t)(precinct_band_y0 / codeblock_height);
    codeblock_x1 = precinct_band_x1 > precinct_band_x0
        ? qw_jp2_packet_ceil_div_u64(precinct_band_x1, codeblock_width)
        : result.codeblock_x0;
    codeblock_y1 = precinct_band_y1 > precinct_band_y0
        ? qw_jp2_packet_ceil_div_u64(precinct_band_y1, codeblock_height)
        : result.codeblock_y0;
    if (codeblock_x1 < result.codeblock_x0 ||
        codeblock_y1 < result.codeblock_y0) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    result.codeblocks_wide =
        (uint32_t)(codeblock_x1 - result.codeblock_x0);
    result.codeblocks_high =
        (uint32_t)(codeblock_y1 - result.codeblock_y0);
    if (!qw_jp2_packet_multiply_u64(result.codeblocks_wide,
                                    result.codeblocks_high,
                                    &codeblock_count) ||
        codeblock_count > QW_JP2_PACKET_MAX_TAG_TREE_NODES) {
        return QW_JP2_PACKET_ERR_RESOURCE;
    }
    result.codeblock_count = (uint32_t)codeblock_count;
    result.codeblock_width_exponent = codeblock_width_exponent;
    result.codeblock_height_exponent = codeblock_height_exponent;
    *out_grid = result;
    return QW_JP2_PACKET_OK;
}

static uint64_t qw_jp2_packet_key(uint32_t layer,
                                  uint16_t component,
                                  uint32_t resolution,
                                  uint32_t precinct)
{
    return ((uint64_t)layer << 48u) |
           ((uint64_t)component << 34u) |
           ((uint64_t)resolution << 28u) |
           (uint64_t)precinct;
}

static size_t qw_jp2_packet_hash_slot(uint64_t key, size_t mask)
{
    key ^= key >> 30u;
    key *= UINT64_C(0xbf58476d1ce4e5b9);
    key ^= key >> 27u;
    key *= UINT64_C(0x94d049bb133111eb);
    key ^= key >> 31u;
    return (size_t)key & mask;
}

static int qw_jp2_packet_seen_or_insert(uint64_t *table,
                                        size_t table_mask,
                                        uint64_t key,
                                        int *out_was_seen)
{
    size_t slot;
    uint64_t stored_key = key + 1u;

    if (!table || !out_was_seen || stored_key == 0u) return 0;
    slot = qw_jp2_packet_hash_slot(key, table_mask);
    for (size_t probes = 0u; probes <= table_mask; ++probes) {
        if (table[slot] == 0u) {
            table[slot] = stored_key;
            *out_was_seen = 0;
            return 1;
        }
        if (table[slot] == stored_key) {
            *out_was_seen = 1;
            return 1;
        }
        slot = (slot + 1u) & table_mask;
    }
    return 0;
}

int qw_jp2_packet_progression_expand_volumes(
    const QwJp2PacketTileGeometry *tile,
    const QwJp2PacketComponentGeometry *components,
    uint16_t component_count,
    uint16_t layer_count,
    uint8_t default_progression_order,
    const QwJp2PacketProgressionVolume *volumes,
    size_t volume_count,
    QwJp2PacketOrderEntry *entries,
    size_t entry_capacity,
    size_t *out_entry_count)
{
    enum { QW_JP2_PACKET_VISITED_TABLE_CAPACITY = 524288u,
           QW_JP2_PACKET_PROGRESSION_MAX_CELL_VISITS = 1048576u,
           QW_JP2_PACKET_PROGRESSION_MAX_PACKET_VISITS = 1048576u };
    QwJp2PacketProgressionVolume default_volume;
    const QwJp2PacketProgressionVolume *active_volumes = volumes;
    size_t active_volume_count = volume_count;
    QwJp2PacketOrderEntry *expanded = NULL;
    uint64_t *visited = NULL;
    size_t expanded_count = 0u;
    size_t cell_visits = 0u;
    size_t packet_visits = 0u;
    int rc = QW_JP2_PACKET_ERR_RESOURCE;

    if (!tile || !components || !component_count ||
        component_count > QW_JP2_PACKET_PROGRESSION_MAX_COMPONENTS ||
        !layer_count || (!volumes && volume_count) ||
        (!entries && entry_capacity) || !out_entry_count) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }
    if (volume_count > QW_JP2_PACKET_PROGRESSION_MAX_VOLUMES) {
        return QW_JP2_PACKET_ERR_RESOURCE;
    }
    if (default_progression_order > QW_JP2_PROGRESSION_CPRL) {
        return QW_JP2_PACKET_ERR_UNSUPPORTED;
    }
    if (active_volume_count == 0u) {
        default_volume.component_start = 0u;
        default_volume.component_end = component_count;
        default_volume.layer_end = layer_count;
        default_volume.resolution_start = 0u;
        default_volume.resolution_end =
            QW_JP2_PACKET_MAX_RESOLUTION_LEVELS;
        default_volume.progression_order = default_progression_order;
        active_volumes = &default_volume;
        active_volume_count = 1u;
    }

    expanded = (QwJp2PacketOrderEntry *)malloc(
        (size_t)QW_JP2_PACKET_PROGRESSION_MAX_PACKETS * sizeof(*expanded));
    visited = (uint64_t *)calloc(QW_JP2_PACKET_VISITED_TABLE_CAPACITY,
                                 sizeof(*visited));
    if (!expanded || !visited) {
        rc = QW_JP2_PACKET_ERR_MEMORY;
        goto cleanup;
    }

    for (size_t volume_index = 0u; volume_index < active_volume_count;
         ++volume_index) {
        const QwJp2PacketProgressionVolume *volume =
            &active_volumes[volume_index];
        size_t volume_start = expanded_count;

        if (volume->component_start >= volume->component_end ||
            volume->component_end > component_count ||
            volume->resolution_start >= volume->resolution_end ||
            volume->resolution_end >
                QW_JP2_PACKET_MAX_RESOLUTION_LEVELS ||
            volume->layer_end == 0u || volume->layer_end > layer_count ||
            volume->progression_order > QW_JP2_PROGRESSION_CPRL) {
            rc = QW_JP2_PACKET_ERR_FORMAT;
            goto cleanup;
        }

        for (uint32_t component_index = volume->component_start;
             component_index < volume->component_end; ++component_index) {
            const QwJp2PacketComponentGeometry *component =
                &components[component_index];
            uint32_t resolution_count;
            uint32_t resolution_start = volume->resolution_start;
            uint32_t resolution_end = volume->resolution_end;

            if (component->decomposition_levels > 32u) {
                rc = QW_JP2_PACKET_ERR_FORMAT;
                goto cleanup;
            }
            resolution_count =
                (uint32_t)component->decomposition_levels + 1u;
            if (resolution_start >= resolution_count) continue;
            if (resolution_end > resolution_count)
                resolution_end = resolution_count;

            for (uint32_t resolution = resolution_start;
                 resolution < resolution_end; ++resolution) {
                QwJp2PacketPrecinctGrid grid;
                int geometry_rc;

                if (++cell_visits >
                    QW_JP2_PACKET_PROGRESSION_MAX_CELL_VISITS) {
                    rc = QW_JP2_PACKET_ERR_RESOURCE;
                    goto cleanup;
                }
                geometry_rc = qw_jp2_packet_precinct_grid_compute(
                    tile, component, (uint8_t)resolution, &grid);
                if (geometry_rc != QW_JP2_PACKET_OK) {
                    rc = geometry_rc;
                    goto cleanup;
                }
                if (grid.precinct_count &&
                    volume->layer_end >
                        (QW_JP2_PACKET_PROGRESSION_MAX_PACKET_VISITS -
                         packet_visits) / grid.precinct_count) {
                    rc = QW_JP2_PACKET_ERR_RESOURCE;
                    goto cleanup;
                }
                packet_visits +=
                    (size_t)volume->layer_end * grid.precinct_count;

                for (uint32_t layer = 0u; layer < volume->layer_end;
                     ++layer) {
                    for (uint32_t precinct = 0u;
                         precinct < grid.precinct_count; ++precinct) {
                        uint64_t key = qw_jp2_packet_key(
                            layer, (uint16_t)component_index, resolution,
                            precinct);
                        int was_seen;
                        uint32_t precinct_x;
                        uint32_t precinct_y;
                        uint64_t reference_x;
                        uint64_t reference_y;
                        if (!qw_jp2_packet_seen_or_insert(
                                visited,
                                QW_JP2_PACKET_VISITED_TABLE_CAPACITY - 1u,
                                key, &was_seen)) {
                            rc = QW_JP2_PACKET_ERR_RESOURCE;
                            goto cleanup;
                        }
                        if (was_seen) continue;
                        if (expanded_count >=
                            QW_JP2_PACKET_PROGRESSION_MAX_PACKETS) {
                            rc = QW_JP2_PACKET_ERR_RESOURCE;
                            goto cleanup;
                        }
                        precinct_x = grid.precinct_x0 +
                            precinct % grid.precincts_wide;
                        precinct_y = grid.precinct_y0 +
                            precinct / grid.precincts_wide;
                        if (!qw_jp2_packet_multiply_u64(
                                precinct_x, grid.reference_x_step,
                                &reference_x) ||
                            !qw_jp2_packet_multiply_u64(
                                precinct_y, grid.reference_y_step,
                                &reference_y)) {
                            rc = QW_JP2_PACKET_ERR_RESOURCE;
                            goto cleanup;
                        }
                        expanded[expanded_count].layer_index =
                            (uint16_t)layer;
                        expanded[expanded_count].component_index =
                            (uint16_t)component_index;
                        expanded[expanded_count].precinct_index = precinct;
                        expanded[expanded_count].reference_x = reference_x;
                        expanded[expanded_count].reference_y = reference_y;
                        expanded[expanded_count].resolution_level =
                            (uint8_t)resolution;
                        ++expanded_count;
                    }
                }
            }
        }

        rc = qw_jp2_packet_progression_sort(
            expanded + volume_start, expanded_count - volume_start,
            volume->progression_order);
        if (rc != QW_JP2_PACKET_OK) goto cleanup;
    }

    if (entries && expanded_count > entry_capacity) {
        rc = QW_JP2_PACKET_ERR_RESOURCE;
        goto cleanup;
    }
    if (entries && expanded_count) {
        memcpy(entries, expanded, expanded_count * sizeof(*entries));
    }
    *out_entry_count = expanded_count;
    rc = QW_JP2_PACKET_OK;

cleanup:
    free(visited);
    free(expanded);
    return rc;
}
