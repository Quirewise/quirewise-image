#include "jp2_packet_progression.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int packet_id(const QwJp2PacketOrderEntry *entry)
{
    return (int)entry->layer_index * 8 +
           (int)entry->component_index * 4 +
           (int)entry->resolution_level * 2 +
           (int)entry->precinct_index;
}

static int test_progression_orders(void)
{
    static const uint8_t expected[5][16] = {
        {0u, 1u, 4u, 5u, 2u, 3u, 6u, 7u,
         8u, 9u, 12u, 13u, 10u, 11u, 14u, 15u},
        {0u, 1u, 4u, 5u, 8u, 9u, 12u, 13u,
         2u, 3u, 6u, 7u, 10u, 11u, 14u, 15u},
        {1u, 9u, 5u, 13u, 0u, 8u, 4u, 12u,
         3u, 11u, 7u, 15u, 2u, 10u, 6u, 14u},
        {1u, 9u, 3u, 11u, 5u, 13u, 7u, 15u,
         0u, 8u, 2u, 10u, 4u, 12u, 6u, 14u},
        {1u, 9u, 3u, 11u, 0u, 8u, 2u, 10u,
         5u, 13u, 7u, 15u, 4u, 12u, 6u, 14u}
    };

    for (uint8_t order = 0u; order < 5u; ++order) {
        QwJp2PacketOrderEntry entries[16];
        for (size_t i = 0u; i < 16u; ++i) {
            uint32_t id = (uint32_t)(15u - i);
            entries[i].layer_index = (uint16_t)(id / 8u);
            entries[i].component_index = (uint16_t)((id / 4u) % 2u);
            entries[i].resolution_level = (uint8_t)((id / 2u) % 2u);
            entries[i].precinct_index = id % 2u;
            entries[i].reference_x = entries[i].precinct_index ? 8u : 0u;
            entries[i].reference_y = entries[i].precinct_index ? 0u : 8u;
        }
        if (qw_jp2_packet_progression_sort(entries, 16u, order) !=
            QW_JP2_PACKET_OK) {
            fputs("packet progression sort returned an error\n", stderr);
            return 0;
        }
        for (size_t i = 0u; i < 16u; ++i) {
            if (packet_id(&entries[i]) != (int)expected[order][i]) {
                fputs("packet progression order mismatch\n", stderr);
                return 0;
            }
        }
    }
    return 1;
}

static int test_arguments_and_resource_limit(void)
{
    QwJp2PacketOrderEntry entry = {3u, 2u, 1u, 4u, 5u, 1u};
    QwJp2PacketOrderEntry original = entry;

    if (qw_jp2_packet_progression_sort(NULL, 0u,
                                       QW_JP2_PROGRESSION_LRCP) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_progression_sort(NULL, 1u,
                                       QW_JP2_PROGRESSION_LRCP) !=
            QW_JP2_PACKET_ERR_ARGUMENT ||
        qw_jp2_packet_progression_sort(
            &entry, 1u, (uint8_t)(QW_JP2_PROGRESSION_CPRL + 1u)) !=
            QW_JP2_PACKET_ERR_UNSUPPORTED ||
        memcmp(&entry, &original, sizeof(entry)) != 0) {
        fputs("packet progression argument handling mismatch\n", stderr);
        return 0;
    }
    if (qw_jp2_packet_progression_sort(
            &entry,
            (size_t)QW_JP2_PACKET_PROGRESSION_MAX_PACKETS + 1u,
            QW_JP2_PROGRESSION_LRCP) != QW_JP2_PACKET_ERR_RESOURCE ||
        memcmp(&entry, &original, sizeof(entry)) != 0) {
        fputs("packet progression resource limit was not transactional\n",
              stderr);
        return 0;
    }
    return 1;
}

static int test_poc_payloads(void)
{
    static const uint8_t payload[] = {
        0u, 0u, 0u, 1u, 2u, 3u, QW_JP2_PROGRESSION_LRCP,
        1u, 1u, 0u, 2u, 2u, 3u, QW_JP2_PROGRESSION_CPRL
    };
    static const uint8_t component_256_payload[] = {
        0u, 255u, 0u, 1u, 1u, 0u, QW_JP2_PROGRESSION_RPCL
    };
    static const uint8_t component_257_payload[] = {
        0u, 1u, 0u, 0u, 1u, 1u, 1u, 1u,
        QW_JP2_PROGRESSION_CPRL
    };
    static const uint8_t component_257_zero_end_payload[] = {
        0u, 0u, 0u, 0u, 1u, 1u, 0u, 0u,
        QW_JP2_PROGRESSION_RPCL
    };
    static const uint8_t single_component_large_end_payload[] = {
        0u, 0u, 0u, 1u, 1u, 255u, QW_JP2_PROGRESSION_LRCP
    };
    static const uint8_t layer_end_above_count_payload[] = {
        0u, 0u, 0u, 9u, 1u, 1u, QW_JP2_PROGRESSION_LRCP
    };
    static const uint8_t zero_layer_end_payload[] = {
        0u, 0u, 0u, 0u, 1u, 1u, QW_JP2_PROGRESSION_LRCP
    };
    static const uint8_t component_16384_payload[] = {
        0u, 0u, 0u, 0u, 1u, 1u, 0u, 0u,
        QW_JP2_PROGRESSION_RPCL
    };
    static const uint8_t component_resolutions[3] = {2u, 2u, 2u};
    static const uint8_t clipped_component_resolutions[3] = {1u, 2u, 2u};
    static const uint8_t invalid_component_resolutions[3] = {0u, 2u, 2u};
    static const uint8_t single_component_resolutions[1] = {1u};
    QwJp2PacketProgressionVolume volumes[2] = {{0}};
    QwJp2PacketProgressionVolume untouched = {0};
    uint8_t invalid[sizeof(payload)];
    size_t count = 77u;

    if (qw_jp2_packet_progression_parse_poc(
            payload, sizeof(payload), 3u, 2u, component_resolutions,
            volumes, 2u, &count) !=
            QW_JP2_PACKET_OK || count != 2u ||
        volumes[0].resolution_start != 0u ||
        volumes[0].resolution_end != 2u ||
        volumes[0].component_start != 0u ||
        volumes[0].component_end != 3u || volumes[0].layer_end != 1u ||
        volumes[0].progression_order != QW_JP2_PROGRESSION_LRCP ||
        volumes[1].resolution_start != 1u ||
        volumes[1].resolution_end != 2u ||
        volumes[1].component_start != 1u ||
        volumes[1].component_end != 3u || volumes[1].layer_end != 2u ||
        volumes[1].progression_order != QW_JP2_PROGRESSION_CPRL) {
        fputs("POC payload fields were not parsed correctly\n", stderr);
        return 0;
    }

    if (qw_jp2_packet_progression_parse_poc(
            NULL, 0u, 3u, 2u, NULL, NULL, 0u, &count) !=
            QW_JP2_PACKET_ERR_FORMAT ||
        qw_jp2_packet_progression_parse_poc(
            payload, sizeof(payload) - 1u, 3u, 2u, NULL, NULL, 0u,
            &count) !=
            QW_JP2_PACKET_ERR_FORMAT) {
        fputs("POC payload boundary checks accepted malformed input\n", stderr);
        return 0;
    }

    memcpy(invalid, payload, sizeof(payload));
    invalid[0] = 33u;
    if (qw_jp2_packet_progression_parse_poc(
            invalid, sizeof(invalid), 3u, 2u, NULL, NULL, 0u, &count) !=
        QW_JP2_PACKET_ERR_FORMAT) {
        fputs("POC resolution start bound was not enforced\n", stderr);
        return 0;
    }
    memcpy(invalid, payload, sizeof(payload));
    invalid[6] = 5u;
    if (qw_jp2_packet_progression_parse_poc(
            invalid, sizeof(invalid), 3u, 2u, NULL, NULL, 0u, &count) !=
        QW_JP2_PACKET_ERR_FORMAT) {
        fputs("POC progression order bound was not enforced\n", stderr);
        return 0;
    }

    if (qw_jp2_packet_progression_parse_poc(
            component_256_payload, sizeof(component_256_payload),
            256u, 1u, NULL, volumes, 2u, &count) != QW_JP2_PACKET_OK ||
        count != 1u || volumes[0].component_start != 255u ||
        volumes[0].component_end != 256u) {
        fputs("POC 256-component end sentinel was not decoded\n", stderr);
        return 0;
    }
    if (qw_jp2_packet_progression_parse_poc(
            single_component_large_end_payload,
            sizeof(single_component_large_end_payload), 1u, 1u,
            clipped_component_resolutions, volumes, 2u, &count) !=
            QW_JP2_PACKET_OK || count != 1u ||
        volumes[0].component_start != 0u ||
        volumes[0].component_end != 1u) {
        fputs("POC CEpoc maximum was not clipped to the component count\n",
              stderr);
        return 0;
    }
    if (qw_jp2_packet_progression_parse_poc(
            layer_end_above_count_payload,
            sizeof(layer_end_above_count_payload), 1u, 8u,
            single_component_resolutions, volumes, 2u, &count) !=
            QW_JP2_PACKET_OK || count != 1u || volumes[0].layer_end != 8u) {
        fputs("POC LYEpoc maximum was not clipped to the layer count\n",
              stderr);
        return 0;
    }
    if (qw_jp2_packet_progression_parse_poc(
            zero_layer_end_payload, sizeof(zero_layer_end_payload), 1u, 8u,
            single_component_resolutions, volumes, 2u, &count) !=
        QW_JP2_PACKET_ERR_FORMAT) {
        fputs("POC zero LYEpoc was not rejected\n", stderr);
        return 0;
    }
    if (qw_jp2_packet_progression_parse_poc(
            component_257_payload, sizeof(component_257_payload),
            257u, 1u, NULL, volumes, 2u, &count) != QW_JP2_PACKET_OK ||
        count != 1u || volumes[0].component_start != 256u ||
        volumes[0].component_end != 257u) {
        fputs("POC 16-bit component indices were not decoded\n", stderr);
        return 0;
    }

    if (qw_jp2_packet_progression_parse_poc(
            component_257_zero_end_payload,
            sizeof(component_257_zero_end_payload), 257u, 1u,
            NULL, volumes, 2u, &count) != QW_JP2_PACKET_OK ||
        count != 1u || volumes[0].component_end != 257u) {
        fputs("16-bit CEpoc zero sentinel was not clipped to Csiz\n", stderr);
        return 0;
    }
    if (qw_jp2_packet_progression_parse_poc(
            component_16384_payload, sizeof(component_16384_payload),
            16384u, 1u, NULL, volumes, 2u, &count) != QW_JP2_PACKET_OK ||
        count != 1u || volumes[0].component_start != 0u ||
        volumes[0].component_end != 16384u) {
        fputs("16-bit CEpoc zero sentinel did not mean 16384\n", stderr);
        return 0;
    }

    if (qw_jp2_packet_progression_parse_poc(
            payload, sizeof(payload), 3u, 2u,
            clipped_component_resolutions, NULL, 0u, &count) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_progression_parse_poc(
            payload, sizeof(payload), 3u, 2u,
            invalid_component_resolutions, NULL, 0u, &count) !=
            QW_JP2_PACKET_ERR_FORMAT) {
        fputs("POC resolution ranges were not clipped per component\n",
              stderr);
        return 0;
    }

    memset(&volumes[0], 0x5a, sizeof(volumes[0]));
    untouched = volumes[0];
    count = 77u;
    if (qw_jp2_packet_progression_parse_poc(
            payload, sizeof(payload), 3u, 2u, component_resolutions,
            volumes, 1u, &count) !=
            QW_JP2_PACKET_ERR_RESOURCE || count != 77u ||
        memcmp(&volumes[0], &untouched, sizeof(untouched)) != 0) {
        fputs("POC output capacity failure was not transactional\n", stderr);
        return 0;
    }
    return 1;
}

static int test_precinct_geometry(void)
{
    QwJp2PacketTileGeometry tile = {
        .x_size = 16u,
        .y_size = 8u,
        .x_origin = 0u,
        .y_origin = 0u,
        .tile_width = 16u,
        .tile_height = 8u,
        .tile_x_origin = 0u,
        .tile_y_origin = 0u,
        .tile_index = 0u
    };
    QwJp2PacketComponentGeometry component = {0};
    QwJp2PacketPrecinctGrid grid = {0};
    QwJp2PacketPrecinctCodeBlockGrid codeblocks = {0};
    QwJp2PacketOrderEntry packet = {0};

    component.x_sampling = 1u;
    component.y_sampling = 1u;
    component.decomposition_levels = 1u;
    component.codeblock_width_exponent = 0u;
    component.codeblock_height_exponent = 0u;
    component.precinct_x_exponents[0] = 1u;
    component.precinct_y_exponents[0] = 1u;
    component.precinct_x_exponents[1] = 2u;
    component.precinct_y_exponents[1] = 1u;
    if (qw_jp2_packet_precinct_grid_compute(&tile, &component, 0u,
                                            &grid) != QW_JP2_PACKET_OK ||
        grid.resolution_x0 != 0u || grid.resolution_x1 != 8u ||
        grid.resolution_y0 != 0u || grid.resolution_y1 != 4u ||
        grid.precinct_x0 != 0u || grid.precinct_y0 != 0u ||
        grid.precincts_wide != 4u || grid.precincts_high != 2u ||
        grid.precinct_count != 8u || grid.reference_x_step != 4u ||
        grid.reference_y_step != 4u) {
        fputs("resolution-zero precinct geometry mismatch\n", stderr);
        return 0;
    }
    packet.precinct_index = 0u;
    if (qw_jp2_packet_precinct_codeblock_grid_compute(
            &tile, &component, &packet, QW_JP2_SUBBAND_LL,
            &codeblocks) != QW_JP2_PACKET_OK ||
        codeblocks.subband_x0 != 0u || codeblocks.subband_y0 != 0u ||
        codeblocks.subband_x1 != 2u || codeblocks.subband_y1 != 2u ||
        codeblocks.codeblocks_wide != 1u ||
        codeblocks.codeblocks_high != 1u ||
        codeblocks.codeblock_count != 1u ||
        codeblocks.codeblock_width_exponent != 1u ||
        codeblocks.codeblock_height_exponent != 1u) {
        fputs("resolution-zero precinct code-block mapping mismatch\n",
              stderr);
        return 0;
    }
    if (qw_jp2_packet_precinct_grid_compute(&tile, &component, 1u,
                                            &grid) != QW_JP2_PACKET_OK ||
        grid.resolution_x0 != 0u || grid.resolution_x1 != 16u ||
        grid.resolution_y0 != 0u || grid.resolution_y1 != 8u ||
        grid.precincts_wide != 4u || grid.precincts_high != 4u ||
        grid.precinct_count != 16u || grid.reference_x_step != 4u ||
        grid.reference_y_step != 2u) {
        fputs("reduced-resolution precinct geometry mismatch\n", stderr);
        return 0;
    }
    packet.precinct_index = 5u;
    packet.reference_x = 4u;
    packet.reference_y = 2u;
    packet.resolution_level = 1u;
    if (qw_jp2_packet_precinct_codeblock_grid_compute(
            &tile, &component, &packet, QW_JP2_SUBBAND_HL,
            &codeblocks) != QW_JP2_PACKET_OK ||
        codeblocks.subband_x0 != 2u || codeblocks.subband_y0 != 1u ||
        codeblocks.subband_x1 != 4u || codeblocks.subband_y1 != 2u ||
        codeblocks.codeblock_x0 != 1u || codeblocks.codeblock_y0 != 1u ||
        codeblocks.codeblocks_wide != 1u ||
        codeblocks.codeblocks_high != 1u ||
        codeblocks.codeblock_count != 1u ||
        codeblocks.codeblock_width_exponent != 1u ||
        codeblocks.codeblock_height_exponent != 0u) {
        fputs("detail-band precinct code-block mapping mismatch\n", stderr);
        return 0;
    }
    ++packet.reference_x;
    if (qw_jp2_packet_precinct_codeblock_grid_compute(
            &tile, &component, &packet, QW_JP2_SUBBAND_HL,
            &codeblocks) != QW_JP2_PACKET_ERR_FORMAT) {
        fputs("inconsistent packet precinct origin was accepted\n", stderr);
        return 0;
    }
    packet.reference_x = 4u;
    if (qw_jp2_packet_precinct_codeblock_grid_compute(
            &tile, &component, &packet, QW_JP2_SUBBAND_LL,
            &codeblocks) != QW_JP2_PACKET_ERR_FORMAT) {
        fputs("LL band at a nonzero resolution was accepted\n", stderr);
        return 0;
    }

    tile = (QwJp2PacketTileGeometry){
        .x_size = 32u,
        .y_size = 16u,
        .x_origin = 0u,
        .y_origin = 0u,
        .tile_width = 32u,
        .tile_height = 16u,
        .tile_x_origin = 0u,
        .tile_y_origin = 0u,
        .tile_index = 0u
    };
    component.precinct_x_exponents[1] = 5u;
    component.precinct_y_exponents[1] = 3u;
    packet.precinct_index = 0u;
    packet.reference_x = 0u;
    packet.reference_y = 0u;
    if (qw_jp2_packet_precinct_codeblock_grid_compute(
            &tile, &component, &packet, QW_JP2_SUBBAND_HL,
            &codeblocks) != QW_JP2_PACKET_OK ||
        codeblocks.codeblocks_wide != 4u ||
        codeblocks.codeblocks_high != 1u ||
        codeblocks.codeblock_count != 4u ||
        codeblocks.codeblock_width_exponent != 2u ||
        codeblocks.codeblock_height_exponent != 2u) {
        fputs("multi-code-block packet precinct mapping mismatch\n", stderr);
        return 0;
    }
    packet.precinct_index = 1u;
    packet.reference_y = 8u;
    if (qw_jp2_packet_precinct_codeblock_grid_compute(
            &tile, &component, &packet, QW_JP2_SUBBAND_HL,
            &codeblocks) != QW_JP2_PACKET_OK ||
        codeblocks.codeblock_y0 != 1u ||
        codeblocks.codeblocks_wide != 4u ||
        codeblocks.codeblocks_high != 1u ||
        codeblocks.codeblock_count != 4u) {
        fputs("second packet precinct code-block origin mismatch\n", stderr);
        return 0;
    }

    tile = (QwJp2PacketTileGeometry){
        .x_size = 15u,
        .y_size = 7u,
        .x_origin = 0u,
        .y_origin = 0u,
        .tile_width = 15u,
        .tile_height = 7u,
        .tile_x_origin = 0u,
        .tile_y_origin = 0u,
        .tile_index = 0u
    };
    component.precinct_x_exponents[1] = 4u;
    component.precinct_y_exponents[1] = 4u;
    packet.precinct_index = 0u;
    packet.reference_x = 0u;
    packet.reference_y = 0u;
    if (qw_jp2_packet_precinct_codeblock_grid_compute(
            &tile, &component, &packet, QW_JP2_SUBBAND_HL,
            &codeblocks) != QW_JP2_PACKET_OK ||
        codeblocks.subband_x0 != 0u || codeblocks.subband_x1 != 7u ||
        codeblocks.subband_y0 != 0u || codeblocks.subband_y1 != 4u ||
        codeblocks.codeblocks_wide != 2u ||
        codeblocks.codeblocks_high != 1u) {
        fputs("odd-sized HL subband edge mapping mismatch\n", stderr);
        return 0;
    }
    if (qw_jp2_packet_precinct_codeblock_grid_compute(
            &tile, &component, &packet, QW_JP2_SUBBAND_LH,
            &codeblocks) != QW_JP2_PACKET_OK ||
        codeblocks.subband_x0 != 0u || codeblocks.subband_x1 != 8u ||
        codeblocks.subband_y0 != 0u || codeblocks.subband_y1 != 3u ||
        codeblocks.codeblocks_wide != 2u ||
        codeblocks.codeblocks_high != 1u) {
        fputs("odd-sized LH subband edge mapping mismatch\n", stderr);
        return 0;
    }
    if (qw_jp2_packet_precinct_codeblock_grid_compute(
            &tile, &component, &packet, QW_JP2_SUBBAND_HH,
            &codeblocks) != QW_JP2_PACKET_OK ||
        codeblocks.subband_x0 != 0u || codeblocks.subband_x1 != 7u ||
        codeblocks.subband_y0 != 0u || codeblocks.subband_y1 != 3u ||
        codeblocks.codeblocks_wide != 2u ||
        codeblocks.codeblocks_high != 1u) {
        fputs("odd-sized HH subband edge mapping mismatch\n", stderr);
        return 0;
    }

    tile = (QwJp2PacketTileGeometry){
        .x_size = 20u,
        .y_size = 14u,
        .x_origin = 0u,
        .y_origin = 0u,
        .tile_width = 8u,
        .tile_height = 8u,
        .tile_x_origin = 0u,
        .tile_y_origin = 0u,
        .tile_index = 2u
    };
    component = (QwJp2PacketComponentGeometry){0};
    component.x_sampling = 2u;
    component.y_sampling = 1u;
    component.decomposition_levels = 2u;
    component.codeblock_width_exponent = 1u;
    component.codeblock_height_exponent = 1u;
    component.precinct_x_exponents[1] = 15u;
    component.precinct_y_exponents[1] = 15u;
    packet = (QwJp2PacketOrderEntry){0};
    packet.resolution_level = 1u;
    if (qw_jp2_packet_precinct_codeblock_grid_compute(
            &tile, &component, &packet, QW_JP2_SUBBAND_HL,
            &codeblocks) != QW_JP2_PACKET_OK ||
        codeblocks.subband_x0 != codeblocks.subband_x1 ||
        codeblocks.codeblocks_wide != 0u || codeblocks.codeblock_count != 0u ||
        qw_jp2_packet_precinct_codeblock_grid_compute(
            &tile, &component, &packet, QW_JP2_SUBBAND_LH,
            &codeblocks) != QW_JP2_PACKET_OK ||
        codeblocks.subband_x0 != 2u || codeblocks.subband_x1 != 3u ||
        codeblocks.codeblocks_wide != 1u || codeblocks.codeblocks_high != 1u ||
        codeblocks.codeblock_count != 1u ||
        qw_jp2_packet_precinct_codeblock_grid_compute(
            &tile, &component, &packet, QW_JP2_SUBBAND_HH,
            &codeblocks) != QW_JP2_PACKET_OK ||
        codeblocks.subband_x0 != codeblocks.subband_x1 ||
        codeblocks.codeblocks_wide != 0u || codeblocks.codeblock_count != 0u) {
        fputs("empty edge subbands created phantom code-blocks\n", stderr);
        return 0;
    }

    tile = (QwJp2PacketTileGeometry){
        .x_size = 14u,
        .y_size = 11u,
        .x_origin = 3u,
        .y_origin = 2u,
        .tile_width = 8u,
        .tile_height = 6u,
        .tile_x_origin = 1u,
        .tile_y_origin = 1u,
        .tile_index = 3u
    };
    component = (QwJp2PacketComponentGeometry){0};
    component.x_sampling = 2u;
    component.y_sampling = 3u;
    component.precinct_x_exponents[0] = 2u;
    component.precinct_y_exponents[0] = 1u;
    if (qw_jp2_packet_precinct_grid_compute(&tile, &component, 0u,
                                            &grid) != QW_JP2_PACKET_OK ||
        grid.tile_component_x0 != 5u || grid.tile_component_x1 != 7u ||
        grid.tile_component_y0 != 3u || grid.tile_component_y1 != 4u ||
        grid.precinct_x0 != 1u || grid.precinct_y0 != 1u ||
        grid.precincts_wide != 1u || grid.precincts_high != 1u ||
        grid.precinct_count != 1u || grid.reference_x_step != 8u ||
        grid.reference_y_step != 6u) {
        fputs("offset, subsampled tile-component geometry mismatch\n", stderr);
        return 0;
    }

    tile = (QwJp2PacketTileGeometry){
        .x_size = 2u,
        .y_size = 2u,
        .x_origin = 0u,
        .y_origin = 0u,
        .tile_width = 1u,
        .tile_height = 2u,
        .tile_x_origin = 0u,
        .tile_y_origin = 0u,
        .tile_index = 1u
    };
    component = (QwJp2PacketComponentGeometry){0};
    component.x_sampling = 2u;
    component.y_sampling = 1u;
    if (qw_jp2_packet_precinct_grid_compute(&tile, &component, 0u,
                                            &grid) != QW_JP2_PACKET_OK ||
        grid.tile_component_x0 != grid.tile_component_x1 ||
        grid.precinct_count != 0u) {
        fputs("empty tile-component precinct grid mismatch\n", stderr);
        return 0;
    }

    component.decomposition_levels = 1u;
    component.precinct_y_exponents[1] = 0u;
    if (qw_jp2_packet_precinct_grid_compute(&tile, &component, 1u,
                                            &grid) !=
        QW_JP2_PACKET_ERR_FORMAT) {
        fputs("zero precinct exponent above resolution zero was accepted\n",
              stderr);
        return 0;
    }
    tile.tile_index = 2u;
    if (qw_jp2_packet_precinct_grid_compute(&tile, &component, 0u,
                                            &grid) !=
        QW_JP2_PACKET_ERR_FORMAT) {
        fputs("out-of-range tile index was accepted\n", stderr);
        return 0;
    }
    return 1;
}

static int packet_fields_equal(const QwJp2PacketOrderEntry *entry,
                               uint16_t layer,
                               uint16_t component,
                               uint8_t resolution,
                               uint32_t precinct)
{
    return entry->layer_index == layer &&
           entry->component_index == component &&
           entry->resolution_level == resolution &&
           entry->precinct_index == precinct;
}

static int test_progression_volume_expansion(void)
{
    QwJp2PacketTileGeometry tile = {
        .x_size = 4u,
        .y_size = 2u,
        .x_origin = 0u,
        .y_origin = 0u,
        .tile_width = 4u,
        .tile_height = 2u,
        .tile_x_origin = 0u,
        .tile_y_origin = 0u,
        .tile_index = 0u
    };
    QwJp2PacketComponentGeometry components[2] = {{0}};
    QwJp2PacketProgressionVolume volumes[2] = {
        {0u, 1u, 1u, 0u, 1u, QW_JP2_PROGRESSION_LRCP},
        {0u, 2u, 2u, 0u, 33u, QW_JP2_PROGRESSION_CPRL}
    };
    QwJp2PacketProgressionVolume empty_volume = {
        1u, 2u, 1u, 2u, 3u, QW_JP2_PROGRESSION_LRCP
    };
    QwJp2PacketOrderEntry entries[6] = {{0}};
    QwJp2PacketOrderEntry original[6];
    size_t count = 99u;
    size_t queried_count = 99u;

    components[0].x_sampling = 1u;
    components[0].y_sampling = 1u;
    components[0].decomposition_levels = 1u;
    components[0].precinct_x_exponents[0] = 2u;
    components[0].precinct_y_exponents[0] = 1u;
    components[0].precinct_x_exponents[1] = 2u;
    components[0].precinct_y_exponents[1] = 1u;
    components[1].x_sampling = 2u;
    components[1].y_sampling = 1u;
    components[1].precinct_x_exponents[0] = 1u;
    components[1].precinct_y_exponents[0] = 1u;

    if (qw_jp2_packet_progression_expand_volumes(
            &tile, components, 2u, 2u, QW_JP2_PROGRESSION_LRCP,
            volumes, 2u, NULL, 0u, &queried_count) != QW_JP2_PACKET_OK ||
        queried_count != 6u) {
        fputs("packet expansion count query did not return exact size\n",
              stderr);
        return 0;
    }

    if (qw_jp2_packet_progression_expand_volumes(
            &tile, components, 2u, 2u, QW_JP2_PROGRESSION_LRCP,
            volumes, 2u, entries, 6u, &count) != QW_JP2_PACKET_OK ||
        count != 6u ||
        !packet_fields_equal(&entries[0], 0u, 0u, 0u, 0u) ||
        !packet_fields_equal(&entries[1], 1u, 0u, 0u, 0u) ||
        !packet_fields_equal(&entries[2], 0u, 0u, 1u, 0u) ||
        !packet_fields_equal(&entries[3], 1u, 0u, 1u, 0u) ||
        !packet_fields_equal(&entries[4], 0u, 1u, 0u, 0u) ||
        !packet_fields_equal(&entries[5], 1u, 1u, 0u, 0u)) {
        fputs("POC progression volumes did not expand without duplicate packets\n",
              stderr);
        return 0;
    }
    for (size_t i = 0u; i < 6u; ++i) {
        if (entries[i].reference_x != 0u || entries[i].reference_y != 0u) {
            fputs("packet key reference-grid position mismatch\n", stderr);
            return 0;
        }
    }

    if (qw_jp2_packet_progression_expand_volumes(
            &tile, components, 2u, 2u, QW_JP2_PROGRESSION_LRCP,
            NULL, 0u, entries, 6u, &count) != QW_JP2_PACKET_OK ||
        count != 6u ||
        !packet_fields_equal(&entries[0], 0u, 0u, 0u, 0u) ||
        !packet_fields_equal(&entries[1], 0u, 1u, 0u, 0u) ||
        !packet_fields_equal(&entries[2], 0u, 0u, 1u, 0u) ||
        !packet_fields_equal(&entries[3], 1u, 0u, 0u, 0u) ||
        !packet_fields_equal(&entries[4], 1u, 1u, 0u, 0u) ||
        !packet_fields_equal(&entries[5], 1u, 0u, 1u, 0u)) {
        fputs("default COD progression was not expanded correctly\n", stderr);
        return 0;
    }

    {
        QwJp2PacketTileGeometry spatial_tile = {
            .x_size = 4u,
            .y_size = 4u,
            .x_origin = 0u,
            .y_origin = 0u,
            .tile_width = 4u,
            .tile_height = 4u,
            .tile_x_origin = 0u,
            .tile_y_origin = 0u,
            .tile_index = 0u
        };
        QwJp2PacketComponentGeometry spatial_component = {0};

        spatial_component.x_sampling = 1u;
        spatial_component.y_sampling = 1u;
        spatial_component.precinct_x_exponents[0] = 1u;
        spatial_component.precinct_y_exponents[0] = 1u;
        if (qw_jp2_packet_progression_expand_volumes(
                &spatial_tile, &spatial_component, 1u, 1u,
                QW_JP2_PROGRESSION_PCRL, NULL, 0u, entries, 4u, &count) !=
                QW_JP2_PACKET_OK ||
            count != 4u ||
            entries[0].reference_x != 0u || entries[0].reference_y != 0u ||
            entries[1].reference_x != 2u || entries[1].reference_y != 0u ||
            entries[2].reference_x != 0u || entries[2].reference_y != 2u ||
            entries[3].reference_x != 2u || entries[3].reference_y != 2u) {
            fputs("derived precinct coordinates did not follow PCRL raster order\n",
                  stderr);
            return 0;
        }
    }

    if (qw_jp2_packet_progression_expand_volumes(
            &tile, components, 2u, 2u, QW_JP2_PROGRESSION_LRCP,
            &empty_volume, 1u, entries, 6u, &count) != QW_JP2_PACKET_OK ||
        count != 0u) {
        fputs("POC ranges beyond a component's levels were not clipped\n",
              stderr);
        return 0;
    }

    memset(entries, 0x5a, sizeof(entries));
    memcpy(original, entries, sizeof(entries));
    count = 99u;
    if (qw_jp2_packet_progression_expand_volumes(
            &tile, components, 2u, 2u, QW_JP2_PROGRESSION_LRCP,
            volumes, 2u, entries, 5u, &count) !=
            QW_JP2_PACKET_ERR_RESOURCE || count != 99u ||
        memcmp(entries, original, sizeof(entries)) != 0) {
        fputs("packet expansion capacity failure was not transactional\n",
              stderr);
        return 0;
    }
    return 1;
}

int main(void)
{
    if (!test_progression_orders() || !test_arguments_and_resource_limit() ||
        !test_poc_payloads() || !test_precinct_geometry() ||
        !test_progression_volume_expansion()) {
        return 1;
    }
    puts("Quirewise JPEG 2000 packet progression tests passed");
    return 0;
}
