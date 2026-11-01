#ifndef QUIREWISE_JP2_PACKET_PROGRESSION_H
#define QUIREWISE_JP2_PACKET_PROGRESSION_H

#include "jp2_packet_header.h"

enum {
    QW_JP2_PACKET_PROGRESSION_MAX_PACKETS = 262144u,
    QW_JP2_PACKET_PROGRESSION_MAX_VOLUMES = 9361u,
    QW_JP2_PACKET_PROGRESSION_MAX_COMPONENTS = 16384u,
    QW_JP2_PACKET_MAX_RESOLUTION_LEVELS = 33u,
    QW_JP2_PROGRESSION_LRCP = 0u,
    QW_JP2_PROGRESSION_RLCP = 1u,
    QW_JP2_PROGRESSION_RPCL = 2u,
    QW_JP2_PROGRESSION_PCRL = 3u,
    QW_JP2_PROGRESSION_CPRL = 4u
};

typedef struct QwJp2PacketOrderEntry {
    uint16_t layer_index;
    uint16_t component_index;
    uint32_t precinct_index;
    uint64_t reference_x;
    uint64_t reference_y;
    uint8_t resolution_level;
} QwJp2PacketOrderEntry;

typedef struct QwJp2PacketProgressionVolume {
    uint16_t component_start;
    uint16_t component_end;
    uint16_t layer_end;
    uint8_t resolution_start;
    uint8_t resolution_end;
    uint8_t progression_order;
} QwJp2PacketProgressionVolume;

/* SIZ coordinates for one raster-indexed tile on the reference grid. */
typedef struct QwJp2PacketTileGeometry {
    uint32_t x_size;
    uint32_t y_size;
    uint32_t x_origin;
    uint32_t y_origin;
    uint32_t tile_width;
    uint32_t tile_height;
    uint32_t tile_x_origin;
    uint32_t tile_y_origin;
    uint32_t tile_index;
} QwJp2PacketTileGeometry;

/* COD defaults or a component-specific COC override for one tile-component.
 * Fill absent COD/COC precinct parameters with exponent 15 at every level;
 * exponent zero is permitted only at resolution level zero. */
typedef struct QwJp2PacketComponentGeometry {
    uint8_t x_sampling;
    uint8_t y_sampling;
    uint8_t decomposition_levels;
    uint8_t wavelet_transform;
    /* COD/COC size parameters; the corresponding actual exponents add 2. */
    uint8_t codeblock_width_exponent;
    uint8_t codeblock_height_exponent;
    uint8_t codeblock_style;
    uint8_t precinct_x_exponents[QW_JP2_PACKET_MAX_RESOLUTION_LEVELS];
    uint8_t precinct_y_exponents[QW_JP2_PACKET_MAX_RESOLUTION_LEVELS];
} QwJp2PacketComponentGeometry;

typedef struct QwJp2PacketPrecinctGrid {
    uint32_t tile_component_x0;
    uint32_t tile_component_y0;
    uint32_t tile_component_x1;
    uint32_t tile_component_y1;
    uint32_t resolution_x0;
    uint32_t resolution_y0;
    uint32_t resolution_x1;
    uint32_t resolution_y1;
    uint32_t precinct_x0;
    uint32_t precinct_y0;
    uint32_t precincts_wide;
    uint32_t precincts_high;
    uint32_t precinct_count;
    uint64_t reference_x_step;
    uint64_t reference_y_step;
} QwJp2PacketPrecinctGrid;

enum {
    QW_JP2_SUBBAND_LL = 0u,
    QW_JP2_SUBBAND_HL = 1u,
    QW_JP2_SUBBAND_LH = 2u,
    QW_JP2_SUBBAND_HH = 3u
};

/* Code-block partition intersecting one packet's precinct in one sub-band.
 * Coordinates and code-block x/y indices are in the sub-band's origin-
 * anchored partition. Output size exponents are the effective actual powers. */
typedef struct QwJp2PacketPrecinctCodeBlockGrid {
    uint32_t subband_x0;
    uint32_t subband_y0;
    uint32_t subband_x1;
    uint32_t subband_y1;
    uint32_t codeblock_x0;
    uint32_t codeblock_y0;
    uint32_t codeblocks_wide;
    uint32_t codeblocks_high;
    uint32_t codeblock_count;
    uint8_t codeblock_width_exponent;
    uint8_t codeblock_height_exponent;
} QwJp2PacketPrecinctCodeBlockGrid;

/*
 * Sorts the packets for one tile and one progression volume in place.
 * reference_x/y are the precinct origin mapped to the reference grid.
 * Failure leaves the caller's array unchanged. Ties preserve input order.
 */
int qw_jp2_packet_progression_sort(QwJp2PacketOrderEntry *entries,
                                   size_t entry_count,
                                   uint8_t progression_order);

/* Derives T.800 B.3, B.5, and B.6 tile-component precinct geometry. Precinct
 * grids are anchored at reference-grid coordinate (0, 0); empty precincts
 * within the bounded tile-component grid remain represented. Output changes
 * only on success. */
int qw_jp2_packet_precinct_grid_compute(
    const QwJp2PacketTileGeometry *tile,
    const QwJp2PacketComponentGeometry *component,
    uint8_t resolution_level,
    QwJp2PacketPrecinctGrid *out_grid);

/* Maps an expanded packet key to its precinct-local code-block grid for one
 * sub-band. Resolution zero accepts LL; higher levels accept HL, LH, or HH.
 * Packet reference coordinates are checked against the derived precinct. */
int qw_jp2_packet_precinct_codeblock_grid_compute(
    const QwJp2PacketTileGeometry *tile,
    const QwJp2PacketComponentGeometry *component,
    const QwJp2PacketOrderEntry *packet,
    uint8_t subband_orientation,
    QwJp2PacketPrecinctCodeBlockGrid *out_grid);

/* Expands POC progression volumes into ordered packet keys for one tile.
 * Each volume's layer range starts at zero; keys already emitted by earlier
 * volumes are skipped as required by T.800 B.12.2. With no explicit volumes,
 * the COD default progression covers all components, resolutions, and layers.
 * Passing NULL/zero for entries queries the required packet count. Failure
 * leaves entries and out_entry_count unchanged. */
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
    size_t *out_entry_count);

/* Parses the POC payload (the bytes after Lpoc). A layer_count of zero skips
 * the external layer bound. component_resolution_counts, when supplied, has
 * one valid resolution count per component; per-component progression ranges
 * are clipped to the available levels during expansion. Passing NULL/zero for
 * volumes validates only. The payload and output array must not overlap.
 * Outputs change only on success. */
int qw_jp2_packet_progression_parse_poc(
    const uint8_t *payload,
    size_t payload_size,
    uint16_t component_count,
    uint16_t layer_count,
    const uint8_t *component_resolution_counts,
    QwJp2PacketProgressionVolume *volumes,
    size_t volume_capacity,
    size_t *out_volume_count);

#endif
