#include "image_internal.h"

#include "quirewise/image.h"
#include "quirewise/status.h"

#include <openjpeg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t reversible_gray8_fixture[] = {
    0x00u, 0x00u, 0x00u, 0x0cu, 0x6au, 0x50u, 0x20u, 0x20u, 0x0du, 0x0au, 0x87u, 0x0au,
    0x00u, 0x00u, 0x00u, 0x14u, 0x66u, 0x74u, 0x79u, 0x70u, 0x6au, 0x70u, 0x32u, 0x20u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x6au, 0x70u, 0x32u, 0x20u, 0x00u, 0x00u, 0x00u, 0x2du,
    0x6au, 0x70u, 0x32u, 0x68u, 0x00u, 0x00u, 0x00u, 0x16u, 0x69u, 0x68u, 0x64u, 0x72u,
    0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x01u, 0x07u, 0x07u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x0fu, 0x63u, 0x6fu, 0x6cu, 0x72u, 0x01u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x11u, 0x00u, 0x00u, 0x00u, 0x89u, 0x6au, 0x70u, 0x32u,
    0x63u, 0xffu, 0x4fu, 0xffu, 0x51u, 0x00u, 0x29u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x01u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x07u, 0x01u, 0x01u, 0xffu, 0x52u,
    0x00u, 0x0cu, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x20u, 0x01u,
    0xffu, 0x5cu, 0x00u, 0x04u, 0x40u, 0x40u, 0xffu, 0x64u, 0x00u, 0x25u, 0x00u, 0x01u,
    0x43u, 0x72u, 0x65u, 0x61u, 0x74u, 0x65u, 0x64u, 0x20u, 0x62u, 0x79u, 0x20u, 0x4fu,
    0x70u, 0x65u, 0x6eu, 0x4au, 0x50u, 0x45u, 0x47u, 0x20u, 0x76u, 0x65u, 0x72u, 0x73u,
    0x69u, 0x6fu, 0x6eu, 0x20u, 0x32u, 0x2eu, 0x35u, 0x2eu, 0x34u, 0xffu, 0x90u, 0x00u,
    0x0au, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x17u, 0x00u, 0x01u, 0xffu, 0x93u, 0xdfu,
    0x80u, 0x30u, 0x08u, 0xa0u, 0x57u, 0xacu, 0x9fu, 0x7fu, 0xffu, 0xd9u,
};

static int read_fixture(const char *name, uint8_t **out_data, size_t *out_size)
{
    char path[512];
    FILE *file;
    long length;
    uint8_t *data;
    int path_length;
    if (!name || !out_data || !out_size) return 0;
    *out_data = NULL;
    *out_size = 0u;
    path_length = snprintf(path, sizeof(path), "%s/%s",
                           QUIREWISE_JP2_TEST_DATA_DIR, name);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        return 0;
    }
    file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0L, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    data = (uint8_t *)malloc((size_t)length ? (size_t)length : 1u);
    if (!data) {
        fclose(file);
        return 0;
    }
    if (fread(data, 1u, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *out_data = data;
    *out_size = (size_t)length;
    return 1;
}

static void write_be32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static void write_be16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static uint16_t read_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static uint8_t *insert_bytes(const uint8_t *data,
                             size_t size,
                             size_t offset,
                             const uint8_t *insert,
                             size_t insert_size)
{
    uint8_t *result;
    if (!data || !insert || offset > size || size > SIZE_MAX - insert_size) {
        return NULL;
    }
    result = (uint8_t *)malloc(size + insert_size);
    if (!result) return NULL;
    memcpy(result, data, offset);
    memcpy(result + offset, insert, insert_size);
    memcpy(result + offset + insert_size, data + offset, size - offset);
    return result;
}

static size_t find_first_marker(const uint8_t *data,
                                size_t size,
                                size_t start,
                                uint8_t marker);
static size_t find_first_tile_sod(const uint8_t *data,
                                  size_t size,
                                  size_t sot_offset);

static void make_poc_segment(uint8_t segment[11],
                             uint16_t component_count,
                             uint16_t layer_end,
                             uint8_t resolution_end)
{
    segment[0] = 0xffu;
    segment[1] = 0x5fu;
    write_be16(segment + 2u, 9u);
    segment[4] = 0u;
    segment[5] = 0u;
    write_be16(segment + 6u, layer_end);
    segment[8] = resolution_end;
    segment[9] = (uint8_t)component_count;
    segment[10] = 0u;
}

static void make_coc_segment(uint8_t segment[11],
                             const uint8_t *codestream,
                             size_t cod_offset)
{
    segment[0] = 0xffu;
    segment[1] = 0x53u;
    write_be16(segment + 2u, 9u);
    segment[4] = 0u;
    segment[5] = 0u;
    segment[6] = 0u;
    memcpy(segment + 7u, codestream + cod_offset + 10u, 4u);
}

static void make_tile_cod_segment(uint8_t segment[14],
                                  const uint8_t *codestream,
                                  size_t cod_offset,
                                  uint16_t layer_count,
                                  uint8_t decomposition_levels)
{
    segment[0] = 0xffu;
    segment[1] = 0x52u;
    write_be16(segment + 2u, 12u);
    segment[4] = 0u;
    segment[5] = 0u;
    write_be16(segment + 6u, layer_count);
    segment[8] = 0u;
    segment[9] = decomposition_levels;
    memcpy(segment + 10u, codestream + cod_offset + 10u, 4u);
}

static int check_tile_coding_state_across_parts(const uint8_t *codestream,
                                                size_t codestream_size)
{
    size_t cod_offset = find_first_marker(codestream, codestream_size,
                                          0u, 0x52u);
    size_t sot_offset = find_first_marker(codestream, codestream_size,
                                         0u, 0x90u);
    size_t sod_offset;
    size_t body_offset;
    size_t body_size;
    const size_t first_part_size = 50u;
    size_t second_part_size;
    size_t expanded_size;
    size_t second_sot_offset;
    size_t cursor;
    uint16_t component_count;
    uint16_t layer_count;
    uint16_t tile_layer_count;
    uint16_t tile_index;
    uint8_t tile_cod[14];
    uint8_t tile_coc[11];
    uint8_t first_poc[11];
    uint8_t second_poc[11];
    uint8_t *expanded = NULL;
    int passed = 0;

    if (cod_offset == codestream_size || sot_offset == codestream_size ||
        codestream_size < 42u) {
        fputs("could not locate COD/SOT for tile-state mutation\n", stderr);
        return 0;
    }
    sod_offset = find_first_tile_sod(codestream, codestream_size, sot_offset);
    if (sod_offset == codestream_size) {
        fputs("could not locate SOD for tile-state mutation\n", stderr);
        return 0;
    }
    component_count = read_be16(codestream + 40u);
    layer_count = read_be16(codestream + cod_offset + 6u);
    tile_index = read_be16(codestream + sot_offset + 4u);
    if (component_count == 0u || component_count >= 257u || !layer_count) {
        fputs("tile-state fixture exceeds its supported marker widths\n",
              stderr);
        return 0;
    }
    if (layer_count == 1u) layer_count = 2u;
    tile_layer_count = (uint16_t)(layer_count - 1u);
    body_offset = sod_offset + 2u;
    if (body_offset > codestream_size || codestream_size - body_offset < 2u) {
        fputs("tile-state fixture has no bounded packet body\n", stderr);
        return 0;
    }
    body_size = codestream_size - body_offset - 2u;
    if (body_size > UINT32_MAX - 25u ||
        body_size > SIZE_MAX - first_part_size - 25u) {
        fputs("tile-state fixture exceeds Psot limits\n", stderr);
        return 0;
    }
    second_part_size = 25u + body_size;
    expanded_size = sot_offset + first_part_size + second_part_size + 2u;
    make_tile_cod_segment(tile_cod, codestream, cod_offset,
                          tile_layer_count, 0u);
    make_coc_segment(tile_coc, codestream, cod_offset);
    make_poc_segment(first_poc, component_count, tile_layer_count, 1u);
    make_poc_segment(second_poc, component_count, tile_layer_count, 2u);

    expanded = (uint8_t *)malloc(expanded_size);
    if (!expanded) return 0;
    memcpy(expanded, codestream, sot_offset);
    if (read_be16(codestream + cod_offset + 6u) == 1u) {
        write_be16(expanded + cod_offset + 6u, layer_count);
    }
    cursor = sot_offset;
    expanded[cursor] = 0xffu;
    expanded[cursor + 1u] = 0x90u;
    write_be16(expanded + cursor + 2u, 10u);
    write_be16(expanded + cursor + 4u, tile_index);
    write_be32(expanded + cursor + 6u, (uint32_t)first_part_size);
    expanded[cursor + 10u] = 0u;
    expanded[cursor + 11u] = 2u;
    cursor += 12u;
    memcpy(expanded + cursor, tile_cod, sizeof(tile_cod));
    cursor += sizeof(tile_cod);
    memcpy(expanded + cursor, tile_coc, sizeof(tile_coc));
    cursor += sizeof(tile_coc);
    memcpy(expanded + cursor, first_poc, sizeof(first_poc));
    cursor += sizeof(first_poc);
    expanded[cursor] = 0xffu;
    expanded[cursor + 1u] = 0x93u;
    cursor += 2u;
    if (cursor != sot_offset + first_part_size) goto cleanup;

    second_sot_offset = cursor;
    expanded[cursor] = 0xffu;
    expanded[cursor + 1u] = 0x90u;
    write_be16(expanded + cursor + 2u, 10u);
    write_be16(expanded + cursor + 4u, tile_index);
    write_be32(expanded + cursor + 6u, (uint32_t)second_part_size);
    expanded[cursor + 10u] = 1u;
    expanded[cursor + 11u] = 2u;
    cursor += 12u;
    memcpy(expanded + cursor, second_poc, sizeof(second_poc));
    cursor += sizeof(second_poc);
    expanded[cursor] = 0xffu;
    expanded[cursor + 1u] = 0x93u;
    cursor += 2u;
    memcpy(expanded + cursor, codestream + body_offset, body_size);
    cursor += body_size;
    expanded[cursor] = 0xffu;
    expanded[cursor + 1u] = 0xd9u;
    cursor += 2u;
    if (cursor != expanded_size) goto cleanup;

    if (qw_jp2_validate_structure(expanded, expanded_size) != QW_STATUS_OK) {
        fputs("valid tile COD/COC state across tile-parts was rejected\n",
              stderr);
        goto cleanup;
    }
    write_be16(expanded + second_sot_offset + 12u + 6u,
               (uint16_t)(tile_layer_count + 1u));
    if (qw_jp2_validate_structure(expanded, expanded_size) !=
        QW_STATUS_OK) {
        fputs("later tile POC above the persisted tile COD layer count was rejected\n",
              stderr);
        goto cleanup;
    }
    passed = 1;

cleanup:
    free(expanded);
    return passed;
}

static size_t make_empty_packet_codestream(uint8_t *data, size_t capacity);

static int check_later_tile_poc_packet_schedule(void)
{
    uint8_t codestream[119];
    uint8_t first_poc[11];
    uint8_t second_poc[11];
    size_t cursor = 65u;
    size_t codestream_size = make_empty_packet_codestream(
        codestream, sizeof(codestream));

    if (codestream_size != 82u) return 0;
    write_be16(codestream + 51u, 2u);
    make_poc_segment(first_poc, 1u, 1u, 1u);
    make_poc_segment(second_poc, 1u, 2u, 1u);

    codestream[cursor] = 0xffu;
    codestream[cursor + 1u] = 0x90u;
    write_be16(codestream + cursor + 2u, 10u);
    write_be16(codestream + cursor + 4u, 0u);
    write_be32(codestream + cursor + 6u, 26u);
    codestream[cursor + 10u] = 0u;
    codestream[cursor + 11u] = 2u;
    cursor += 12u;
    memcpy(codestream + cursor, first_poc, sizeof(first_poc));
    cursor += sizeof(first_poc);
    codestream[cursor] = 0xffu;
    codestream[cursor + 1u] = 0x93u;
    codestream[cursor + 2u] = 0u;
    cursor += 3u;

    codestream[cursor] = 0xffu;
    codestream[cursor + 1u] = 0x90u;
    write_be16(codestream + cursor + 2u, 10u);
    write_be16(codestream + cursor + 4u, 0u);
    write_be32(codestream + cursor + 6u, 26u);
    codestream[cursor + 10u] = 1u;
    codestream[cursor + 11u] = 2u;
    cursor += 12u;
    memcpy(codestream + cursor, second_poc, sizeof(second_poc));
    cursor += sizeof(second_poc);
    codestream[cursor] = 0xffu;
    codestream[cursor + 1u] = 0x93u;
    codestream[cursor + 2u] = 0u;
    cursor += 3u;
    codestream[cursor] = 0xffu;
    codestream[cursor + 1u] = 0xd9u;
    cursor += 2u;

    if (cursor != sizeof(codestream) ||
        qw_jp2_validate_structure(codestream, sizeof(codestream)) !=
            QW_STATUS_OK) {
        fputs("later tile-part POC did not append its packet volume\n", stderr);
        return 0;
    }
    write_be16(codestream + 103u + 6u, 3u);
    if (qw_jp2_validate_structure(codestream, sizeof(codestream)) !=
        QW_STATUS_OK) {
        fputs("later tile-part POC above the tile COD layer count was rejected\n",
              stderr);
        return 0;
    }
    return 1;
}

static int check_signaled_precinct_sizes(const uint8_t *codestream,
                                         size_t codestream_size)
{
    size_t cod_offset = find_first_marker(codestream, codestream_size,
                                          0u, 0x52u);
    size_t sot_offset = find_first_marker(codestream, codestream_size,
                                          0u, 0x90u);
    size_t precinct_size_count;
    size_t codestream_with_cod_size;
    size_t codestream_with_coc_size;
    uint16_t cod_length;
    uint16_t component_count;
    uint8_t decomposition_levels;
    uint8_t coc[44];
    uint8_t *with_cod = NULL;
    uint8_t *bad_cod = NULL;
    uint8_t *with_coc = NULL;
    uint8_t *bad_coc = NULL;
    int passed = 0;

    if (cod_offset == codestream_size || sot_offset == codestream_size ||
        codestream_size < 42u || cod_offset > sot_offset ||
        cod_offset > codestream_size - 14u) {
        fputs("could not locate bounded COD/SOT for precinct marker vectors\n",
              stderr);
        return 0;
    }
    component_count = read_be16(codestream + 40u);
    decomposition_levels = codestream[cod_offset + 9u];
    precinct_size_count = (size_t)decomposition_levels + 1u;
    cod_length = read_be16(codestream + cod_offset + 2u);
    if (!component_count || component_count > 4u ||
        decomposition_levels > 32u || cod_length < 12u ||
        cod_offset + 2u + cod_length > codestream_size ||
        precinct_size_count > sizeof(coc) - 11u) {
        fputs("COD precinct marker fixture has invalid bounds\n", stderr);
        return 0;
    }

    if (codestream[cod_offset + 4u] & 0x01u) {
        if (cod_length != 12u + precinct_size_count) {
            fputs("fixture's signaled COD length is inconsistent\n", stderr);
            goto cleanup;
        }
        codestream_with_cod_size = codestream_size;
        with_cod = (uint8_t *)malloc(codestream_size);
        if (with_cod) memcpy(with_cod, codestream, codestream_size);
    } else {
        uint8_t precinct_bytes[33];
        if (cod_length != 12u ||
            codestream_size > SIZE_MAX - precinct_size_count) {
            fputs("fixture's default COD length is inconsistent\n", stderr);
            goto cleanup;
        }
        memset(precinct_bytes, 0xff, precinct_size_count);
        codestream_with_cod_size = codestream_size + precinct_size_count;
        with_cod = insert_bytes(codestream, codestream_size,
                                cod_offset + 2u + cod_length,
                                precinct_bytes, precinct_size_count);
        if (with_cod) {
            write_be16(with_cod + cod_offset + 2u,
                       (uint16_t)(cod_length + precinct_size_count));
            with_cod[cod_offset + 4u] |= 0x01u;
        }
    }
    if (!with_cod) goto cleanup;
    if (qw_jp2_validate_structure(with_cod, codestream_with_cod_size) !=
        QW_STATUS_OK) {
        fputs("valid COD with explicit precinct sizes was rejected\n", stderr);
        goto cleanup;
    }
    bad_cod = (uint8_t *)malloc(codestream_with_cod_size);
    if (!bad_cod) goto cleanup;
    memcpy(bad_cod, with_cod, codestream_with_cod_size);
    write_be16(bad_cod + cod_offset + 2u,
               (uint16_t)(read_be16(bad_cod + cod_offset + 2u) - 1u));
    if (qw_jp2_validate_structure(bad_cod, codestream_with_cod_size) !=
        QW_STATUS_ERR_FORMAT) {
        fputs("COD with mismatched explicit precinct length was accepted\n",
              stderr);
        goto cleanup;
    }

    memset(coc, 0, sizeof(coc));
    coc[0] = 0xffu;
    coc[1] = 0x53u;
    write_be16(coc + 2u,
               (uint16_t)(9u + precinct_size_count));
    coc[4] = 0u;
    coc[5] = 0x01u;
    coc[6] = decomposition_levels;
    memcpy(coc + 7u, codestream + cod_offset + 10u, 4u);
    memset(coc + 11u, 0xff, precinct_size_count);
    if (codestream_size > SIZE_MAX - 11u - precinct_size_count) {
        goto cleanup;
    }
    codestream_with_coc_size = codestream_size + 11u + precinct_size_count;
    with_coc = insert_bytes(codestream, codestream_size, sot_offset,
                            coc, 11u + precinct_size_count);
    if (!with_coc) goto cleanup;
    if (qw_jp2_validate_structure(with_coc, codestream_with_coc_size) !=
        QW_STATUS_OK) {
        fputs("valid COC with explicit precinct sizes was rejected\n", stderr);
        goto cleanup;
    }
    bad_coc = (uint8_t *)malloc(codestream_with_coc_size);
    if (!bad_coc) goto cleanup;
    memcpy(bad_coc, with_coc, codestream_with_coc_size);
    write_be16(bad_coc + sot_offset + 2u,
               (uint16_t)(read_be16(bad_coc + sot_offset + 2u) - 1u));
    if (qw_jp2_validate_structure(bad_coc, codestream_with_coc_size) !=
        QW_STATUS_ERR_FORMAT) {
        fputs("COC with mismatched explicit precinct length was accepted\n",
              stderr);
        goto cleanup;
    }
    passed = 1;

cleanup:
    free(bad_coc);
    free(with_coc);
    free(bad_cod);
    free(with_cod);
    return passed;
}

static size_t make_empty_packet_codestream(uint8_t *data, size_t capacity)
{
    enum { CODESTREAM_SIZE = 82u };
    if (!data || capacity < CODESTREAM_SIZE) return 0u;
    memset(data, 0, CODESTREAM_SIZE);
    data[0] = 0xffu;
    data[1] = 0x4fu;
    data[2] = 0xffu;
    data[3] = 0x51u;
    write_be16(data + 4u, 41u);
    write_be32(data + 8u, 1u);
    write_be32(data + 12u, 1u);
    write_be32(data + 24u, 1u);
    write_be32(data + 28u, 1u);
    write_be16(data + 40u, 1u);
    data[42] = 7u;
    data[43] = 1u;
    data[44] = 1u;

    data[45] = 0xffu;
    data[46] = 0x52u;
    write_be16(data + 47u, 12u);
    data[49] = 0u;
    data[50] = 0u;
    write_be16(data + 51u, 1u);
    data[53] = 0u;
    data[54] = 0u;
    data[55] = 4u;
    data[56] = 4u;
    data[57] = 0u;
    data[58] = 1u;

    data[59] = 0xffu;
    data[60] = 0x5cu;
    write_be16(data + 61u, 4u);
    data[63] = 0u;
    data[64] = 0x18u;

    data[65] = 0xffu;
    data[66] = 0x90u;
    write_be16(data + 67u, 10u);
    write_be32(data + 71u, 15u);
    data[76] = 1u;
    data[77] = 0xffu;
    data[78] = 0x93u;
    data[79] = 0u;
    data[80] = 0xffu;
    data[81] = 0xd9u;
    return CODESTREAM_SIZE;
}

static int check_tier1_packet_decode(void)
{
    static const uint8_t codeword[] = {
        0xffu, 0x00u, 0x7fu, 0x01u, 0x55u, 0xaau
    };
    uint8_t codestream[88];
    int status;

    if (make_empty_packet_codestream(codestream, sizeof(codestream)) != 82u) {
        return 0;
    }
    memmove(codestream + 86u, codestream + 80u, 2u);
    codestream[64] = 0x10u;
    write_be32(codestream + 71u, 21u);
    codestream[79] = 0xe6u;
    memcpy(codestream + 80u, codeword, sizeof(codeword));

    status = qw_jp2_validate_structure(codestream, sizeof(codestream));
    if (status != QW_STATUS_OK) {
        fprintf(stderr,
                "valid packet codeword did not reach Tier-1 successfully (%d)\n",
                status);
        return 0;
    }
    return 1;
}

static int check_packet_body_accumulation(void)
{
    uint8_t codestream[99];
    uint8_t truncated[98];
    size_t codestream_size = make_empty_packet_codestream(
        codestream, sizeof(codestream));
    size_t second_sot = 81u;

    if (codestream_size != 82u) return 0;
    write_be16(codestream + 51u, 2u);
    write_be32(codestream + 71u, 16u);
    codestream[76] = 0u;
    codestream[77] = 0xffu;
    codestream[78] = 0x93u;
    codestream[79] = 0xe1u;
    codestream[80] = 0x5au;

    codestream[second_sot] = 0xffu;
    codestream[second_sot + 1u] = 0x90u;
    write_be16(codestream + second_sot + 2u, 10u);
    write_be16(codestream + second_sot + 4u, 0u);
    write_be32(codestream + second_sot + 6u, 16u);
    codestream[second_sot + 10u] = 1u;
    codestream[second_sot + 11u] = 2u;
    codestream[second_sot + 12u] = 0xffu;
    codestream[second_sot + 13u] = 0x93u;
    codestream[second_sot + 14u] = 0xc2u;
    codestream[second_sot + 15u] = 0xa5u;
    codestream[97] = 0xffu;
    codestream[98] = 0xd9u;

    if (qw_jp2_validate_structure(codestream, sizeof(codestream)) !=
        QW_STATUS_OK) {
        fputs("valid two-layer code-block body contributions were rejected\n",
              stderr);
        return 0;
    }

    memcpy(truncated, codestream, 96u);
    write_be32(truncated + second_sot + 6u, 15u);
    truncated[96] = 0xffu;
    truncated[97] = 0xd9u;
    if (qw_jp2_validate_structure(truncated, sizeof(truncated)) !=
        QW_STATUS_ERR_FORMAT) {
        fputs("truncated code-block body contribution was accepted\n", stderr);
        return 0;
    }
    return 1;
}

static int check_ppm_ppt_marker_mutations(const uint8_t *codestream,
                                          size_t codestream_size)
{
    static const uint8_t ppm[] = {
        0xffu, 0x60u, 0x00u, 0x08u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x01u, 0x00u
    };
    static const uint8_t ppt[] = {
        0xffu, 0x61u, 0x00u, 0x04u, 0x00u, 0x00u
    };
    uint8_t minimal_codestream[82];
    size_t minimal_size = make_empty_packet_codestream(
        minimal_codestream, sizeof(minimal_codestream));
    size_t sot_offset = 65u;
    size_t base_sod_offset = 77u;
    size_t ppm_inserted_size;
    size_t ppm_sod_offset;
    size_t with_ppm_size;
    size_t ppt_inserted_size;
    size_t with_ppt_size;
    uint8_t *ppm_inserted = NULL;
    uint8_t *ppt_inserted = NULL;
    uint8_t *with_ppm = NULL;
    uint8_t *bad_ppm = NULL;
    uint8_t *with_ppt = NULL;
    uint8_t *bad_ppt = NULL;
    uint8_t *with_ppm_and_ppt = NULL;
    int passed = 0;

    (void)codestream;
    (void)codestream_size;
    if (minimal_size == 0u) return 0;

    ppm_inserted_size = minimal_size + sizeof(ppm);
    ppm_inserted = insert_bytes(minimal_codestream, minimal_size, sot_offset,
                                ppm, sizeof(ppm));
    if (!ppm_inserted) goto cleanup;
    write_be32(ppm_inserted + sot_offset + sizeof(ppm) + 6u, 14u);
    ppm_sod_offset = find_first_tile_sod(ppm_inserted, ppm_inserted_size,
                                         sot_offset + sizeof(ppm));
    if (ppm_sod_offset == ppm_inserted_size ||
        ppm_sod_offset + 3u > ppm_inserted_size)
        goto cleanup;
    with_ppm_size = ppm_inserted_size - 1u;
    with_ppm = (uint8_t *)malloc(with_ppm_size);
    if (!with_ppm) goto cleanup;
    memcpy(with_ppm, ppm_inserted, ppm_sod_offset + 2u);
    memcpy(with_ppm + ppm_sod_offset + 2u,
           ppm_inserted + ppm_sod_offset + 3u,
           ppm_inserted_size - ppm_sod_offset - 3u);
    if (qw_jp2_validate_structure(with_ppm, with_ppm_size) != QW_STATUS_OK) {
        fputs("valid PPM marker and one tile-part record were rejected\n",
              stderr);
        goto cleanup;
    }
    bad_ppm = (uint8_t *)malloc(with_ppm_size);
    if (!bad_ppm) goto cleanup;
    memcpy(bad_ppm, with_ppm, with_ppm_size);
    bad_ppm[sot_offset + 4u] = 1u;
    if (qw_jp2_validate_structure(bad_ppm, with_ppm_size) !=
        QW_STATUS_ERR_FORMAT) {
        fputs("out-of-order PPM sequence index was accepted\n", stderr);
        goto cleanup;
    }
    memcpy(bad_ppm, with_ppm, with_ppm_size);
    write_be32(bad_ppm + sot_offset + 5u, 2u);
    if (qw_jp2_validate_structure(bad_ppm, with_ppm_size) !=
        QW_STATUS_ERR_FORMAT) {
        fputs("PPM Nppm length beyond its segment was accepted\n", stderr);
        goto cleanup;
    }

    ppt_inserted_size = minimal_size + sizeof(ppt);
    ppt_inserted = insert_bytes(minimal_codestream, minimal_size,
                                base_sod_offset, ppt, sizeof(ppt));
    if (!ppt_inserted) goto cleanup;
    write_be32(ppt_inserted + sot_offset + 6u, 20u);
    with_ppt_size = ppt_inserted_size - 1u;
    with_ppt = (uint8_t *)malloc(with_ppt_size);
    if (!with_ppt) goto cleanup;
    memcpy(with_ppt, ppt_inserted,
           base_sod_offset + sizeof(ppt) + 2u);
    memcpy(with_ppt + base_sod_offset + sizeof(ppt) + 2u,
           ppt_inserted + base_sod_offset + sizeof(ppt) + 3u,
           ppt_inserted_size - base_sod_offset - sizeof(ppt) - 3u);
    if (qw_jp2_validate_structure(with_ppt, with_ppt_size) != QW_STATUS_OK) {
        fputs("valid PPT marker was rejected\n", stderr);
        goto cleanup;
    }
    bad_ppt = (uint8_t *)malloc(with_ppt_size);
    if (!bad_ppt) goto cleanup;
    memcpy(bad_ppt, with_ppt, with_ppt_size);
    bad_ppt[base_sod_offset + 4u] = 1u;
    if (qw_jp2_validate_structure(bad_ppt, with_ppt_size) !=
        QW_STATUS_ERR_FORMAT) {
        fputs("out-of-order PPT sequence index was accepted\n", stderr);
        goto cleanup;
    }

    ppm_sod_offset = find_first_tile_sod(with_ppm, with_ppm_size,
                                         sot_offset + sizeof(ppm));
    if (ppm_sod_offset == with_ppm_size) goto cleanup;
    with_ppm_and_ppt = insert_bytes(with_ppm, with_ppm_size, ppm_sod_offset,
                                    ppt, sizeof(ppt));
    if (!with_ppm_and_ppt) goto cleanup;
    write_be32(with_ppm_and_ppt + sot_offset + sizeof(ppm) + 6u, 20u);
    if (qw_jp2_validate_structure(with_ppm_and_ppt,
                                  with_ppm_size + sizeof(ppt)) !=
        QW_STATUS_ERR_FORMAT) {
        fputs("codestream containing both PPM and PPT was accepted\n",
              stderr);
        goto cleanup;
    }
    passed = 1;

cleanup:
    free(with_ppm_and_ppt);
    free(bad_ppt);
    free(with_ppt);
    free(bad_ppm);
    free(with_ppm);
    free(ppt_inserted);
    free(ppm_inserted);
    return passed;
}

static int check_poc_marker_mutations(const uint8_t *fixture_codestream,
                                      size_t fixture_codestream_size)
{
    uint8_t minimal_codestream[82];
    size_t codestream_size = make_empty_packet_codestream(
        minimal_codestream, sizeof(minimal_codestream));
    const uint8_t *codestream = minimal_codestream;
    size_t cod_offset = find_first_marker(codestream, codestream_size,
                                          0u, 0x52u);
    size_t sot_offset = find_first_marker(codestream, codestream_size,
                                         0u, 0x90u);
    size_t sod_offset;
    uint16_t component_count;
    uint16_t layer_count;
    uint16_t above_layer_end;
    uint8_t poc[11];
    uint8_t coc[11];
    uint8_t duplicate_poc[22];
    uint8_t duplicate_coc[22];
    uint8_t coc_then_clipped_poc[22];
    uint8_t *with_poc = NULL;
    uint8_t *with_coc = NULL;
    uint8_t *with_tile_poc = NULL;
    uint32_t tile_part_size;
    int passed = 0;

    (void)fixture_codestream;
    (void)fixture_codestream_size;
    if (codestream_size == 0u || cod_offset == codestream_size ||
        sot_offset == codestream_size ||
        codestream_size < 42u) {
        fputs("could not locate COD/SOT for POC marker mutations\n", stderr);
        return 0;
    }
    component_count = read_be16(codestream + 40u);
    layer_count = read_be16(codestream + cod_offset + 6u);
    if (component_count == 0u || component_count >= 257u ||
        layer_count == 0u) {
        fputs("POC marker mutation fixture is outside the supported range\n",
              stderr);
        return 0;
    }
    make_poc_segment(poc, component_count, layer_count, 1u);
    with_poc = insert_bytes(codestream, codestream_size, sot_offset,
                            poc, sizeof(poc));
    if (!with_poc ||
        qw_jp2_validate_structure(with_poc, codestream_size + sizeof(poc)) !=
            QW_STATUS_OK) {
        fputs("valid main-header POC marker was rejected\n", stderr);
        goto cleanup;
    }
    free(with_poc);
    with_poc = NULL;

    above_layer_end = layer_count == UINT16_MAX
                          ? layer_count : (uint16_t)(layer_count + 1u);
    make_poc_segment(poc, component_count, above_layer_end, 1u);
    with_poc = insert_bytes(codestream, codestream_size, sot_offset,
                            poc, sizeof(poc));
    if (!with_poc ||
        qw_jp2_validate_structure(with_poc, codestream_size + sizeof(poc)) !=
            QW_STATUS_OK) {
        fputs("main-header POC above the COD layer count was rejected\n",
              stderr);
        goto cleanup;
    }
    free(with_poc);
    with_poc = NULL;

    make_poc_segment(poc, component_count, 0u, 1u);
    with_poc = insert_bytes(codestream, codestream_size, sot_offset,
                            poc, sizeof(poc));
    if (!with_poc ||
        qw_jp2_validate_structure(with_poc, codestream_size + sizeof(poc)) !=
            QW_STATUS_ERR_FORMAT) {
        fputs("main-header POC with zero LYEpoc was accepted\n", stderr);
        goto cleanup;
    }
    free(with_poc);
    with_poc = NULL;

    make_poc_segment(poc, component_count, layer_count, 1u);
    memcpy(duplicate_poc, poc, sizeof(poc));
    memcpy(duplicate_poc + sizeof(poc), poc, sizeof(poc));
    with_poc = insert_bytes(codestream, codestream_size, sot_offset,
                            duplicate_poc, sizeof(duplicate_poc));
    if (!with_poc ||
        qw_jp2_validate_structure(with_poc,
                                  codestream_size + sizeof(duplicate_poc)) !=
            QW_STATUS_ERR_FORMAT) {
        fputs("duplicate main-header POC markers were accepted\n", stderr);
        goto cleanup;
    }
    free(with_poc);
    with_poc = NULL;

    make_coc_segment(coc, codestream, cod_offset);
    with_coc = insert_bytes(codestream, codestream_size, sot_offset,
                            coc, sizeof(coc));
    if (!with_coc ||
        qw_jp2_validate_structure(with_coc, codestream_size + sizeof(coc)) !=
            QW_STATUS_OK) {
        fputs("valid main-header COC marker was rejected\n", stderr);
        goto cleanup;
    }
    free(with_coc);
    with_coc = NULL;

    memcpy(duplicate_coc, coc, sizeof(coc));
    memcpy(duplicate_coc + sizeof(coc), coc, sizeof(coc));
    with_coc = insert_bytes(codestream, codestream_size, sot_offset,
                            duplicate_coc, sizeof(duplicate_coc));
    if (!with_coc ||
        qw_jp2_validate_structure(
            with_coc, codestream_size + sizeof(duplicate_coc)) !=
            QW_STATUS_ERR_FORMAT) {
        fputs("duplicate main-header COC markers were accepted\n", stderr);
        goto cleanup;
    }
    free(with_coc);
    with_coc = NULL;

    make_poc_segment(poc, component_count, layer_count, 2u);
    memcpy(coc_then_clipped_poc, coc, sizeof(coc));
    memcpy(coc_then_clipped_poc + sizeof(coc), poc, sizeof(poc));
    with_coc = insert_bytes(codestream, codestream_size, sot_offset,
                            coc_then_clipped_poc,
                            sizeof(coc_then_clipped_poc));
    if (!with_coc ||
        qw_jp2_validate_structure(
            with_coc, codestream_size + sizeof(coc_then_clipped_poc)) !=
            QW_STATUS_OK) {
        fputs("main-header POC was not clipped to the COC resolution range\n",
              stderr);
        goto cleanup;
    }
    free(with_coc);
    with_coc = NULL;

    sod_offset = find_first_tile_sod(codestream, codestream_size, sot_offset);
    if (sod_offset == codestream_size) {
        fputs("could not locate SOD for tile-header POC mutation\n", stderr);
        goto cleanup;
    }
    make_poc_segment(poc, component_count, 1u, 1u);
    with_tile_poc = insert_bytes(codestream, codestream_size, sod_offset,
                                 poc, sizeof(poc));
    if (!with_tile_poc) goto cleanup;
    tile_part_size = read_be32(codestream + sot_offset + 6u);
    if (tile_part_size != 0u) {
        if (tile_part_size > UINT32_MAX - sizeof(poc)) goto cleanup;
        write_be32(with_tile_poc + sot_offset + 6u,
                   tile_part_size + (uint32_t)sizeof(poc));
    }
    if (qw_jp2_validate_structure(with_tile_poc,
                                  codestream_size + sizeof(poc)) !=
        QW_STATUS_OK) {
        fputs("valid tile-header POC marker was rejected\n", stderr);
        goto cleanup;
    }
    free(with_tile_poc);
    with_tile_poc = NULL;

    memcpy(duplicate_poc, poc, sizeof(poc));
    memcpy(duplicate_poc + sizeof(poc), poc, sizeof(poc));
    with_tile_poc = insert_bytes(codestream, codestream_size, sod_offset,
                                 duplicate_poc, sizeof(duplicate_poc));
    if (!with_tile_poc) goto cleanup;
    if (tile_part_size != 0u) {
        if (tile_part_size > UINT32_MAX - sizeof(duplicate_poc)) goto cleanup;
        write_be32(with_tile_poc + sot_offset + 6u,
                   tile_part_size + (uint32_t)sizeof(duplicate_poc));
    }
    if (qw_jp2_validate_structure(with_tile_poc,
                                  codestream_size + sizeof(duplicate_poc)) !=
        QW_STATUS_ERR_FORMAT) {
        fputs("duplicate tile-header POC markers were accepted\n", stderr);
        goto cleanup;
    }
    if (!check_tile_coding_state_across_parts(codestream, codestream_size)) {
        goto cleanup;
    }
    if (!check_later_tile_poc_packet_schedule()) goto cleanup;
    passed = 1;

cleanup:
    free(with_tile_poc);
    free(with_coc);
    free(with_poc);
    return passed;
}

static size_t find_first_marker(const uint8_t *data,
                                size_t size,
                                size_t start,
                                uint8_t marker)
{
    for (size_t i = start; i + 1u < size; ++i) {
        if (data[i] == 0xffu && data[i + 1u] == marker) return i;
    }
    return size;
}

static size_t find_first_tile_sod(const uint8_t *data,
                                  size_t size,
                                  size_t sot_offset)
{
    size_t position;
    if (sot_offset > size || size - sot_offset < 12u) return size;
    position = sot_offset + 12u;
    while (position < size) {
        uint8_t marker;
        size_t marker_offset = position;
        while (position < size && data[position] == 0xffu) ++position;
        if (position >= size || marker_offset == position ||
            data[position] == 0x00u) {
            return size;
        }
        marker = data[position++];
        if (marker == 0x93u) return marker_offset;
        if (marker == 0x90u || marker == 0xd9u || marker == 0x4fu ||
            size - position < 2u) {
            return size;
        }
        {
            uint16_t length = (uint16_t)(((uint16_t)data[position] << 8) |
                                         (uint16_t)data[position + 1u]);
            if (length < 2u || (size_t)length > size - position) return size;
            position += (size_t)length;
        }
    }
    return size;
}

static uint64_t fnv1a64(const uint8_t *data, size_t size)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0u; i < size; ++i) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int check_fixture(const char *name)
{
    static const uint64_t expected_hash = UINT64_C(5470596801655083028);
    uint8_t *data = NULL;
    size_t size = 0u;
    QwImage image = {0};
    int rc;
    if (!read_fixture(name, &data, &size)) {
        fprintf(stderr, "could not load JP2 fixture: %s\n", name);
        return 0;
    }
    rc = qw_jp2_validate_structure(data, size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "%s structure status was %d\n", name, rc);
        free(data);
        return 0;
    }
    rc = qw_image_decode(data, size, &image);
    free(data);
    if (rc != QW_IMAGE_OK || image.width != 31u || image.height != 32u ||
        image.format != QW_IMAGE_PIXEL_RGB24 || image.size != 2976u ||
        fnv1a64(image.pixels, image.size) != expected_hash) {
        fprintf(stderr, "%s decode parity failed (status=%d, %ux%u, size=%zu)\n",
                name, rc, image.width, image.height, image.size);
        qw_image_free(&image);
        return 0;
    }
    qw_image_free(&image);
    return 1;
}

static int check_large_fixture(const char *name, uint64_t expected_hash)
{
    uint8_t *data = NULL;
    size_t size = 0u;
    QwImage image = {0};
    uint64_t actual_hash;
    int rc;
    if (!read_fixture(name, &data, &size)) {
        fprintf(stderr, "could not load large JP2 fixture: %s\n", name);
        return 0;
    }
    rc = qw_jp2_validate_structure(data, size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "%s structure status was %d\n", name, rc);
        free(data);
        return 0;
    }
    rc = qw_image_decode(data, size, &image);
    free(data);
    actual_hash = image.pixels ? fnv1a64(image.pixels, image.size) : 0u;
    if (rc != QW_IMAGE_OK || image.width != 2717u || image.height != 3701u ||
        image.format != QW_IMAGE_PIXEL_RGB24 || image.stride != 8151u ||
        image.size != 30166851u || !actual_hash ||
        (expected_hash && actual_hash != expected_hash)) {
        fprintf(stderr, "%s large decode failed (status=%d, %ux%u, size=%zu, hash=%llu)\n",
                name, rc, image.width, image.height, image.size,
                (unsigned long long)actual_hash);
        qw_image_free(&image);
        return 0;
    }
    qw_image_free(&image);
    return 1;
}

typedef struct TestOpenJpegMemoryStream {
    const uint8_t *data;
    size_t size;
    size_t position;
} TestOpenJpegMemoryStream;

static OPJ_SIZE_T test_openjpeg_read(void *buffer,
                                     OPJ_SIZE_T byte_count,
                                     void *user_data)
{
    TestOpenJpegMemoryStream *memory =
        (TestOpenJpegMemoryStream *)user_data;
    size_t available;
    size_t count;
    if (!memory || !buffer || memory->position > memory->size) {
        return (OPJ_SIZE_T)-1;
    }
    if (memory->position == memory->size) return (OPJ_SIZE_T)-1;
    available = memory->size - memory->position;
    count = byte_count > (OPJ_SIZE_T)available
        ? available : (size_t)byte_count;
    if (!count) return 0;
    memcpy(buffer, memory->data + memory->position, count);
    memory->position += count;
    return (OPJ_SIZE_T)count;
}

static OPJ_OFF_T test_openjpeg_skip(OPJ_OFF_T byte_count, void *user_data)
{
    TestOpenJpegMemoryStream *memory =
        (TestOpenJpegMemoryStream *)user_data;
    uint64_t position;
    if (!memory || byte_count < 0 || memory->position > memory->size) {
        return -1;
    }
    position = (uint64_t)memory->position + (uint64_t)byte_count;
    if (position > (uint64_t)memory->size) return -1;
    memory->position = (size_t)position;
    return byte_count;
}

static OPJ_BOOL test_openjpeg_seek(OPJ_OFF_T position, void *user_data)
{
    TestOpenJpegMemoryStream *memory =
        (TestOpenJpegMemoryStream *)user_data;
    if (!memory || position < 0 ||
        (uint64_t)position > (uint64_t)memory->size) {
        return OPJ_FALSE;
    }
    memory->position = (size_t)position;
    return OPJ_TRUE;
}

static void test_openjpeg_quiet_message(const char *message, void *user_data)
{
    (void)message;
    (void)user_data;
}

static int check_balloon_rgb_quality(const char *fixture_name,
                                     OPJ_CODEC_FORMAT reference_format,
                                     uint64_t expected_hash,
                                     int decode_internal_directly)
{
    uint8_t *data = NULL;
    size_t size = 0u;
    QwImage decoded = {0};
    QwPageImage internal_page = {0};
    const uint8_t *actual_pixels = NULL;
    size_t actual_size = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    TestOpenJpegMemoryStream memory = {0};
    opj_stream_t *stream = NULL;
    opj_codec_t *codec = NULL;
    opj_image_t *reference = NULL;
    opj_dparameters_t parameters;
    size_t pixel_count;
    size_t changed_bytes = 0u;
    uint64_t internal_hash;
    unsigned maximum_error = 0u;
    int passed = 0;
    int rc;

    if (!fixture_name || !read_fixture(fixture_name, &data, &size)) {
        fprintf(stderr, "could not load balloon quality fixture: %s\n",
                fixture_name ? fixture_name : "(null)");
        goto cleanup;
    }
    if (decode_internal_directly) {
        rc = qw_jp2_decode_internal_gray8(data, size, &internal_page);
        if (rc != QW_STATUS_OK ||
            internal_page.kind != QW_PAGE_IMAGE_RGB24 ||
            internal_page.width != 2717u || internal_page.height != 3701u ||
            internal_page.size != 30166851u || !internal_page.data) {
            fprintf(stderr,
                    "%s direct internal RGB decode failed "
                    "(status=%d, kind=%d, %ux%u, size=%zu)\n",
                    fixture_name, rc, internal_page.kind,
                    internal_page.width, internal_page.height,
                    internal_page.size);
            goto cleanup;
        }
        actual_pixels = internal_page.data;
        actual_size = internal_page.size;
        width = internal_page.width;
        height = internal_page.height;
    } else {
        rc = qw_image_decode(data, size, &decoded);
        if (rc != QW_IMAGE_OK || decoded.width != 2717u ||
            decoded.height != 3701u ||
            decoded.format != QW_IMAGE_PIXEL_RGB24 ||
            decoded.stride != 8151u || decoded.size != 30166851u ||
            !decoded.pixels) {
            fprintf(stderr,
                    "%s public RGB decode has an unexpected layout "
                    "(status=%d, %ux%u, size=%zu)\n",
                    fixture_name, rc, decoded.width, decoded.height,
                    decoded.size);
            goto cleanup;
        }
        actual_pixels = decoded.pixels;
        actual_size = decoded.size;
        width = decoded.width;
        height = decoded.height;
    }
    internal_hash = fnv1a64(actual_pixels, actual_size);
    if (expected_hash && internal_hash != expected_hash) {
        fprintf(stderr,
                "%s internal hash mismatch (actual=%llu, expected=%llu)\n",
                fixture_name, (unsigned long long)internal_hash,
                (unsigned long long)expected_hash);
        goto cleanup;
    }

    memory.data = data;
    memory.size = size;
    stream = opj_stream_default_create(OPJ_TRUE);
    codec = opj_create_decompress(reference_format);
    if (!stream || !codec) goto reference_decode_failed;
    opj_stream_set_user_data(stream, &memory, NULL);
    opj_stream_set_user_data_length(stream, (OPJ_UINT64)size);
    opj_stream_set_read_function(stream, test_openjpeg_read);
    opj_stream_set_skip_function(stream, test_openjpeg_skip);
    opj_stream_set_seek_function(stream, test_openjpeg_seek);
    opj_set_error_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_warning_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_info_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_default_decoder_parameters(&parameters);
    if (!opj_setup_decoder(codec, &parameters) ||
        !opj_read_header(stream, codec, &reference) || !reference ||
        !opj_decode(codec, stream, reference) ||
        !opj_end_decompress(codec, stream)) {
        goto reference_decode_failed;
    }
    if (reference->x1 <= reference->x0 || reference->y1 <= reference->y0 ||
        reference->x1 - reference->x0 != width ||
        reference->y1 - reference->y0 != height ||
        reference->numcomps != 3u) {
        fprintf(stderr, "%s OpenJPEG reference has an unexpected layout\n",
                fixture_name);
        goto cleanup;
    }
    for (OPJ_UINT32 component = 0u; component < 3u; ++component) {
        const opj_image_comp_t *plane = &reference->comps[component];
        if (!plane->data || plane->w != width || plane->h != height ||
            plane->prec != 8u || plane->sgnd) {
            fprintf(stderr,
                    "%s OpenJPEG reference has an unsupported RGB plane\n",
                    fixture_name);
            goto cleanup;
        }
    }

    pixel_count = (size_t)width * (size_t)height;
    for (size_t pixel = 0u; pixel < pixel_count; ++pixel) {
        for (OPJ_UINT32 component = 0u; component < 3u; ++component) {
            int sample = reference->comps[component].data[pixel];
            int actual = actual_pixels[pixel * 3u + component];
            unsigned error;
            if (sample < 0 || sample > 255) {
                fprintf(stderr,
                        "%s OpenJPEG reference sample is out of range\n",
                        fixture_name);
                goto cleanup;
            }
            error = (unsigned)(actual > sample ? actual - sample
                                                  : sample - actual);
            if (error) ++changed_bytes;
            if (error > maximum_error) maximum_error = error;
        }
    }
    if (maximum_error > 1u) {
        fprintf(stderr,
                "%s exceeds the one-LSB OpenJPEG quality limit "
                "(max error %u, changed bytes %zu)\n",
                fixture_name, maximum_error, changed_bytes);
        goto cleanup;
    }
    printf("%s RGB vs OpenJPEG: max error %u LSB, "
           "%zu/%zu samples changed; internal FNV-1a64 %llu\n",
           fixture_name,
           maximum_error, changed_bytes, pixel_count * 3u,
           (unsigned long long)internal_hash);
    passed = 1;
    goto cleanup;

reference_decode_failed:
    fprintf(stderr, "could not decode %s with the OpenJPEG reference\n",
            fixture_name);

cleanup:
    if (reference) opj_image_destroy(reference);
    if (codec) opj_destroy_codec(codec);
    if (stream) opj_stream_destroy(stream);
    qw_image_free(&decoded);
    qw_image_release_page(&internal_page);
    free(data);
    return passed;
}

static int check_balloon_internal_public_parity(const char *fixture_name)
{
    uint8_t *data = NULL;
    size_t size = 0u;
    QwPageImage internal_page = {0};
    QwImage public_image = {0};
    int passed = 0;
    int rc;

    if (!fixture_name || !read_fixture(fixture_name, &data, &size)) {
        fprintf(stderr, "could not load balloon parity fixture: %s\n",
                fixture_name ? fixture_name : "(null)");
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(data, size, &internal_page);
    if (rc != QW_STATUS_OK || internal_page.kind != QW_PAGE_IMAGE_RGB24 ||
        internal_page.width != 2717u || internal_page.height != 3701u ||
        internal_page.size != 30166851u || !internal_page.data) {
        fprintf(stderr,
                "%s direct internal RGB decode failed before public parity "
                "check (status=%d, kind=%d, %ux%u, size=%zu)\n",
                fixture_name, rc, internal_page.kind,
                internal_page.width, internal_page.height,
                internal_page.size);
        goto cleanup;
    }
    rc = qw_image_decode(data, size, &public_image);
    if (rc != QW_IMAGE_OK || public_image.width != 2717u ||
        public_image.height != 3701u ||
        public_image.format != QW_IMAGE_PIXEL_RGB24 ||
        public_image.stride != 8151u ||
        public_image.size != internal_page.size || !public_image.pixels) {
        fprintf(stderr,
                "%s public RGB decode has an unexpected layout "
                "(status=%d, %ux%u, size=%zu)\n",
                fixture_name, rc, public_image.width, public_image.height,
                public_image.size);
        goto cleanup;
    }
    if (memcmp(internal_page.data, public_image.pixels,
               internal_page.size) != 0) {
        fprintf(stderr,
                "%s direct internal and public RGB output differ\n",
                fixture_name);
        goto cleanup;
    }
    printf("%s direct internal RGB24 and public qw_image_decode match "
           "byte-for-byte (%zu bytes)\n",
           fixture_name, internal_page.size);
    passed = 1;

cleanup:
    qw_image_free(&public_image);
    qw_image_release_page(&internal_page);
    free(data);
    return passed;
}

static int check_malformed_inputs(void)
{
    uint8_t *container = NULL;
    uint8_t *codestream = NULL;
    size_t container_size = 0u;
    size_t codestream_size = 0u;
    uint8_t *mutated = NULL;
    uint8_t *expanded = NULL;
    size_t sot_offset;
    size_t sod_offset;
    int passed = 0;

    if (!read_fixture("rgb_u1.jp2", &container, &container_size) ||
        !read_fixture("rgb_u1.j2k", &codestream, &codestream_size)) {
        fprintf(stderr, "could not load JP2 mutation fixtures\n");
        goto cleanup;
    }
    mutated = (uint8_t *)malloc(container_size);
    if (!mutated) goto cleanup;

    if (qw_jp2_validate_structure(container, 11u) != QW_STATUS_ERR_FORMAT) {
        fprintf(stderr, "truncated signature was not classified as format\n");
        goto cleanup;
    }
    {
        QwImage image = {0};
        if (qw_image_decode(container, 11u, &image) != QW_IMAGE_ERR_FORMAT ||
            image.pixels || image.size || image.width || image.height) {
            fprintf(stderr, "public truncated-signature result was unstable\n");
            qw_image_free(&image);
            goto cleanup;
        }
    }
    memcpy(mutated, container, container_size);
    write_be32(mutated + 12u, UINT32_C(0x7fffffff));
    if (qw_jp2_validate_structure(mutated, container_size) !=
        QW_STATUS_ERR_FORMAT) {
        fprintf(stderr, "invalid top-level box length was accepted\n");
        goto cleanup;
    }
    memcpy(mutated, container, container_size);
    write_be32(mutated + 32u, UINT32_C(0x7fffffff));
    if (qw_jp2_validate_structure(mutated, container_size) !=
        QW_STATUS_ERR_FORMAT) {
        fprintf(stderr, "invalid jp2h nesting length was accepted\n");
        goto cleanup;
    }
    memcpy(mutated, container, container_size);
    memcpy(mutated + 20u, "jpx ", 4u);
    if (qw_jp2_validate_structure(mutated, container_size) !=
        QW_STATUS_ERR_UNSUPPORTED) {
        fprintf(stderr, "JPX brand did not return stable unsupported\n");
        goto cleanup;
    }
    {
        QwImage image = {0};
        if (qw_image_decode(mutated, container_size, &image) !=
                QW_IMAGE_ERR_UNSUPPORTED ||
            image.pixels || image.size || image.width || image.height) {
            fprintf(stderr, "public JPX result was unstable\n");
            qw_image_free(&image);
            goto cleanup;
        }
    }
    memcpy(mutated, container, container_size);
    write_be32(mutated + 52u, 30u);
    if (qw_jp2_validate_structure(mutated, container_size) !=
        QW_STATUS_ERR_FORMAT) {
        fprintf(stderr, "JP2 ihdr/SIZ mismatch was accepted\n");
        goto cleanup;
    }
    if (qw_jp2_validate_structure(codestream, 20u) != QW_STATUS_ERR_FORMAT) {
        fprintf(stderr, "truncated raw codestream was not rejected\n");
        goto cleanup;
    }
    free(mutated);
    mutated = (uint8_t *)malloc(codestream_size);
    if (!mutated) goto cleanup;
    memcpy(mutated, codestream, codestream_size);
    mutated[4] = 0u;
    mutated[5] = 1u;
    if (qw_jp2_validate_structure(mutated, codestream_size) !=
        QW_STATUS_ERR_FORMAT) {
        fprintf(stderr, "invalid SIZ length was accepted\n");
        goto cleanup;
    }
    memcpy(mutated, codestream, codestream_size);
    mutated[42] = 32u;
    if (qw_jp2_validate_structure(mutated, codestream_size) !=
        QW_STATUS_ERR_UNSUPPORTED) {
        fprintf(stderr, "unsupported component precision was not classified\n");
        goto cleanup;
    }
    sot_offset = find_first_marker(codestream, codestream_size, 0u, 0x90u);
    if (sot_offset == codestream_size ||
        codestream_size > (size_t)UINT32_MAX - 1u) {
        fprintf(stderr, "could not locate bounded fixture SOT marker\n");
        goto cleanup;
    }
    memcpy(mutated, codestream, codestream_size);
    write_be32(mutated + sot_offset + 6u,
               (uint32_t)(codestream_size + 1u));
    if (qw_jp2_validate_structure(mutated, codestream_size) !=
        QW_STATUS_ERR_FORMAT) {
        fprintf(stderr, "tile-part length beyond the codestream was accepted\n");
        goto cleanup;
    }
    sod_offset = find_first_tile_sod(codestream, codestream_size, sot_offset);
    if (sod_offset == codestream_size) {
        fprintf(stderr, "could not locate bounded fixture SOD marker\n");
        goto cleanup;
    }
    if (!check_signaled_precinct_sizes(codestream, codestream_size)) {
        goto cleanup;
    }
    if (!check_ppm_ppt_marker_mutations(codestream, codestream_size)) {
        goto cleanup;
    }
    if (!check_poc_marker_mutations(codestream, codestream_size)) goto cleanup;
    if (codestream_size > SIZE_MAX - 2u) goto cleanup;
    expanded = (uint8_t *)malloc(codestream_size + 2u);
    if (!expanded) goto cleanup;
    memcpy(expanded, codestream, sot_offset);
    expanded[sot_offset] = 0xffu;
    expanded[sot_offset + 1u] = 0x30u;
    memcpy(expanded + sot_offset + 2u, codestream + sot_offset,
           codestream_size - sot_offset);
    if (qw_jp2_validate_structure(expanded, codestream_size + 2u) !=
        QW_STATUS_OK) {
        fprintf(stderr, "reserved no-parameter main-header marker was rejected\n");
        goto cleanup;
    }
    {
        uint32_t tile_part_size =
            ((uint32_t)codestream[sot_offset + 6u] << 24) |
            ((uint32_t)codestream[sot_offset + 7u] << 16) |
            ((uint32_t)codestream[sot_offset + 8u] << 8) |
            (uint32_t)codestream[sot_offset + 9u];
        if (tile_part_size > UINT32_MAX - 2u) goto cleanup;
        memcpy(expanded, codestream, sod_offset);
        expanded[sod_offset] = 0xffu;
        expanded[sod_offset + 1u] = 0x30u;
        memcpy(expanded + sod_offset + 2u, codestream + sod_offset,
               codestream_size - sod_offset);
        write_be32(expanded + sot_offset + 6u, tile_part_size + 2u);
    }
    if (qw_jp2_validate_structure(expanded, codestream_size + 2u) !=
        QW_STATUS_OK) {
        fprintf(stderr, "reserved no-parameter tile-header marker was rejected\n");
        goto cleanup;
    }
    memcpy(mutated, codestream, codestream_size);
    write_be32(mutated + sot_offset + 6u, 0u);
    if (qw_jp2_validate_structure(mutated, codestream_size) != QW_STATUS_OK) {
        fprintf(stderr, "final tile-part with zero Psot was rejected\n");
        goto cleanup;
    }
    if (sod_offset + 3u >= codestream_size - 2u) goto cleanup;
    memcpy(mutated, codestream, codestream_size);
    write_be32(mutated + sot_offset + 6u, 0u);
    mutated[sod_offset + 2u] = 0xffu;
    mutated[sod_offset + 3u] = 0xd9u;
    if (qw_jp2_validate_structure(mutated, codestream_size) !=
        QW_STATUS_ERR_FORMAT) {
        fprintf(stderr, "early EOC in a zero-Psot tile-part was accepted\n");
        goto cleanup;
    }
    {
        static const uint8_t following_tile_part[14] = {
            0xffu, 0x90u, 0x00u, 0x0au, 0x00u, 0x00u,
            0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u,
            0xffu, 0x93u
        };
        uint8_t *larger;
        size_t expanded_size;
        if (codestream_size > SIZE_MAX - sizeof(following_tile_part)) {
            goto cleanup;
        }
        expanded_size = codestream_size + sizeof(following_tile_part);
        larger = (uint8_t *)realloc(expanded, expanded_size);
        if (!larger) goto cleanup;
        expanded = larger;
        memcpy(expanded, codestream, codestream_size - 2u);
        memcpy(expanded + codestream_size - 2u, following_tile_part,
               sizeof(following_tile_part));
        memcpy(expanded + codestream_size - 2u + sizeof(following_tile_part),
               codestream + codestream_size - 2u, 2u);
        write_be32(expanded + sot_offset + 6u, 0u);
        if (qw_jp2_validate_structure(expanded, expanded_size) !=
            QW_STATUS_ERR_FORMAT) {
            fprintf(stderr, "SOT after zero-Psot tile-part was accepted\n");
            goto cleanup;
        }
    }
    memcpy(mutated, codestream, codestream_size);
    mutated[sod_offset + 1u] = 0x92u;
    if (qw_jp2_validate_structure(mutated, codestream_size) !=
        QW_STATUS_ERR_FORMAT) {
        fprintf(stderr, "tile-part without SOD was accepted\n");
        goto cleanup;
    }
    if (codestream_size < 2u || codestream[codestream_size - 2u] != 0xffu ||
        codestream[codestream_size - 1u] != 0xd9u ||
        qw_jp2_validate_structure(codestream, codestream_size - 2u) !=
            QW_STATUS_ERR_FORMAT) {
        fprintf(stderr, "missing final EOC was not rejected\n");
        goto cleanup;
    }
    passed = 1;

cleanup:
    free(expanded);
    free(mutated);
    free(container);
    free(codestream);
    return passed;
}

static int check_internal_reversible_gray8_decode(void)
{
    QwPageImage page = {0};
    int rc = qw_jp2_decode_internal_gray8(
        reversible_gray8_fixture, sizeof(reversible_gray8_fixture), &page);
    int passed = rc == QW_STATUS_OK && page.data && page.size == 1u &&
                 page.kind == QW_PAGE_IMAGE_GRAY8 && page.width == 1u &&
                 page.height == 1u && page.data[0] == 0u;
    if (!passed) {
        fprintf(stderr,
                "Internal reversible gray JP2 decode failed: rc=%d, "
                "kind=%d, dimensions=%ux%u, size=%zu, pixel=%u\n",
                rc, page.kind, page.width, page.height, page.size,
                page.data && page.size ? (unsigned)page.data[0] : 0u);
    }
    qw_image_release_page(&page);
    if (!passed) return 0;

    rc = qw_jp2_decode_page(reversible_gray8_fixture,
                            sizeof(reversible_gray8_fixture), &page);
    passed = rc == QW_STATUS_OK && page.data && page.size == 1u &&
             page.kind == QW_PAGE_IMAGE_GRAY8 && page.width == 1u &&
             page.height == 1u && page.data[0] == 0u;
    if (!passed) {
        fprintf(stderr,
                "Dispatched reversible gray JP2 decode failed: rc=%d, "
                "kind=%d, dimensions=%ux%u, size=%zu, pixel=%u\n",
                rc, page.kind, page.width, page.height, page.size,
                page.data && page.size ? (unsigned)page.data[0] : 0u);
    }
    qw_image_release_page(&page);
    return passed;
}

static int check_internal_irreversible_gray8_decode(void)
{
    const size_t expected_width = 32u;
    const size_t expected_height = 24u;
    static const struct {
        const char *jp2;
        const char *reference;
    } fixtures[] = {
        {"gray97_openjpeg_32x24.jp2", "gray97_openjpeg_32x24.gray"},
        {"gray97_openjpeg_rate20_32x24.jp2",
         "gray97_openjpeg_rate20_32x24.gray"},
    };

    for (size_t fixture = 0u;
         fixture < sizeof(fixtures) / sizeof(fixtures[0]); ++fixture) {
        uint8_t *jp2 = NULL;
        size_t jp2_size = 0u;
        uint8_t *reference = NULL;
        size_t reference_size = 0u;
        QwPageImage page = {0};
        int passed = 0;

        if (!read_fixture(fixtures[fixture].jp2, &jp2, &jp2_size) ||
            !read_fixture(fixtures[fixture].reference, &reference,
                          &reference_size)) {
            fprintf(stderr,
                    "Unable to read irreversible gray OpenJPEG fixture %s\n",
                    fixtures[fixture].jp2);
            free(reference);
            free(jp2);
            return 0;
        }
        if (reference_size != expected_width * expected_height) {
            fprintf(stderr,
                    "Irreversible gray reference fixture %s has an "
                    "unexpected size\n",
                    fixtures[fixture].reference);
            free(reference);
            free(jp2);
            return 0;
        }

        for (unsigned dispatch = 0u; dispatch < 2u; ++dispatch) {
            int rc = dispatch == 0u
                ? qw_jp2_decode_internal_gray8(jp2, jp2_size, &page)
                : qw_jp2_decode_page(jp2, jp2_size, &page);
            passed = rc == QW_STATUS_OK && page.data &&
                     page.kind == QW_PAGE_IMAGE_GRAY8 &&
                     page.width == expected_width &&
                     page.height == expected_height &&
                     page.size == reference_size &&
                     memcmp(page.data, reference, reference_size) == 0;
            if (!passed) {
                fprintf(stderr,
                        "%s irreversible gray JP2 reference parity failed "
                        "for %s: rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                        dispatch == 0u ? "Internal" : "Dispatched",
                        fixtures[fixture].jp2, rc, page.kind, page.width,
                        page.height, page.size);
                if (rc == QW_STATUS_OK && page.data &&
                    page.size == reference_size) {
                    size_t first_mismatch = reference_size;
                    size_t mismatch_count = 0u;
                    unsigned maximum_error = 0u;
                    for (size_t i = 0u; i < reference_size; ++i) {
                        unsigned actual = page.data[i];
                        unsigned expected = reference[i];
                        unsigned error = actual > expected
                            ? actual - expected : expected - actual;
                        if (error != 0u) {
                            if (first_mismatch == reference_size) {
                                first_mismatch = i;
                            }
                            ++mismatch_count;
                            if (error > maximum_error) maximum_error = error;
                        }
                    }
                    if (first_mismatch < reference_size) {
                        fprintf(stderr,
                                "First mismatch at (%zu,%zu): expected=%u "
                                "actual=%u; %zu pixels differ, max error=%u\n",
                                first_mismatch % expected_width,
                                first_mismatch / expected_width,
                                (unsigned)reference[first_mismatch],
                                (unsigned)page.data[first_mismatch],
                                mismatch_count, maximum_error);
                    }
                }
            }
            qw_image_release_page(&page);
            if (!passed) break;
        }

        qw_image_release_page(&page);
        free(reference);
        free(jp2);
        if (!passed) return 0;
    }

    return 1;
}

static size_t make_t803_gray8_jp2(const uint8_t *codestream,
                                  size_t codestream_size,
                                  uint32_t width,
                                  uint32_t height,
                                  uint8_t *output,
                                  size_t output_capacity)
{
    static const uint8_t signature[12] = {
        0x00u, 0x00u, 0x00u, 0x0cu, 0x6au, 0x50u, 0x20u, 0x20u,
        0x0du, 0x0au, 0x87u, 0x0au
    };
    size_t total_size;

    if (!codestream || !codestream_size || !width || !height || !output ||
        codestream_size > (size_t)UINT32_MAX - 8u ||
        codestream_size > SIZE_MAX - 85u) {
        return 0u;
    }
    total_size = 85u + codestream_size;
    if (output_capacity < total_size) return 0u;
    memset(output, 0, total_size);
    memcpy(output, signature, sizeof(signature));

    write_be32(output + 12u, 20u);
    memcpy(output + 16u, "ftyp", 4u);
    write_be32(output + 20u, UINT32_C(0x6a703220));
    write_be32(output + 28u, UINT32_C(0x6a703220));

    write_be32(output + 32u, 45u);
    memcpy(output + 36u, "jp2h", 4u);
    write_be32(output + 40u, 22u);
    memcpy(output + 44u, "ihdr", 4u);
    write_be32(output + 48u, height);
    write_be32(output + 52u, width);
    write_be16(output + 56u, 1u);
    output[58] = 7u;
    output[59] = 7u;
    write_be32(output + 62u, 15u);
    memcpy(output + 66u, "colr", 4u);
    output[70] = 1u;
    write_be32(output + 73u, 17u);

    write_be32(output + 77u, (uint32_t)(codestream_size + 8u));
    memcpy(output + 81u, "jp2c", 4u);
    memcpy(output + 85u, codestream, codestream_size);
    return total_size;
}

static size_t make_t803_signed4_gray_jp2(const uint8_t *codestream,
                                         size_t codestream_size,
                                         uint32_t width,
                                         uint32_t height,
                                         uint8_t *output,
                                         size_t output_capacity)
{
    size_t size = make_t803_gray8_jp2(
        codestream, codestream_size, width, height, output, output_capacity);
    if (size != 0u) output[58] = 0x83u;
    return size;
}

static uint8_t *make_t803_quantization_override_codestream(
    const uint8_t *codestream,
    size_t codestream_size,
    uint8_t marker,
    int tile_header,
    size_t *out_size)
{
    size_t qcd_offset;
    size_t sot_offset;
    size_t insert_offset;
    size_t qcd_payload_size;
    size_t override_payload_size;
    size_t segment_size;
    size_t quantization_payload_offset;
    uint16_t qcd_marker_length;
    uint8_t quantization_style;
    uint32_t tile_part_length = 0u;
    uint8_t *segment = NULL;
    uint8_t *result = NULL;

    if (!codestream || !out_size ||
        (marker != 0x5cu && marker != 0x5du)) {
        return NULL;
    }
    *out_size = 0u;
    qcd_offset = find_first_marker(codestream, codestream_size, 0u, 0x5cu);
    sot_offset = find_first_marker(codestream, codestream_size, 0u, 0x90u);
    if (qcd_offset >= sot_offset || sot_offset > codestream_size ||
        codestream_size - qcd_offset < 4u) {
        return NULL;
    }
    qcd_marker_length = read_be16(codestream + qcd_offset + 2u);
    if (qcd_marker_length < 3u ||
        (size_t)qcd_marker_length > codestream_size - qcd_offset - 2u ||
        (size_t)qcd_marker_length > sot_offset - qcd_offset - 2u) {
        return NULL;
    }
    qcd_payload_size = (size_t)qcd_marker_length - 2u;
    override_payload_size = qcd_payload_size + (marker == 0x5du ? 1u : 0u);
    if (override_payload_size > (size_t)UINT16_MAX - 2u) return NULL;
    segment_size = 4u + override_payload_size;
    segment = (uint8_t *)malloc(segment_size);
    if (!segment) return NULL;
    segment[0] = 0xffu;
    segment[1] = marker;
    write_be16(segment + 2u, (uint16_t)(override_payload_size + 2u));
    if (marker == 0x5du) segment[4] = 0u;
    quantization_payload_offset = 4u + (marker == 0x5du ? 1u : 0u);
    memcpy(segment + quantization_payload_offset,
           codestream + qcd_offset + 4u, qcd_payload_size);
    quantization_style =
        (uint8_t)(segment[quantization_payload_offset] & 0x1fu);
    if (qcd_payload_size < 3u || (qcd_payload_size & 1u) == 0u ||
        (quantization_style != 1u && quantization_style != 2u) ||
        (quantization_style == 1u && qcd_payload_size != 3u)) {
        goto cleanup;
    }
    for (size_t step_offset = 1u; step_offset < qcd_payload_size;
         step_offset += 2u) {
        uint16_t step_size = read_be16(
            segment + quantization_payload_offset + step_offset);
        write_be16(segment + quantization_payload_offset + step_offset,
                   (uint16_t)(step_size & 0xf800u));
        if (quantization_style == 1u) break;
    }

    if (tile_header) {
        size_t sod_offset = find_first_tile_sod(
            codestream, codestream_size, sot_offset);
        if (sod_offset >= codestream_size ||
            codestream_size - sot_offset < 12u) {
            goto cleanup;
        }
        insert_offset = sod_offset;
        tile_part_length = read_be32(codestream + sot_offset + 6u);
        if (tile_part_length != 0u &&
            tile_part_length > UINT32_MAX - (uint32_t)segment_size) {
            goto cleanup;
        }
    } else {
        insert_offset = sot_offset;
    }
    result = insert_bytes(codestream, codestream_size, insert_offset,
                          segment, segment_size);
    if (!result) goto cleanup;
    if (tile_header && tile_part_length != 0u) {
        write_be32(result + sot_offset + 6u,
                   tile_part_length + (uint32_t)segment_size);
    }
    *out_size = codestream_size + segment_size;

cleanup:
    free(segment);
    return result;
}

static uint8_t *make_t803_nonzero_origin_codestream(
    const uint8_t *codestream,
    size_t codestream_size,
    uint32_t origin_offset,
    size_t *out_size)
{
    size_t siz_offset;
    uint16_t siz_length;
    uint32_t x_size;
    uint32_t y_size;
    uint32_t x_origin;
    uint32_t y_origin;
    uint32_t width;
    uint32_t height;
    uint8_t *result;

    if (!codestream || !out_size || origin_offset == 0u) return NULL;
    *out_size = 0u;
    siz_offset = find_first_marker(codestream, codestream_size, 0u, 0x51u);
    if (siz_offset > codestream_size ||
        codestream_size - siz_offset < 6u ||
        codestream[siz_offset] != 0xffu ||
        codestream[siz_offset + 1u] != 0x51u) {
        return NULL;
    }
    siz_length = read_be16(codestream + siz_offset + 2u);
    if (siz_length < 38u ||
        (size_t)siz_length > codestream_size - siz_offset - 2u) {
        return NULL;
    }
    x_size = read_be32(codestream + siz_offset + 6u);
    y_size = read_be32(codestream + siz_offset + 10u);
    x_origin = read_be32(codestream + siz_offset + 14u);
    y_origin = read_be32(codestream + siz_offset + 18u);
    if (x_size <= x_origin || y_size <= y_origin ||
        read_be32(codestream + siz_offset + 30u) != 0u ||
        read_be32(codestream + siz_offset + 34u) != 0u) {
        return NULL;
    }
    width = x_size - x_origin;
    height = y_size - y_origin;
    if (x_size > UINT32_MAX - origin_offset ||
        y_size > UINT32_MAX - origin_offset ||
        x_origin > UINT32_MAX - origin_offset ||
        y_origin > UINT32_MAX - origin_offset) {
        return NULL;
    }
    result = (uint8_t *)malloc(codestream_size);
    if (!result) return NULL;
    memcpy(result, codestream, codestream_size);
    write_be32(result + siz_offset + 6u, x_size + origin_offset);
    write_be32(result + siz_offset + 10u, y_size + origin_offset);
    write_be32(result + siz_offset + 14u, x_origin + origin_offset);
    write_be32(result + siz_offset + 18u, y_origin + origin_offset);
    write_be32(result + siz_offset + 30u, origin_offset);
    write_be32(result + siz_offset + 34u, origin_offset);
    if (read_be32(result + siz_offset + 6u) -
            read_be32(result + siz_offset + 14u) != width ||
        read_be32(result + siz_offset + 10u) -
            read_be32(result + siz_offset + 18u) != height) {
        free(result);
        return NULL;
    }
    *out_size = codestream_size;
    return result;
}

static int check_t803_profile0_p0_09_nonzero_origin(
    const uint8_t *codestream,
    size_t codestream_size,
    const uint8_t *reference,
    size_t reference_size)
{
    static const uint8_t pgx_header[] = "PG ML  8 17 37\n";
    const uint32_t expected_width = 17u;
    const uint32_t expected_height = 37u;
    const size_t expected_pixel_count =
        (size_t)expected_width * (size_t)expected_height;
    size_t cod_offset;
    size_t shifted_size = 0u;
    uint8_t *shifted = NULL;
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    uint8_t decomposition_levels;
    uint32_t origin_offset;
    QwPageImage page = {0};
    int rc;
    int passed = 0;

    if (!codestream || !reference ||
        reference_size != sizeof(pgx_header) - 1u + expected_pixel_count ||
        memcmp(reference, pgx_header, sizeof(pgx_header) - 1u) != 0) {
        return 0;
    }
    cod_offset = find_first_marker(codestream, codestream_size, 0u, 0x52u);
    if (cod_offset > codestream_size || codestream_size - cod_offset < 10u) {
        return 0;
    }
    decomposition_levels = codestream[cod_offset + 9u];
    if ((unsigned)decomposition_levels + 15u > 31u) return 0;
    /* Preserve the official packet grid by shifting whole maximum-size LL precincts. */
    origin_offset = UINT32_C(1) << ((unsigned)decomposition_levels + 15u);
    shifted = make_t803_nonzero_origin_codestream(
        codestream, codestream_size, origin_offset, &shifted_size);
    if (!shifted || shifted_size > SIZE_MAX - 85u) goto cleanup;
    jp2 = (uint8_t *)malloc(85u + shifted_size);
    if (!jp2) goto cleanup;
    jp2_size = make_t803_gray8_jp2(
        shifted, shifted_size, expected_width, expected_height,
        jp2, 85u + shifted_size);
    if (!jp2_size) goto cleanup;

    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK && page.data &&
             page.kind == QW_PAGE_IMAGE_GRAY8 &&
             page.width == expected_width && page.height == expected_height &&
             page.size == expected_pixel_count &&
             memcmp(page.data, reference + sizeof(pgx_header) - 1u,
                    expected_pixel_count) == 0;
    if (!passed) {
        fprintf(stderr,
                "Internal T.803 p0_09 aligned nonzero-origin parity failed: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    free(shifted);
    return passed;
}

static int check_t803_profile0_p0_01(void)
{
    static const uint8_t pgx_header[] = "PG ML + 8 128 128\n";
    const size_t expected_pixel_count = 128u * 128u;
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    uint8_t *reference = NULL;
    size_t reference_size = 0u;
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    QwImage image = {0};
    int rc;
    unsigned peak_error = 0u;
    double mse = 0.0;
    int passed = 0;

    if (!read_fixture("t803_profile0_p0_01.j2k",
                      &codestream, &codestream_size) ||
        !read_fixture("t803_profile0_p0_01_class0.pgx",
                      &reference, &reference_size)) {
        fputs("Unable to read official T.803 Profile 0 p0_01 vectors\n",
              stderr);
        goto cleanup;
    }
    if (reference_size != sizeof(pgx_header) - 1u + expected_pixel_count ||
        memcmp(reference, pgx_header, sizeof(pgx_header) - 1u) != 0) {
        fputs("Official T.803 p0_01 PGX header or length is unexpected\n",
              stderr);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(
        codestream, codestream_size, &page);
    passed = rc == QW_STATUS_OK && page.data &&
             page.kind == QW_PAGE_IMAGE_GRAY8 && page.width == 128u &&
             page.height == 128u && page.size == expected_pixel_count &&
             memcmp(page.data, reference + sizeof(pgx_header) - 1u,
                    expected_pixel_count) == 0;
    if (!passed) {
        fprintf(stderr,
                "Internal raw T.803 p0_01 pixel parity failed: rc=%d, "
                "kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
    }
    qw_image_release_page(&page);
    if (!passed) goto cleanup;

    rc = qw_image_decode(codestream, codestream_size, &image);
    passed = rc == QW_IMAGE_OK && image.pixels &&
             image.format == QW_IMAGE_PIXEL_GRAY8 && image.width == 128u &&
             image.height == 128u && image.stride == 128u &&
             image.size == expected_pixel_count &&
             memcmp(image.pixels, reference + sizeof(pgx_header) - 1u,
                    expected_pixel_count) == 0;
    if (!passed) {
        fprintf(stderr,
                "Public raw T.803 p0_01 pixel parity failed: rc=%d, "
                "format=%d, dimensions=%ux%u, stride=%zu, size=%zu\n",
                rc, image.format, image.width, image.height,
                image.stride, image.size);
    }
    qw_image_free(&image);
    if (!passed) goto cleanup;

    if (codestream_size > SIZE_MAX - 85u) goto cleanup;
    jp2 = (uint8_t *)malloc(85u + codestream_size);
    if (!jp2) goto cleanup;
    jp2_size = make_t803_gray8_jp2(codestream, codestream_size,
                                   128u, 128u,
                                   jp2, 85u + codestream_size);
    if (!jp2_size) goto cleanup;

    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK && page.data &&
             page.kind == QW_PAGE_IMAGE_GRAY8 && page.width == 128u &&
             page.height == 128u && page.size == expected_pixel_count &&
             memcmp(page.data, reference + sizeof(pgx_header) - 1u,
                    expected_pixel_count) == 0;
    if (!passed) {
        fprintf(stderr,
                "Internal T.803 p0_01 pixel parity failed: rc=%d, "
                "kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
    }
    qw_image_release_page(&page);
    if (!passed) goto cleanup;

    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK && page.data &&
             page.kind == QW_PAGE_IMAGE_GRAY8 && page.width == 128u &&
             page.height == 128u && page.size == expected_pixel_count &&
             memcmp(page.data, reference + sizeof(pgx_header) - 1u,
                    expected_pixel_count) == 0;
    if (!passed) {
        fprintf(stderr,
                "Dispatched T.803 p0_01 pixel parity failed: rc=%d, "
                "kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
    }

cleanup:
    qw_image_free(&image);
    qw_image_release_page(&page);
    free(jp2);
    free(reference);
    free(codestream);
    return passed;
}

static uint8_t test_p0_08_sample_to_u8(const opj_image_comp_t *component,
                                      OPJ_INT32 sample)
{
    int64_t value;
    uint64_t maximum;
    unsigned precision;

    if (!component || component->prec == 0u || component->prec > 32u) {
        return 0u;
    }
    precision = component->prec;
    value = sample;
    if (component->sgnd) {
        value += (int64_t)1 << (precision - 1u);
        if (value < 0) value = 0;
    } else if (value < 0) {
        value = 0;
    }
    maximum = ((uint64_t)1u << precision) - 1u;
    if ((uint64_t)value > maximum) value = (int64_t)maximum;
    return (uint8_t)(((uint64_t)value * 255u + maximum / 2u) / maximum);
}

static int check_t803_profile0_p0_08_component_qcc(void)
{
    const uint32_t expected_width = 513u;
    const uint32_t expected_height = 3072u;
    const size_t expected_size =
        (size_t)expected_width * (size_t)expected_height * 3u;
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    QwPageImage internal_page = {0};
    QwImage decoded = {0};
    TestOpenJpegMemoryStream memory = {0};
    opj_stream_t *stream = NULL;
    opj_codec_t *codec = NULL;
    opj_image_t *reference = NULL;
    opj_dparameters_t parameters;
    uint64_t decoded_hash;
    int rc;
    int passed = 0;

    if (!read_fixture("t803_profile0_p0_08.j2k",
                      &codestream, &codestream_size)) {
        fputs("Unable to read official T.803 Profile 0 p0_08 vector\n",
              stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(codestream, codestream_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "Official T.803 p0_08 structure validation failed: %d\n",
                rc);
        goto cleanup;
    }

    rc = qw_jp2_decode_internal_gray8(codestream, codestream_size,
                                      &internal_page);
    if (rc != QW_STATUS_OK || !internal_page.data ||
        internal_page.kind != QW_PAGE_IMAGE_RGB24 ||
        internal_page.width != expected_width ||
        internal_page.height != expected_height ||
        internal_page.size != expected_size) {
        fprintf(stderr,
                "Official T.803 p0_08 direct internal decode failed: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, internal_page.kind, internal_page.width,
                internal_page.height, internal_page.size);
        goto cleanup;
    }

    memory.data = codestream;
    memory.size = codestream_size;
    stream = opj_stream_default_create(OPJ_TRUE);
    codec = opj_create_decompress(OPJ_CODEC_J2K);
    if (!stream || !codec) goto reference_decode_failed;
    opj_stream_set_user_data(stream, &memory, NULL);
    opj_stream_set_user_data_length(stream, (OPJ_UINT64)codestream_size);
    opj_stream_set_read_function(stream, test_openjpeg_read);
    opj_stream_set_skip_function(stream, test_openjpeg_skip);
    opj_stream_set_seek_function(stream, test_openjpeg_seek);
    opj_set_error_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_warning_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_info_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_default_decoder_parameters(&parameters);
    if (!opj_setup_decoder(codec, &parameters) ||
        !opj_read_header(stream, codec, &reference) || !reference ||
        !opj_decode(codec, stream, reference) ||
        !opj_end_decompress(codec, stream)) {
        goto reference_decode_failed;
    }
    if (reference->x0 != 0u || reference->y0 != 0u ||
        reference->x1 != expected_width || reference->y1 != expected_height ||
        reference->numcomps != 3u) {
        fputs("Official T.803 p0_08 OpenJPEG reference layout is unexpected\n",
              stderr);
        goto cleanup;
    }
    for (OPJ_UINT32 component_index = 0u;
         component_index < reference->numcomps; ++component_index) {
        const opj_image_comp_t *component =
            &reference->comps[component_index];
        if (!component->data || component->w != expected_width ||
            component->h != expected_height || component->prec != 12u ||
            !component->sgnd) {
            fputs("Official T.803 p0_08 component metadata is unexpected\n",
                  stderr);
            goto cleanup;
        }
    }

    rc = qw_image_decode(codestream, codestream_size, &decoded);
    if (rc != QW_IMAGE_OK || decoded.width != expected_width ||
        decoded.height != expected_height ||
        decoded.format != QW_IMAGE_PIXEL_RGB24 ||
        decoded.stride != (size_t)expected_width * 3u ||
        decoded.size != expected_size || !decoded.pixels) {
        fprintf(stderr,
                "Official T.803 p0_08 public dispatch layout failed: "
                "rc=%d, format=%d, dimensions=%ux%u, stride=%zu, size=%zu\n",
                rc, decoded.format, decoded.width, decoded.height,
                decoded.stride, decoded.size);
        goto cleanup;
    }
    for (size_t pixel = 0u;
         pixel < (size_t)expected_width * (size_t)expected_height; ++pixel) {
        for (OPJ_UINT32 component_index = 0u;
             component_index < reference->numcomps; ++component_index) {
            uint8_t expected = test_p0_08_sample_to_u8(
                &reference->comps[component_index],
                reference->comps[component_index].data[pixel]);
            uint8_t actual = decoded.pixels[pixel * 3u + component_index];
            uint8_t internal_actual =
                internal_page.data[pixel * 3u + component_index];
            if (internal_actual != expected || actual != expected) {
                fprintf(stderr,
                        "Official T.803 p0_08 internal/OpenJPEG mismatch at "
                        "pixel %zu component %u: internal=%u public=%u "
                        "expected=%u\n",
                        pixel, (unsigned)component_index,
                        (unsigned)internal_actual, (unsigned)actual,
                        (unsigned)expected);
                goto cleanup;
            }
        }
    }
    decoded_hash = fnv1a64(internal_page.data, internal_page.size);
    printf("Official T.803 p0_08 direct internal and public RGB24 match "
           "OpenJPEG exactly (%zu bytes, FNV-1a64 %llu)\n",
           internal_page.size, (unsigned long long)decoded_hash);
    passed = 1;
    goto cleanup;

reference_decode_failed:
    fputs("Could not decode official T.803 p0_08 with the OpenJPEG reference\n",
          stderr);

cleanup:
    if (reference) opj_image_destroy(reference);
    if (codec) opj_destroy_codec(codec);
    if (stream) opj_stream_destroy(stream);
    qw_image_free(&decoded);
    qw_image_release_page(&internal_page);
    free(codestream);
    return passed;
}

static int check_t803_profile0_p0_08_reduced(void)
{
    const uint32_t expected_width = 257u;
    const uint32_t expected_height = 1536u;
    const size_t expected_size =
        (size_t)expected_width * (size_t)expected_height * 3u;
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    QwPageImage reduced_page = {0};
    QwPageImage unsupported_page = {0};
    TestOpenJpegMemoryStream memory = {0};
    opj_stream_t *stream = NULL;
    opj_codec_t *codec = NULL;
    opj_image_t *reference = NULL;
    opj_dparameters_t parameters;
    uint64_t reduced_hash;
    int rc;
    int passed = 0;

    if (!read_fixture("t803_profile0_p0_08.j2k",
                      &codestream, &codestream_size)) {
        fputs("Unable to read official T.803 p0_08 vector for reduced decode\n",
              stderr);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8_reduced(
        codestream, codestream_size, 1u, &reduced_page);
    if (rc != QW_STATUS_OK || !reduced_page.data ||
        reduced_page.kind != QW_PAGE_IMAGE_RGB24 ||
        reduced_page.width != expected_width ||
        reduced_page.height != expected_height ||
        reduced_page.size != expected_size) {
        fprintf(stderr,
                "Official T.803 p0_08 reduced internal decode failed: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, reduced_page.kind, reduced_page.width,
                reduced_page.height, reduced_page.size);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8_reduced(
        codestream, codestream_size, 8u, &unsupported_page);
    if (rc != QW_STATUS_ERR_UNSUPPORTED || unsupported_page.data) {
        fprintf(stderr,
                "Official T.803 p0_08 accepted reduction beyond the "
                "shallowest component level: rc=%d\n", rc);
        goto cleanup;
    }

    memory.data = codestream;
    memory.size = codestream_size;
    stream = opj_stream_default_create(OPJ_TRUE);
    codec = opj_create_decompress(OPJ_CODEC_J2K);
    if (!stream || !codec) goto reference_decode_failed;
    opj_stream_set_user_data(stream, &memory, NULL);
    opj_stream_set_user_data_length(stream, (OPJ_UINT64)codestream_size);
    opj_stream_set_read_function(stream, test_openjpeg_read);
    opj_stream_set_skip_function(stream, test_openjpeg_skip);
    opj_stream_set_seek_function(stream, test_openjpeg_seek);
    opj_set_error_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_warning_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_info_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_default_decoder_parameters(&parameters);
    parameters.cp_reduce = 1u;
    if (!opj_setup_decoder(codec, &parameters) ||
        !opj_read_header(stream, codec, &reference) || !reference ||
        !opj_decode(codec, stream, reference) ||
        !opj_end_decompress(codec, stream)) {
        goto reference_decode_failed;
    }
    if (reference->x0 != 0u || reference->y0 != 0u ||
        reference->x1 != 513u || reference->y1 != 3072u ||
        reference->numcomps != 3u) {
        fprintf(stderr,
                "Official T.803 p0_08 reduced OpenJPEG layout is unexpected: "
                "origin=(%u,%u), end=(%u,%u), components=%u\n",
                reference->x0, reference->y0, reference->x1, reference->y1,
                reference->numcomps);
        goto cleanup;
    }
    for (OPJ_UINT32 component_index = 0u;
         component_index < reference->numcomps; ++component_index) {
        const opj_image_comp_t *component =
            &reference->comps[component_index];
        if (!component->data || component->w != expected_width ||
            component->h != expected_height || component->prec != 12u ||
            !component->sgnd) {
            fputs("Official T.803 p0_08 reduced component metadata is "
                  "unexpected\n", stderr);
            goto cleanup;
        }
    }
    for (size_t pixel = 0u;
         pixel < (size_t)expected_width * (size_t)expected_height; ++pixel) {
        for (OPJ_UINT32 component_index = 0u;
             component_index < reference->numcomps; ++component_index) {
            uint8_t expected = test_p0_08_sample_to_u8(
                &reference->comps[component_index],
                reference->comps[component_index].data[pixel]);
            uint8_t actual =
                reduced_page.data[pixel * 3u + component_index];
            if (actual != expected) {
                fprintf(stderr,
                        "Official T.803 p0_08 reduced internal/OpenJPEG "
                        "mismatch at pixel %zu component %u: actual=%u "
                        "expected=%u\n",
                        pixel, (unsigned)component_index,
                        (unsigned)actual, (unsigned)expected);
                goto cleanup;
            }
        }
    }
    reduced_hash = fnv1a64(reduced_page.data, reduced_page.size);
    printf("Official T.803 p0_08 reduction 1 RGB24 matches OpenJPEG exactly "
           "(%zu bytes, FNV-1a64 %llu); reduction 8 rejected for the "
           "shallowest component\n",
           reduced_page.size, (unsigned long long)reduced_hash);
    passed = 1;
    goto cleanup;

reference_decode_failed:
    fputs("Could not decode reduced official T.803 p0_08 with OpenJPEG\n",
          stderr);

cleanup:
    if (reference) opj_image_destroy(reference);
    if (codec) opj_destroy_codec(codec);
    if (stream) opj_stream_destroy(stream);
    qw_image_release_page(&unsupported_page);
    qw_image_release_page(&reduced_page);
    free(codestream);
    return passed;
}

static int check_reduced_rgb53_reference(void)
{
    const uint32_t output_width = 8u;
    const uint32_t output_height = 8u;
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    TestOpenJpegMemoryStream memory = {0};
    opj_stream_t *stream = NULL;
    opj_codec_t *codec = NULL;
    opj_image_t *reference = NULL;
    opj_dparameters_t parameters;
    int rc;
    int passed = 0;

    if (!read_fixture("synthetic_rgb8_no_mct_16x16.jp2", &jp2, &jp2_size)) {
        fputs("Unable to read the reduced RGB 5/3 reference fixture\n",
              stderr);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8_reduced(jp2, jp2_size, 1u, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != output_width ||
        page.height != output_height ||
        page.size != (size_t)output_width * output_height * 3u) {
        fprintf(stderr,
                "Reduced RGB 5/3 internal decode has an unexpected layout: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }

    memory.data = jp2;
    memory.size = jp2_size;
    stream = opj_stream_default_create(OPJ_TRUE);
    codec = opj_create_decompress(OPJ_CODEC_JP2);
    if (!stream || !codec) goto reference_decode_failed;
    opj_stream_set_user_data(stream, &memory, NULL);
    opj_stream_set_user_data_length(stream, (OPJ_UINT64)jp2_size);
    opj_stream_set_read_function(stream, test_openjpeg_read);
    opj_stream_set_skip_function(stream, test_openjpeg_skip);
    opj_stream_set_seek_function(stream, test_openjpeg_seek);
    opj_set_error_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_warning_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_info_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_default_decoder_parameters(&parameters);
    parameters.cp_reduce = 1u;
    if (!opj_setup_decoder(codec, &parameters) ||
        !opj_read_header(stream, codec, &reference) || !reference ||
        !opj_decode(codec, stream, reference) ||
        !opj_end_decompress(codec, stream)) {
        goto reference_decode_failed;
    }
    if (reference->x0 != 0u || reference->y0 != 0u ||
        reference->x1 != 16u || reference->y1 != 16u ||
        reference->numcomps != 3u) {
        fputs("Reduced RGB 5/3 OpenJPEG reference has an unexpected canvas\n",
              stderr);
        goto cleanup;
    }
    for (OPJ_UINT32 component = 0u; component < reference->numcomps;
         ++component) {
        const opj_image_comp_t *plane = &reference->comps[component];
        if (!plane->data || plane->w != output_width ||
            plane->h != output_height || plane->prec != 8u || plane->sgnd) {
            fputs("Reduced RGB 5/3 OpenJPEG plane has an unexpected layout\n",
                  stderr);
            goto cleanup;
        }
    }
    for (size_t pixel = 0u; pixel < (size_t)output_width * output_height;
         ++pixel) {
        for (OPJ_UINT32 component = 0u; component < 3u; ++component) {
            int sample = reference->comps[component].data[pixel];
            uint8_t actual = page.data[pixel * 3u + component];
            if (sample < 0 || sample > 255 || actual != (uint8_t)sample) {
                fprintf(stderr,
                        "Reduced RGB 5/3 internal/OpenJPEG mismatch at "
                        "pixel %zu component %u: actual=%u expected=%d\n",
                        pixel, (unsigned)component, (unsigned)actual, sample);
                goto cleanup;
            }
        }
    }
    puts("Reduced RGB 5/3 matches OpenJPEG exactly at 8x8 RGB24");
    passed = 1;
    goto cleanup;

reference_decode_failed:
    fputs("Could not decode the reduced RGB 5/3 fixture with OpenJPEG\n",
          stderr);

cleanup:
    if (reference) opj_image_destroy(reference);
    if (codec) opj_destroy_codec(codec);
    if (stream) opj_stream_destroy(stream);
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_t803_profile0_p0_09_lossy(void)
{
    static const uint8_t pgx_header[] = "PG ML  8 17 37\n";
    static const uint8_t reduced_pgx_header[] = "PG ML 8 5 10\n";
    const uint32_t expected_width = 17u;
    const uint32_t expected_height = 37u;
    const size_t expected_pixel_count =
        (size_t)expected_width * (size_t)expected_height;
    const uint32_t reduced_width = 5u;
    const uint32_t reduced_height = 10u;
    const size_t reduced_pixel_count =
        (size_t)reduced_width * (size_t)reduced_height;
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    uint8_t *reference = NULL;
    size_t reference_size = 0u;
    uint8_t *reduced_reference = NULL;
    size_t reduced_reference_size = 0u;
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage raw_page = {0};
    QwImage raw_image = {0};
    QwPageImage page = {0};
    int passed = 0;

    if (!read_fixture("t803_profile0_p0_09.j2k",
                      &codestream, &codestream_size) ||
        !read_fixture("t803_profile0_p0_09_class1.pgx",
                      &reference, &reference_size) ||
        !read_fixture("t803_profile0_p0_09_class0.pgx",
                      &reduced_reference, &reduced_reference_size)) {
        fputs("Unable to read official T.803 Profile 0 p0_09 vectors\n",
              stderr);
        goto cleanup;
    }
    if (reference_size != sizeof(pgx_header) - 1u + expected_pixel_count ||
        memcmp(reference, pgx_header, sizeof(pgx_header) - 1u) != 0) {
        fputs("Official T.803 p0_09 PGX header or length is unexpected\n",
              stderr);
        goto cleanup;
    }
    if (reduced_reference_size !=
            sizeof(reduced_pgx_header) - 1u + reduced_pixel_count ||
        memcmp(reduced_reference, reduced_pgx_header,
               sizeof(reduced_pgx_header) - 1u) != 0) {
        fputs("Official T.803 p0_09 reduced PGX header or length is unexpected\n",
              stderr);
        goto cleanup;
    }
    {
        int rc = qw_jp2_decode_internal_gray8(codestream, codestream_size,
                                               &raw_page);
        if (rc != QW_STATUS_OK || !raw_page.data ||
            raw_page.kind != QW_PAGE_IMAGE_GRAY8 ||
            raw_page.width != expected_width ||
            raw_page.height != expected_height ||
            raw_page.size != expected_pixel_count ||
            memcmp(raw_page.data, reference + sizeof(pgx_header) - 1u,
                   expected_pixel_count) != 0) {
            fprintf(stderr,
                    "Internal raw T.803 p0_09 Class 1 pixel check failed: "
                    "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                    rc, raw_page.kind, raw_page.width, raw_page.height,
                    raw_page.size);
            goto cleanup;
        }
        rc = qw_image_decode(codestream, codestream_size, &raw_image);
        if (rc != QW_IMAGE_OK || !raw_image.pixels ||
            raw_image.format != QW_IMAGE_PIXEL_GRAY8 ||
            raw_image.width != expected_width ||
            raw_image.height != expected_height ||
            raw_image.stride != expected_width ||
            raw_image.size != expected_pixel_count ||
            memcmp(raw_image.pixels, raw_page.data,
                   expected_pixel_count) != 0) {
            fprintf(stderr,
                    "Public raw T.803 p0_09 Class 1 dispatch check failed: "
                    "rc=%d, format=%d, dimensions=%ux%u, stride=%zu, "
                    "size=%zu\n",
                    rc, raw_image.format, raw_image.width, raw_image.height,
                    raw_image.stride, raw_image.size);
            goto cleanup;
        }
        qw_image_free(&raw_image);
        qw_image_release_page(&raw_page);
    }
    if (codestream_size > SIZE_MAX - 85u) goto cleanup;
    jp2 = (uint8_t *)malloc(85u + codestream_size);
    if (!jp2) goto cleanup;
    jp2_size = make_t803_gray8_jp2(codestream, codestream_size,
                                   expected_width, expected_height,
                                   jp2, 85u + codestream_size);
    if (!jp2_size) goto cleanup;

    for (unsigned dispatch = 0u; dispatch < 2u; ++dispatch) {
        int rc = dispatch == 0u
            ? qw_jp2_decode_internal_gray8(jp2, jp2_size, &page)
            : qw_jp2_decode_page(jp2, jp2_size, &page);
        passed = rc == QW_STATUS_OK && page.data &&
                 page.kind == QW_PAGE_IMAGE_GRAY8 &&
                 page.width == expected_width &&
                 page.height == expected_height &&
                 page.size == expected_pixel_count &&
                 memcmp(page.data, reference + sizeof(pgx_header) - 1u,
                        expected_pixel_count) == 0;
        if (!passed) {
            fprintf(stderr,
                    "%s T.803 p0_09 Class 1 pixel check failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    dispatch == 0u ? "Internal" : "Dispatched", rc,
                    page.kind, page.width, page.height, page.size);
            if (page.data && page.size == expected_pixel_count) {
                unsigned peak_error = 0u;
                uint64_t squared_error_sum = 0u;
                for (size_t i = 0u; i < expected_pixel_count; ++i) {
                    unsigned actual = page.data[i];
                    unsigned expected = reference[sizeof(pgx_header) - 1u + i];
                    unsigned error = actual > expected
                        ? actual - expected : expected - actual;
                    if (error > peak_error) peak_error = error;
                    squared_error_sum += (uint64_t)error * error;
                }
                fprintf(stderr,
                        "T.803 p0_09 errors: peak=%u, MSE=%.9f "
                        "(Class 1 Profile 0 limits: peak=0, MSE=0)\n",
                        peak_error,
                        (double)squared_error_sum /
                            (double)expected_pixel_count);
            }
        }
        qw_image_release_page(&page);
        if (!passed) break;
    }
    if (passed) {
        int rc = qw_jp2_decode_internal_gray8_reduced(
            jp2, jp2_size, 2u, &page);
        unsigned peak_error = 0u;
        uint64_t squared_error_sum = 0u;
        double mean_squared_error = 0.0;
        int output_valid = rc == QW_STATUS_OK && page.data &&
                           page.kind == QW_PAGE_IMAGE_GRAY8 &&
                           page.width == reduced_width &&
                           page.height == reduced_height &&
                           page.size == reduced_pixel_count;
        if (output_valid) {
            const uint8_t *actual = (const uint8_t *)page.data;
            const uint8_t *expected = reduced_reference +
                                      sizeof(reduced_pgx_header) - 1u;
            for (size_t i = 0u; i < reduced_pixel_count; ++i) {
                unsigned error = actual[i] > expected[i]
                    ? (unsigned)actual[i] - (unsigned)expected[i]
                    : (unsigned)expected[i] - (unsigned)actual[i];
                if (error > peak_error) peak_error = error;
                squared_error_sum += (uint64_t)error * error;
            }
            mean_squared_error = (double)squared_error_sum /
                                 (double)reduced_pixel_count;
        }
        passed = output_valid && peak_error <= 4u &&
                 mean_squared_error <= 1.47;
        if (!passed) {
            fprintf(stderr,
                    "Reduced T.803 p0_09 Class 0 check failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu, "
                    "peak=%u, MSE=%.9f (limits: peak=4, MSE=1.47)\n",
                    rc, page.kind, page.width, page.height, page.size,
                    peak_error, mean_squared_error);
        }
        qw_image_release_page(&page);
    }

cleanup:
    qw_image_free(&raw_image);
    qw_image_release_page(&raw_page);
    qw_image_release_page(&page);
    free(jp2);
    free(reduced_reference);
    free(reference);
    free(codestream);
    return passed;
}

static int check_t803_profile0_p0_02_subsampled(void)
{
    static const uint8_t pgx_header[] = "PG ML +8 64 126\n";
    const uint32_t reference_width = 127u;
    const uint32_t expected_width = 64u;
    const uint32_t expected_height = 126u;
    const size_t expected_pixel_count =
        (size_t)expected_width * (size_t)expected_height;
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    uint8_t *reference = NULL;
    size_t reference_size = 0u;
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    int passed = 0;

    if (!read_fixture("t803_profile0_p0_02.j2k",
                      &codestream, &codestream_size) ||
        !read_fixture("t803_profile0_p0_02_class1.pgx",
                      &reference, &reference_size)) {
        fputs("Unable to read official T.803 Profile 0 p0_02 vectors\n",
              stderr);
        goto cleanup;
    }
    if (reference_size != sizeof(pgx_header) - 1u + expected_pixel_count ||
        memcmp(reference, pgx_header, sizeof(pgx_header) - 1u) != 0) {
        fputs("Official T.803 p0_02 PGX header or length is unexpected\n",
              stderr);
        goto cleanup;
    }
    {
        int structure_rc = qw_jp2_validate_structure(codestream,
                                                     codestream_size);
        if (structure_rc != QW_STATUS_OK) {
            fprintf(stderr,
                    "Official raw T.803 p0_02 structure failed: rc=%d\n",
                    structure_rc);
            goto cleanup;
        }
    }
    {
        int internal_rc = qw_jp2_decode_internal_gray8(
            codestream, codestream_size, &page);
        if (internal_rc != QW_STATUS_OK || !page.data ||
            page.kind != QW_PAGE_IMAGE_GRAY8 ||
            page.width != expected_width || page.height != expected_height ||
            page.size != expected_pixel_count ||
            memcmp(page.data, reference + sizeof(pgx_header) - 1u,
                   expected_pixel_count) != 0) {
            fprintf(stderr,
                    "Internal raw T.803 p0_02 decode failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    internal_rc, page.kind, page.width, page.height,
                    page.size);
            goto cleanup;
        }
    }
    qw_image_release_page(&page);
    {
        int rc = qw_jp2_decode_page(codestream, codestream_size, &page);
        if (rc != QW_STATUS_OK || !page.data ||
            page.kind != QW_PAGE_IMAGE_GRAY8 ||
            page.width != expected_width || page.height != expected_height ||
            page.size != expected_pixel_count ||
            memcmp(page.data, reference + sizeof(pgx_header) - 1u,
                   expected_pixel_count) != 0) {
            fprintf(stderr,
                    "Dispatched raw T.803 p0_02 decode failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    rc, page.kind, page.width, page.height, page.size);
            goto cleanup;
        }
    }
    qw_image_release_page(&page);
    if (codestream_size > SIZE_MAX - 85u) goto cleanup;
    jp2 = (uint8_t *)malloc(85u + codestream_size);
    if (!jp2) goto cleanup;
    jp2_size = make_t803_gray8_jp2(codestream, codestream_size,
                                   reference_width, expected_height,
                                   jp2, 85u + codestream_size);
    if (!jp2_size) goto cleanup;
    {
        int structure_rc = qw_jp2_validate_structure(jp2, jp2_size);
        if (structure_rc != QW_STATUS_OK) {
            fprintf(stderr,
                    "Official T.803 p0_02 structural validation failed: "
                    "rc=%d\n",
                    structure_rc);
            goto cleanup;
        }
    }

    {
        int internal_rc =
            qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
        if (internal_rc != QW_STATUS_OK) {
            fprintf(stderr,
                    "T.803 p0_02 direct internal decoder did not complete: "
                    "status %d\n",
                    internal_rc);
            goto cleanup;
        }
        if (!page.data || page.kind != QW_PAGE_IMAGE_GRAY8 ||
            page.width != expected_width || page.height != expected_height ||
            page.size != expected_pixel_count ||
            memcmp(page.data, reference + sizeof(pgx_header) - 1u,
                   expected_pixel_count) != 0) {
            fputs("Internal T.803 p0_02 output differs from Class 1 reference\n",
                  stderr);
            goto cleanup;
        }
    }
    qw_image_release_page(&page);

    {
        int rc = qw_jp2_decode_page(jp2, jp2_size, &page);
        passed = rc == QW_STATUS_OK && page.data &&
                 page.kind == QW_PAGE_IMAGE_GRAY8 &&
                 page.width == expected_width &&
                 page.height == expected_height &&
                 page.size == expected_pixel_count &&
                 memcmp(page.data, reference + sizeof(pgx_header) - 1u,
                        expected_pixel_count) == 0;
        if (!passed) {
            fprintf(stderr,
                    "Dispatched T.803 p0_02 Class 1 pixel check failed: "
                    "rc=%d, kind=%d, dimensions=%ux%u, size=%zu; "
                    "expected %ux%u\n",
                    rc, page.kind, page.width, page.height, page.size,
                    expected_width, expected_height);
            if (page.data && page.size == expected_pixel_count) {
                unsigned peak_error = 0u;
                uint64_t squared_error_sum = 0u;
                for (size_t i = 0u; i < expected_pixel_count; ++i) {
                    unsigned actual = page.data[i];
                    unsigned expected =
                        reference[sizeof(pgx_header) - 1u + i];
                    unsigned error = actual > expected
                        ? actual - expected : expected - actual;
                    if (error > peak_error) peak_error = error;
                    squared_error_sum += (uint64_t)error * error;
                }
                fprintf(stderr,
                        "T.803 p0_02 errors: peak=%u, MSE=%.9f "
                        "(Class 1 Profile 0 limits: peak=0, MSE=0)\n",
                        peak_error,
                        (double)squared_error_sum /
                            (double)expected_pixel_count);
            }
        }
    }
    qw_image_release_page(&page);

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    free(reference);
    free(codestream);
    return passed;
}

static int check_t803_profile0_p0_09_quantization_overrides(void)
{
    static const uint8_t pgx_header[] = "PG ML  8 17 37\n";
    static const struct {
        uint8_t marker;
        int tile_header;
        const char *label;
    } cases[] = {
        {0x5du, 0, "main QCC"},
        {0x5cu, 1, "tile QCD"},
        {0x5du, 1, "tile QCC"},
    };
    const uint32_t expected_width = 17u;
    const uint32_t expected_height = 37u;
    const size_t expected_pixel_count =
        (size_t)expected_width * (size_t)expected_height;
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    uint8_t *reference = NULL;
    size_t reference_size = 0u;
    uint8_t first_override_pixels[17u * 37u];
    int have_first_override_pixels = 0;
    int passed = 0;

    if (!read_fixture("t803_profile0_p0_09.j2k",
                      &codestream, &codestream_size) ||
        !read_fixture("t803_profile0_p0_09_class1.pgx",
                      &reference, &reference_size)) {
        fputs("Unable to read official T.803 Profile 0 p0_09 override vectors\n",
              stderr);
        goto cleanup;
    }
    if (reference_size != sizeof(pgx_header) - 1u + expected_pixel_count ||
        memcmp(reference, pgx_header, sizeof(pgx_header) - 1u) != 0) {
        fputs("Official T.803 p0_09 override reference is unexpected\n",
              stderr);
        goto cleanup;
    }

    passed = 1;
    for (size_t case_index = 0u;
         case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
        uint8_t *overridden = NULL;
        size_t overridden_size = 0u;
        uint8_t *jp2 = NULL;
        size_t jp2_size = 0u;
        QwPageImage page = {0};
        QwImage public_image = {0};
        int rc;

        overridden = make_t803_quantization_override_codestream(
            codestream, codestream_size, cases[case_index].marker,
            cases[case_index].tile_header, &overridden_size);
        if (!overridden || overridden_size > SIZE_MAX - 85u) {
            free(overridden);
            passed = 0;
            fprintf(stderr, "Unable to create p0_09 %s test codestream\n",
                    cases[case_index].label);
            break;
        }
        jp2 = (uint8_t *)malloc(85u + overridden_size);
        if (!jp2) {
            free(overridden);
            passed = 0;
            break;
        }
        jp2_size = make_t803_gray8_jp2(
            overridden, overridden_size, expected_width, expected_height,
            jp2, 85u + overridden_size);
        free(overridden);
        if (!jp2_size) {
            free(jp2);
            passed = 0;
            break;
        }
        rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
        passed = rc == QW_STATUS_OK && page.data &&
                 page.kind == QW_PAGE_IMAGE_GRAY8 &&
                 page.width == expected_width &&
                 page.height == expected_height &&
                 page.size == expected_pixel_count &&
                 memcmp(page.data, reference + sizeof(pgx_header) - 1u,
                        expected_pixel_count) != 0 &&
                 (!have_first_override_pixels ||
                  memcmp(page.data, first_override_pixels,
                         expected_pixel_count) == 0);
        if (!passed) {
            fprintf(stderr,
                    "Internal T.803 p0_09 %s parity failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    cases[case_index].label, rc, page.kind, page.width,
                    page.height, page.size);
        } else {
            int public_rc = qw_image_decode(jp2, jp2_size, &public_image);
            if (public_rc != QW_IMAGE_OK || !public_image.pixels ||
                public_image.format != QW_IMAGE_PIXEL_GRAY8 ||
                public_image.width != expected_width ||
                public_image.height != expected_height ||
                public_image.stride != expected_width ||
                public_image.size != expected_pixel_count ||
                memcmp(public_image.pixels, page.data,
                       expected_pixel_count) != 0) {
                fprintf(stderr,
                        "Public T.803 p0_09 %s dispatch parity failed: "
                        "rc=%d, format=%d, dimensions=%ux%u, stride=%zu, "
                        "size=%zu\n",
                        cases[case_index].label, public_rc,
                        public_image.format, public_image.width,
                        public_image.height, public_image.stride,
                        public_image.size);
                passed = 0;
            }
        }
        if (passed && !have_first_override_pixels) {
            memcpy(first_override_pixels, page.data, expected_pixel_count);
            have_first_override_pixels = 1;
        }
        qw_image_free(&public_image);
        qw_image_release_page(&page);
        free(jp2);
        if (!passed) break;
    }
    if (passed && !check_t803_profile0_p0_09_nonzero_origin(
                      codestream, codestream_size, reference, reference_size)) {
        fputs("Internal T.803 p0_09 nonzero-origin vector failed\n", stderr);
        passed = 0;
    }

cleanup:
    free(reference);
    free(codestream);
    return passed;
}

static int check_t803_profile0_p0_11_one_dimensional(void)
{
    static const uint8_t pgx_header[] = "PG ML + 8 128 1\n";
    const uint32_t expected_width = 128u;
    const uint32_t expected_height = 1u;
    const size_t expected_pixel_count =
        (size_t)expected_width * (size_t)expected_height;
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    uint8_t *reference = NULL;
    size_t reference_size = 0u;
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    int passed = 0;

    if (!read_fixture("t803_profile0_p0_11.j2k",
                      &codestream, &codestream_size) ||
        !read_fixture("t803_profile0_p0_11_class0.pgx",
                      &reference, &reference_size)) {
        fputs("Unable to read official T.803 Profile 0 p0_11 vectors\n",
              stderr);
        goto cleanup;
    }
    if (reference_size != sizeof(pgx_header) - 1u + expected_pixel_count ||
        memcmp(reference, pgx_header, sizeof(pgx_header) - 1u) != 0) {
        fputs("Official T.803 p0_11 PGX header or length is unexpected\n",
              stderr);
        goto cleanup;
    }
    if (codestream_size > SIZE_MAX - 85u) goto cleanup;
    jp2 = (uint8_t *)malloc(85u + codestream_size);
    if (!jp2) goto cleanup;
    jp2_size = make_t803_gray8_jp2(codestream, codestream_size,
                                   expected_width, expected_height,
                                   jp2, 85u + codestream_size);
    if (!jp2_size) goto cleanup;
    {
        int structure_rc = qw_jp2_validate_structure(jp2, jp2_size);
        if (structure_rc != QW_STATUS_OK) {
            fprintf(stderr,
                    "Official T.803 p0_11 structural validation failed: "
                    "rc=%d\n",
                    structure_rc);
            goto cleanup;
        }
    }

    {
        int internal_rc =
            qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
        if (internal_rc != QW_STATUS_OK || !page.data ||
            page.kind != QW_PAGE_IMAGE_GRAY8 ||
            page.width != expected_width || page.height != expected_height ||
            page.size != expected_pixel_count ||
            memcmp(page.data, reference + sizeof(pgx_header) - 1u,
                   expected_pixel_count) != 0) {
            fprintf(stderr,
                    "Internal T.803 p0_11 pixel parity failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    internal_rc, page.kind, page.width, page.height, page.size);
            goto cleanup;
        }
    }
    qw_image_release_page(&page);

    {
        int rc = qw_jp2_decode_page(jp2, jp2_size, &page);
        passed = rc == QW_STATUS_OK && page.data &&
                 page.kind == QW_PAGE_IMAGE_GRAY8 &&
                 page.width == expected_width &&
                 page.height == expected_height &&
                 page.size == expected_pixel_count &&
                 memcmp(page.data, reference + sizeof(pgx_header) - 1u,
                        expected_pixel_count) == 0;
        if (!passed) {
            fprintf(stderr,
                    "Dispatched T.803 p0_11 pixel parity failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    rc, page.kind, page.width, page.height, page.size);
        }
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    free(reference);
    free(codestream);
    return passed;
}

static int check_t803_profile0_p0_12_tiny(void)
{
    static const uint8_t pgx_header[] = "PG ML + 8 3 5\n";
    const uint32_t expected_width = 3u;
    const uint32_t expected_height = 5u;
    const size_t expected_pixel_count =
        (size_t)expected_width * (size_t)expected_height;
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    uint8_t *reference = NULL;
    size_t reference_size = 0u;
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    int passed = 0;

    if (!read_fixture("t803_profile0_p0_12.j2k",
                      &codestream, &codestream_size) ||
        !read_fixture("t803_profile0_p0_12_class0.pgx",
                      &reference, &reference_size)) {
        fputs("Unable to read official T.803 Profile 0 p0_12 vectors\n",
              stderr);
        goto cleanup;
    }
    if (reference_size != sizeof(pgx_header) - 1u + expected_pixel_count ||
        memcmp(reference, pgx_header, sizeof(pgx_header) - 1u) != 0) {
        fputs("Official T.803 p0_12 PGX header or length is unexpected\n",
              stderr);
        goto cleanup;
    }
    if (codestream_size > SIZE_MAX - 85u) goto cleanup;
    jp2 = (uint8_t *)malloc(85u + codestream_size);
    if (!jp2) goto cleanup;
    jp2_size = make_t803_gray8_jp2(codestream, codestream_size,
                                   expected_width, expected_height,
                                   jp2, 85u + codestream_size);
    if (!jp2_size) goto cleanup;

    {
        int internal_rc =
            qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
        if (internal_rc != QW_STATUS_OK || !page.data ||
            page.kind != QW_PAGE_IMAGE_GRAY8 ||
            page.width != expected_width || page.height != expected_height ||
            page.size != expected_pixel_count ||
            memcmp(page.data, reference + sizeof(pgx_header) - 1u,
                   expected_pixel_count) != 0) {
            fprintf(stderr,
                    "Internal T.803 p0_12 pixel parity failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    internal_rc, page.kind, page.width, page.height, page.size);
            goto cleanup;
        }
    }
    qw_image_release_page(&page);

    {
        int rc = qw_jp2_decode_page(jp2, jp2_size, &page);
        passed = rc == QW_STATUS_OK && page.data &&
                 page.kind == QW_PAGE_IMAGE_GRAY8 &&
                 page.width == expected_width &&
                 page.height == expected_height &&
                 page.size == expected_pixel_count &&
                 memcmp(page.data, reference + sizeof(pgx_header) - 1u,
                        expected_pixel_count) == 0;
        if (!passed) {
            fprintf(stderr,
                    "Dispatched T.803 p0_12 pixel parity failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    rc, page.kind, page.width, page.height, page.size);
        }
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    free(reference);
    free(codestream);
    return passed;
}

static int check_t803_profile0_p0_16_multilayer(void)
{
    static const uint8_t pgx_header[] = "PG ML  8 128 128\n";
    const uint32_t expected_width = 128u;
    const uint32_t expected_height = 128u;
    const size_t expected_pixel_count =
        (size_t)expected_width * (size_t)expected_height;
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    uint8_t *reference = NULL;
    size_t reference_size = 0u;
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    int passed = 0;

    if (!read_fixture("t803_profile0_p0_16.j2k",
                      &codestream, &codestream_size) ||
        !read_fixture("t803_profile0_p0_16_class0.pgx",
                      &reference, &reference_size)) {
        fputs("Unable to read official T.803 p0_16 vectors\n", stderr);
        goto cleanup;
    }
    if (reference_size != sizeof(pgx_header) - 1u + expected_pixel_count ||
        memcmp(reference, pgx_header, sizeof(pgx_header) - 1u) != 0) {
        fputs("Official T.803 p0_16 PGX header or length is unexpected\n",
              stderr);
        goto cleanup;
    }
    if (codestream_size > SIZE_MAX - 85u) goto cleanup;
    jp2 = (uint8_t *)malloc(85u + codestream_size);
    if (!jp2) goto cleanup;
    jp2_size = make_t803_gray8_jp2(codestream, codestream_size,
                                   expected_width, expected_height,
                                   jp2, 85u + codestream_size);
    if (!jp2_size) goto cleanup;
    {
        int structure_rc = qw_jp2_validate_structure(jp2, jp2_size);
        if (structure_rc != QW_STATUS_OK) {
            fprintf(stderr,
                    "Official T.803 p0_16 structural validation failed: "
                    "rc=%d\n",
                    structure_rc);
            goto cleanup;
        }
    }

    {
        int internal_rc =
            qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
        if (internal_rc != QW_STATUS_OK || !page.data ||
            page.kind != QW_PAGE_IMAGE_GRAY8 ||
            page.width != expected_width || page.height != expected_height ||
            page.size != expected_pixel_count ||
            memcmp(page.data, reference + sizeof(pgx_header) - 1u,
                   expected_pixel_count) != 0) {
            fprintf(stderr,
                    "Internal T.803 p0_16 pixel parity failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    internal_rc, page.kind, page.width, page.height, page.size);
            goto cleanup;
        }
    }
    qw_image_release_page(&page);

    {
        int rc = qw_jp2_decode_page(jp2, jp2_size, &page);
        passed = rc == QW_STATUS_OK && page.data &&
                 page.kind == QW_PAGE_IMAGE_GRAY8 &&
                 page.width == expected_width &&
                 page.height == expected_height &&
                 page.size == expected_pixel_count &&
                 memcmp(page.data, reference + sizeof(pgx_header) - 1u,
                        expected_pixel_count) == 0;
        if (!passed) {
            fprintf(stderr,
                    "Dispatched T.803 p0_16 pixel parity failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    rc, page.kind, page.width, page.height, page.size);
        }
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    free(reference);
    free(codestream);
    return passed;
}

static int jp2_t803_signed4_page_matches_reference(
    const QwPageImage *page,
    const uint8_t *reference,
    size_t reference_size)
{
    static const uint8_t pgx_header[] = "PG ML -4 256 256\n";
    const size_t expected_pixel_count = 256u * 256u;
    const size_t pgx_payload_offset = sizeof(pgx_header) - 1u;

    if (!page || !page->data || !reference ||
        reference_size != pgx_payload_offset + expected_pixel_count ||
        memcmp(reference, pgx_header, pgx_payload_offset) != 0 ||
        page->kind != QW_PAGE_IMAGE_GRAY8 || page->width != 256u ||
        page->height != 256u || page->size != expected_pixel_count) {
        return 0;
    }
    for (size_t i = 0u; i < expected_pixel_count; ++i) {
        int sample = (int)reference[pgx_payload_offset + i];
        uint8_t expected;
        if (sample >= 128) sample -= 256;
        if (sample < -8 || sample > 7) return 0;
        expected = (uint8_t)(((unsigned)(sample + 8) * 255u + 7u) / 15u);
        if (page->data[i] != expected) return 0;
    }
    return 1;
}

static int check_t803_profile0_p0_03_signed4_dispatch_mapping(void)
{
    static const uint8_t pgx_header[] = "PG ML -4 256 256\n";
    const uint32_t expected_width = 256u;
    const uint32_t expected_height = 256u;
    const size_t expected_pixel_count =
        (size_t)expected_width * (size_t)expected_height;
    const size_t pgx_payload_offset = sizeof(pgx_header) - 1u;
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    uint8_t *reference = NULL;
    size_t reference_size = 0u;
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    int passed = 0;

    if (!read_fixture("t803_profile0_p0_03.j2k",
                      &codestream, &codestream_size) ||
        !read_fixture("t803_profile0_p0_03_class1.pgx",
                      &reference, &reference_size)) {
        fputs("Unable to read official T.803 p0_03 vectors\n", stderr);
        goto cleanup;
    }
    if (reference_size != pgx_payload_offset + expected_pixel_count ||
        memcmp(reference, pgx_header, pgx_payload_offset) != 0) {
        fputs("Official T.803 p0_03 PGX header or length is unexpected\n",
              stderr);
        goto cleanup;
    }
    {
        int structure_rc = qw_jp2_validate_structure(codestream,
                                                     codestream_size);
        if (structure_rc != QW_STATUS_OK) {
            fprintf(stderr,
                    "Official T.803 p0_03 structural validation failed: "
                    "rc=%d\n",
                    structure_rc);
            goto cleanup;
        }
    }
    {
        int internal_rc = qw_jp2_decode_internal_gray8(
            codestream, codestream_size, &page);
        if (internal_rc != QW_STATUS_OK ||
            !jp2_t803_signed4_page_matches_reference(
                &page, reference, reference_size)) {
            fprintf(stderr,
                    "Internal raw T.803 p0_03 decode failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    internal_rc, page.kind, page.width, page.height,
                    page.size);
            goto cleanup;
        }
    }
    qw_image_release_page(&page);
    {
        int rc = qw_jp2_decode_page(codestream, codestream_size, &page);
        if (rc != QW_STATUS_OK ||
            !jp2_t803_signed4_page_matches_reference(
                &page, reference, reference_size)) {
            fprintf(stderr,
                    "Dispatched raw T.803 p0_03 decode failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    rc, page.kind, page.width, page.height, page.size);
            goto cleanup;
        }
    }
    qw_image_release_page(&page);

    if (codestream_size > SIZE_MAX - 85u) goto cleanup;
    jp2 = (uint8_t *)malloc(codestream_size + 85u);
    if (!jp2) goto cleanup;
    jp2_size = make_t803_signed4_gray_jp2(
        codestream, codestream_size, expected_width, expected_height,
        jp2, codestream_size + 85u);
    if (jp2_size == 0u ||
        qw_jp2_validate_structure(jp2, jp2_size) != QW_STATUS_OK) {
        fputs("Unable to wrap official T.803 p0_03 in a signed-4 JP2 header\n",
              stderr);
        goto cleanup;
    }
    {
        int rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
        if (rc != QW_STATUS_OK ||
            !jp2_t803_signed4_page_matches_reference(
                &page, reference, reference_size)) {
            fprintf(stderr,
                    "Internal signed-4 JP2 decode failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    rc, page.kind, page.width, page.height, page.size);
            goto cleanup;
        }
    }
    qw_image_release_page(&page);

    {
        int rc = qw_jp2_decode_page(jp2, jp2_size, &page);
        if (rc != QW_STATUS_OK ||
            !jp2_t803_signed4_page_matches_reference(
                &page, reference, reference_size)) {
            fprintf(stderr,
                    "Dispatched signed-4 JP2 output mapping failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    rc, page.kind, page.width, page.height, page.size);
            goto cleanup;
        }
    }
    passed = 1;

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    free(reference);
    free(codestream);
    return passed;
}

static int gray53_page_matches_pattern(const QwPageImage *page)
{
    if (!page || !page->data || page->kind != QW_PAGE_IMAGE_GRAY8 ||
        page->width != 13u || page->height != 17u || page->size != 221u) {
        return 0;
    }
    for (uint32_t y = 0u; y < page->height; ++y) {
        for (uint32_t x = 0u; x < page->width; ++x) {
            uint8_t expected = (uint8_t)(x * 19u + y * 13u + (x * y) % 17u);
            if (page->data[(size_t)y * page->width + x] != expected) {
                fprintf(stderr,
                        "Gray 5/3 reference mismatch at (%u,%u): "
                        "expected=%u actual=%u\n",
                        x, y, (unsigned)expected,
                        (unsigned)page->data[(size_t)y * page->width + x]);
                return 0;
            }
        }
    }
    return 1;
}

static int check_internal_reversible_gray8_multilevel(void)
{
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    QwPageImage page = {0};
    int rc;
    int passed;

    if (!read_fixture("gray53_13x17.jp2", &codestream, &codestream_size)) {
        fprintf(stderr, "Unable to read gray 5/3 multilevel fixture\n");
        return 0;
    }
    rc = qw_jp2_decode_internal_gray8(
        codestream, codestream_size, &page);
    passed = rc == QW_STATUS_OK && gray53_page_matches_pattern(&page);
    if (!passed) {
        fprintf(stderr,
                "Internal multilevel gray JP2 decode failed: rc=%d, "
                "kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
    }
    qw_image_release_page(&page);

    if (passed) {
        rc = qw_jp2_decode_page(codestream, codestream_size, &page);
        passed = rc == QW_STATUS_OK && gray53_page_matches_pattern(&page);
        if (!passed) {
            fprintf(stderr,
                    "Dispatched multilevel gray JP2 decode failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    rc, page.kind, page.width, page.height, page.size);
        }
        qw_image_release_page(&page);
    }
    free(codestream);
    return passed;
}

static int multitile_gray8_page_matches_pattern(const QwPageImage *page,
                                               uint32_t width,
                                               uint32_t height)
{
    if (!page || !page->data || page->kind != QW_PAGE_IMAGE_GRAY8 ||
        page->width != width || page->height != height ||
        page->size != (size_t)width * (size_t)height) {
        return 0;
    }
    for (uint32_t y = 0u; y < page->height; ++y) {
        for (uint32_t x = 0u; x < page->width; ++x) {
            uint8_t expected = (uint8_t)(x * 17u + y * 29u + x * y * 3u);
            if (page->data[(size_t)y * page->width + x] != expected) {
                fprintf(stderr,
                        "Gray 5/3 multi-tile mismatch at (%u,%u): "
                        "expected=%u actual=%u\n",
                        x, y, (unsigned)expected,
                        (unsigned)page->data[(size_t)y * page->width + x]);
                return 0;
            }
        }
    }
    return 1;
}

static int check_internal_reversible_gray8_multitile_fixture(
    const char *fixture_name,
    uint32_t width,
    uint32_t height)
{
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    uint8_t *jp2 = NULL;
    size_t jp2_size;
    QwPageImage page = {0};
    int rc;
    int passed = 0;

    if (!read_fixture(fixture_name, &codestream, &codestream_size)) {
        fprintf(stderr, "Unable to read synthetic 5/3 multi-tile fixture\n");
        return 0;
    }
    if (codestream_size > SIZE_MAX - 85u) goto cleanup;
    jp2 = (uint8_t *)malloc(85u + codestream_size);
    if (!jp2) goto cleanup;
    jp2_size = make_t803_gray8_jp2(
        codestream, codestream_size, width, height, jp2,
        85u + codestream_size);
    if (!jp2_size) goto cleanup;

    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK &&
             multitile_gray8_page_matches_pattern(&page, width, height);
    if (!passed) {
        fprintf(stderr,
                "Internal multi-tile gray JP2 decode failed: rc=%d, "
                "kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
    }
    qw_image_release_page(&page);

    if (passed) {
        rc = qw_jp2_decode_page(jp2, jp2_size, &page);
        passed = rc == QW_STATUS_OK &&
                 multitile_gray8_page_matches_pattern(&page, width, height);
        if (!passed) {
            fprintf(stderr,
                    "Dispatched multi-tile gray JP2 decode failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    rc, page.kind, page.width, page.height, page.size);
        }
        qw_image_release_page(&page);
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    free(codestream);
    return passed;
}

static int check_internal_reversible_gray8_tile_local_coding_override(
    int use_coc)
{
    static const char fixture_name[] =
        "synthetic_gray8_16x16_tiles_8x8.j2k";
    uint8_t tile_cod[64];
    uint8_t tile_coc[64];
    const uint8_t *override_segment;
    size_t override_size;
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    size_t cod_offset;
    size_t sot_offset;
    size_t sod_offset;
    uint16_t layer_count;
    uint32_t tile_part_length;
    uint8_t *modified_codestream = NULL;
    size_t modified_codestream_size;
    uint8_t *jp2 = NULL;
    size_t jp2_size;
    QwPageImage page = {0};
    int rc;
    int passed = 0;

    if (!read_fixture(fixture_name, &codestream, &codestream_size)) {
        fprintf(stderr,
                "Unable to read the gray multi-tile coding override fixture\n");
        goto cleanup;
    }
    cod_offset = find_first_marker(codestream, codestream_size, 0u, 0x52u);
    sot_offset = find_first_marker(codestream, codestream_size, 0u, 0x90u);
    if (cod_offset == codestream_size || sot_offset == codestream_size) {
        fputs("Could not locate COD/SOT for tile-local coding override\n",
              stderr);
        goto cleanup;
    }
    sod_offset = find_first_tile_sod(codestream, codestream_size, sot_offset);
    if (sod_offset == codestream_size) {
        fputs("Could not locate SOD for tile-local coding override\n", stderr);
        goto cleanup;
    }

    if (use_coc) {
        size_t precinct_count;
        make_coc_segment(tile_coc, codestream, cod_offset);
        tile_coc[6] = codestream[cod_offset + 9u];
        override_segment = tile_coc;
        if (use_coc == 2) {
            precinct_count = (size_t)tile_coc[6] + 1u;
            if (precinct_count > sizeof(tile_coc) - 11u) {
                fprintf(stderr, "COC precinct count is too large: %zu\n",
                        precinct_count);
                goto cleanup;
            }
            tile_coc[5] = 1u;
            write_be16(tile_coc + 2u, (uint16_t)(9u + precinct_count));
            memset(tile_coc + 11u, 0x33u, precinct_count);
            override_size = 11u + precinct_count;
        } else {
            override_size = 11u;
        }
    } else {
        size_t precinct_count;
        layer_count = read_be16(codestream + cod_offset + 6u);
        if (!layer_count) {
            fputs("Main COD has no layers for tile-local COD override\n",
                  stderr);
            goto cleanup;
        }
        make_tile_cod_segment(tile_cod, codestream, cod_offset,
                              layer_count, codestream[cod_offset + 9u]);
        override_segment = tile_cod;
        if (use_coc == 3) {
            precinct_count = (size_t)tile_cod[9] + 1u;
            if (precinct_count > sizeof(tile_cod) - 14u) {
                fprintf(stderr, "COD precinct count is too large: %zu\n",
                        precinct_count);
                goto cleanup;
            }
            tile_cod[4] = 1u;
            write_be16(tile_cod + 2u, (uint16_t)(12u + precinct_count));
            memset(tile_cod + 14u, 0x33u, precinct_count);
            override_size = 14u + precinct_count;
        } else {
            override_size = 14u;
        }
    }

    tile_part_length = read_be32(codestream + sot_offset + 6u);
    if (!tile_part_length ||
        tile_part_length > codestream_size - sot_offset ||
        tile_part_length > UINT32_MAX - override_size ||
        codestream_size > SIZE_MAX - override_size) {
        fputs("Tile-part bounds prevent coding override insertion\n", stderr);
        goto cleanup;
    }
    modified_codestream_size = codestream_size + override_size;
    modified_codestream = insert_bytes(
        codestream, codestream_size, sod_offset,
        override_segment, override_size);
    if (!modified_codestream) goto cleanup;
    write_be32(modified_codestream + sot_offset + 6u,
               tile_part_length + (uint32_t)override_size);

    if (modified_codestream_size > SIZE_MAX - 85u) goto cleanup;
    jp2 = (uint8_t *)malloc(85u + modified_codestream_size);
    if (!jp2) goto cleanup;
    jp2_size = make_t803_gray8_jp2(
        modified_codestream, modified_codestream_size, 16u, 16u,
        jp2, 85u + modified_codestream_size);
    if (!jp2_size) {
        fputs("Could not wrap tile-local COC precinct override in JP2\n",
              stderr);
        goto cleanup;
    }

    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK &&
             multitile_gray8_page_matches_pattern(&page, 16u, 16u);
    if (!passed) {
        fprintf(stderr,
                "Internal tile-local %s override decode failed: rc=%d, "
                "kind=%d, dimensions=%ux%u, size=%zu\n",
                use_coc == 1 || use_coc == 2 ? "COC" : "COD",
                rc, page.kind,
                page.width, page.height, page.size);
    }
    qw_image_release_page(&page);

    if (passed) {
        rc = qw_jp2_decode_page(jp2, jp2_size, &page);
        passed = rc == QW_STATUS_OK &&
                 multitile_gray8_page_matches_pattern(&page, 16u, 16u);
        if (!passed) {
            fprintf(stderr,
                    "Dispatched tile-local %s override decode failed: rc=%d, "
                    "kind=%d, dimensions=%ux%u, size=%zu\n",
                    use_coc == 1 || use_coc == 2 ? "COC" : "COD",
                    rc, page.kind,
                    page.width, page.height, page.size);
        }
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    free(modified_codestream);
    free(codestream);
    return passed;
}

static int check_internal_reversible_gray8_multitile(void)
{
    return check_internal_reversible_gray8_multitile_fixture(
               "synthetic_gray8_16x8_tiles_8x8.j2k", 16u, 8u) &&
           check_internal_reversible_gray8_multitile_fixture(
               "synthetic_gray8_16x16_tiles_8x8.j2k", 16u, 16u) &&
           check_internal_reversible_gray8_multitile_fixture(
               "synthetic_gray8_crop4x0_16x8_tiles_8x8.j2k", 16u, 8u) &&
           check_internal_reversible_gray8_multitile_fixture(
               "synthetic_gray8_origin2x2_20x14_tiles8x8.j2k", 20u, 14u) &&
            check_internal_reversible_gray8_multitile_fixture(
                "synthetic_gray8_16x16_tiles_8x8_roi_u2.j2k", 16u, 16u) &&
             check_internal_reversible_gray8_multitile_fixture(
                 "synthetic_gray8_16x16_tiles_8x8_roi_u1.j2k", 16u, 16u) &&
             check_internal_reversible_gray8_multitile_fixture(
                 "synthetic_gray8_16x16_tiles_8x8_roi_u30.j2k", 16u, 16u) &&
           check_internal_reversible_gray8_multitile_fixture(
               "synthetic_gray8_16x16_tiles_8x8_roi_u31.j2k", 16u, 16u) &&
           check_internal_reversible_gray8_multitile_fixture(
               "synthetic_gray8_16x16_tiles_8x8_roi_u52.j2k", 16u, 16u) &&
            check_internal_reversible_gray8_multitile_fixture(
                "synthetic_gray8_16x16_tiles_8x8_tile_rgn_u2.j2k", 16u, 16u) &&
            check_internal_reversible_gray8_multitile_fixture(
                "synthetic_gray8_16x16_single_tile_roi_u2.j2k", 16u, 16u) &&
           check_internal_reversible_gray8_tile_local_coding_override(0) &&
            check_internal_reversible_gray8_tile_local_coding_override(1) &&
            check_internal_reversible_gray8_tile_local_coding_override(2) &&
            check_internal_reversible_gray8_tile_local_coding_override(3);
}

static int check_gray8_codestream_matches_reference(
    const uint8_t *codestream,
    size_t codestream_size,
    const uint8_t *reference,
    size_t reference_size,
    uint32_t width,
    uint32_t height,
    const char *label)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size;
    QwPageImage page = {0};
    int rc;
    int passed = 0;

    if (!codestream || !codestream_size || !reference ||
        reference_size != (size_t)width * height ||
        codestream_size > SIZE_MAX - 85u) {
        return 0;
    }
    jp2 = (uint8_t *)malloc(85u + codestream_size);
    if (!jp2) goto cleanup;
    jp2_size = make_t803_gray8_jp2(
        codestream, codestream_size, width, height, jp2,
        85u + codestream_size);
    if (!jp2_size) goto cleanup;

    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK && page.data &&
             page.kind == QW_PAGE_IMAGE_GRAY8 && page.width == width &&
             page.height == height && page.size == reference_size &&
             memcmp(page.data, reference, reference_size) == 0;
    if (!passed) {
        if (page.data && page.size == reference_size) {
            size_t reported = 0u;
            for (size_t i = 0u; i < reference_size && reported < 8u; ++i) {
                if (page.data[i] != reference[i]) {
                    fprintf(stderr,
                            "9/7 reference mismatch at (%zu,%zu): expected=%u actual=%u\n",
                            i % width, i / width, (unsigned)reference[i],
                            (unsigned)page.data[i]);
                    ++reported;
                }
            }
        }
        fprintf(stderr,
                    "Internal 9/7 multi-tile %s failed: "
                "rc=%d, dimensions=%ux%u, size=%zu\n",
                label, rc, page.width, page.height, page.size);
    }
    qw_image_release_page(&page);
    if (passed) {
        rc = qw_jp2_decode_page(jp2, jp2_size, &page);
        passed = rc == QW_STATUS_OK && page.data &&
                 page.kind == QW_PAGE_IMAGE_GRAY8 && page.width == width &&
                 page.height == height && page.size == reference_size &&
                 memcmp(page.data, reference, reference_size) == 0;
        if (!passed) {
            fprintf(stderr,
                    "Dispatched 9/7 multi-tile %s failed: "
                    "rc=%d, dimensions=%ux%u, size=%zu\n",
                    label, rc, page.width, page.height, page.size);
        }
        qw_image_release_page(&page);
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int jp2_rgb_page_matches_reference(
    const QwPageImage *page,
    const uint8_t *reference,
    size_t reference_size,
    uint32_t width,
    uint32_t height,
    unsigned peak_error_limit,
    double mse_limit,
    unsigned *out_peak_error,
    double *out_mse)
{
    unsigned peak_error = 0u;
    uint64_t squared_error_sum = 0u;
    double mse = 0.0;

    if (out_peak_error) *out_peak_error = 0u;
    if (out_mse) *out_mse = 0.0;
    if (!page || !reference || !page->data ||
        page->kind != QW_PAGE_IMAGE_RGB24 || page->width != width ||
        page->height != height || page->size != reference_size ||
        reference_size == 0u) {
        return 0;
    }
    {
        const uint8_t *actual = (const uint8_t *)page->data;
        for (size_t i = 0u; i < reference_size; ++i) {
            unsigned error = actual[i] > reference[i]
                ? (unsigned)actual[i] - (unsigned)reference[i]
                : (unsigned)reference[i] - (unsigned)actual[i];
            if (error > peak_error) peak_error = error;
            squared_error_sum += (uint64_t)error * error;
        }
    }
    mse = (double)squared_error_sum / (double)reference_size;
    if (out_peak_error) *out_peak_error = peak_error;
    if (out_mse) *out_mse = mse;
    return peak_error <= peak_error_limit && mse <= mse_limit;
}

static int check_internal_rgb_reference_fixture(
    const char *fixture_name,
    const uint8_t *reference,
    size_t reference_size,
    const char *label,
    unsigned peak_error_limit,
    double mse_limit)
{
    const uint32_t width = 16u;
    const uint32_t height = 16u;
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    int rc;
    unsigned peak_error = 0u;
    double mse = 0.0;
    int passed = 0;

    if (!fixture_name || !reference || reference_size != 768u ||
        !read_fixture(fixture_name, &jp2, &jp2_size)) {
        fputs("Unable to read an RGB MCT reference fixture\n", stderr);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK && jp2_rgb_page_matches_reference(
        &page, reference, reference_size, width, height, peak_error_limit,
        mse_limit, &peak_error, &mse);
    if (!passed) {
        fprintf(stderr,
                "Internal RGB MCT %s failed: rc=%d, kind=%d, "
                "dimensions=%ux%u, size=%zu, peak=%u (limit=%u), "
                "MSE=%.9f (limit=%.9f)\n",
                label, rc, page.kind, page.width, page.height, page.size,
                peak_error, peak_error_limit, mse, mse_limit);
    }
    qw_image_release_page(&page);
    if (passed) {
        rc = qw_jp2_decode_page(jp2, jp2_size, &page);
        peak_error = 0u;
        mse = 0.0;
        passed = rc == QW_STATUS_OK && jp2_rgb_page_matches_reference(
            &page, reference, reference_size, width, height,
            peak_error_limit, mse_limit, &peak_error, &mse);
        if (!passed) {
            fprintf(stderr,
                    "Dispatched RGB MCT %s failed: rc=%d, kind=%d, "
                    "dimensions=%ux%u, size=%zu, peak=%u (limit=%u), "
                    "MSE=%.9f (limit=%.9f)\n",
                    label, rc, page.kind, page.width, page.height, page.size,
                    peak_error, peak_error_limit, mse, mse_limit);
        }
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_rgb_direct(void)
{
    uint8_t source[16u * 16u * 3u];
    size_t offset = 0u;
    for (uint32_t y = 0u; y < 16u; ++y) {
        for (uint32_t x = 0u; x < 16u; ++x) {
            source[offset++] = (uint8_t)(x * 13u + y * 3u);
            source[offset++] = (uint8_t)(x * 5u + y * 17u + 31u);
            source[offset++] = (uint8_t)(x * 11u + y * 7u + 43u);
        }
    }
    return check_internal_rgb_reference_fixture(
        "synthetic_rgb8_no_mct_16x16.jp2", source, sizeof(source),
        "direct RGB", 0u, 0.0);
}

static int check_internal_rgb_mct_rct(void)
{
    uint8_t source[16u * 16u * 3u];
    size_t offset = 0u;
    for (uint32_t y = 0u; y < 16u; ++y) {
        for (uint32_t x = 0u; x < 16u; ++x) {
            source[offset++] = (uint8_t)(x * 13u + y * 3u);
            source[offset++] = (uint8_t)(x * 5u + y * 17u + 31u);
            source[offset++] = (uint8_t)(x * 11u + y * 7u + 43u);
        }
    }
    return check_internal_rgb_reference_fixture(
        "synthetic_rgb8_mct_rct_16x16.jp2", source, sizeof(source),
        "reversible RCT", 0u, 0.0);
}

static int check_internal_rgb_mct_ict(void)
{
    uint8_t *reference = NULL;
    size_t reference_size = 0u;
    int passed;
    if (!read_fixture("synthetic_rgb8_mct_ict_16x16_reference.rgb24",
                      &reference, &reference_size)) {
        fputs("Unable to read the OpenJPEG RGB ICT reference\n", stderr);
        return 0;
    }
    passed = check_internal_rgb_reference_fixture(
        "synthetic_rgb8_mct_ict_16x16.jp2", reference, reference_size,
        "irreversible ICT", 1u, 0.002);
    free(reference);
    return passed;
}

static int jp2_rgb24_pixels_match_reference(const uint8_t *pixels,
                                           size_t pixels_size,
                                           const uint8_t *reference,
                                           size_t reference_size)
{
    if (!pixels || !reference || pixels_size != 768u ||
        reference_size != 768u) {
        return 0;
    }
    for (size_t i = 0u; i < reference_size; ++i) {
        int difference = (int)pixels[i] - (int)reference[i];
        if (difference < -1 || difference > 1) {
            fprintf(stderr,
                    "ICT plus alpha reference mismatch at byte %zu "
                    "(difference exceeds one LSB): got %u, expected %u\n",
                    i, pixels[i], reference[i]);
            return 0;
        }
    }
    return 1;
}

static int jp2_rgb24_page_matches_reference(const QwPageImage *page,
                                            const uint8_t *reference,
                                            size_t reference_size)
{
    return page && page->data && page->kind == QW_PAGE_IMAGE_RGB24 &&
           page->width == 16u && page->height == 16u &&
           jp2_rgb24_pixels_match_reference(page->data, page->size,
                                            reference, reference_size);
}

static int check_internal_rgb_mct_ict_alpha_data(
    const uint8_t *jp2,
    size_t jp2_size,
    const uint8_t *reference,
    size_t reference_size,
    const char *alpha_description)
{
    QwPageImage page = {0};
    QwImage image = {0};
    uint8_t direct_pixels[768u];
    int rc;
    int passed = 0;

    if (!jp2 || !jp2_size || !reference || reference_size != 768u ||
        !alpha_description) {
        fprintf(stderr, "Unable to read the ICT plus %s JP2 vector\n",
                alpha_description ? alpha_description : "alpha");
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "ICT plus %s structure failed: rc=%d\n",
                alpha_description, rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK ||
        !jp2_rgb24_page_matches_reference(&page, reference, reference_size)) {
        fprintf(stderr,
                "Internal ICT plus %s decode failed: rc=%d, "
                "kind=%d, dimensions=%ux%u, size=%zu\n",
                alpha_description, rc, page.kind, page.width, page.height,
                page.size);
        goto cleanup;
    }
    memcpy(direct_pixels, page.data, sizeof(direct_pixels));
    qw_image_release_page(&page);

    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK ||
        !jp2_rgb24_page_matches_reference(&page, reference, reference_size) ||
        memcmp(page.data, direct_pixels, sizeof(direct_pixels)) != 0) {
        fprintf(stderr,
                "Dispatched ICT plus %s decode failed: rc=%d, "
                "kind=%d, dimensions=%ux%u, size=%zu, "
                "matches direct internal=%d\n",
                alpha_description, rc, page.kind, page.width, page.height,
                page.size,
                page.data && memcmp(page.data, direct_pixels,
                                    sizeof(direct_pixels)) == 0);
        goto cleanup;
    }
    qw_image_release_page(&page);

    rc = qw_image_decode(jp2, jp2_size, &image);
    passed = rc == QW_IMAGE_OK && image.pixels &&
             image.format == QW_IMAGE_PIXEL_RGB24 && image.width == 16u &&
             image.height == 16u &&
             jp2_rgb24_pixels_match_reference(image.pixels, image.size,
                                              reference, reference_size) &&
             memcmp(image.pixels, direct_pixels,
                    sizeof(direct_pixels)) == 0;
    if (!passed) {
        fprintf(stderr,
                "Public ICT plus %s decode failed: rc=%d, "
                "format=%d, dimensions=%ux%u, size=%zu, "
                "matches direct internal=%d\n",
                alpha_description, rc, image.format, image.width, image.height,
                image.size,
                image.pixels && memcmp(image.pixels, direct_pixels,
                                       sizeof(direct_pixels)) == 0);
    }

cleanup:
    qw_image_free(&image);
    qw_image_release_page(&page);
    return passed;
}

static int check_internal_rgb_mct_ict_alpha_fixture(
    const char *fixture_name,
    const char *reference_name,
    const char *alpha_description)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    uint8_t *reference = NULL;
    size_t reference_size = 0u;
    int passed = 0;

    if (!read_fixture(fixture_name, &jp2, &jp2_size) ||
        !read_fixture(reference_name, &reference, &reference_size)) {
        fprintf(stderr, "Unable to read the ICT plus %s JP2 vector\n",
                alpha_description ? alpha_description : "alpha");
        goto cleanup;
    }
    passed = check_internal_rgb_mct_ict_alpha_data(
        jp2, jp2_size, reference, reference_size, alpha_description);

cleanup:
    free(reference);
    free(jp2);
    return passed;
}

static int check_internal_rgb_mct_ict_straight_alpha(void)
{
    return check_internal_rgb_mct_ict_alpha_fixture(
        "synthetic_rgba8_mct_ict_straight_alpha_16x16.jp2",
        "synthetic_rgba8_mct_ict_straight_alpha_16x16.rgb24",
        "straight-alpha");
}

static int check_internal_rgb_mct_ict_premultiplied_alpha(void)
{
    return check_internal_rgb_mct_ict_alpha_fixture(
        "synthetic_rgba8_mct_ict_premultiplied_alpha_16x16.jp2",
        "synthetic_rgba8_mct_ict_premultiplied_alpha_16x16.rgb24",
        "premultiplied-alpha");
}

static uint8_t jp2_expected_composite_white(uint8_t color,
                                            uint8_t alpha,
                                            int premultiplied)
{
    if (premultiplied) {
        unsigned composite = (unsigned)color + (255u - alpha);
        return (uint8_t)(composite > 255u ? 255u : composite);
    }
    return (uint8_t)(((uint32_t)color * alpha +
                      255u * (255u - alpha) + 127u) / 255u);
}

static int check_internal_rgba_alpha_fixture(const char *fixture_name,
                                             int premultiplied)
{
    const uint32_t width = 16u;
    const uint32_t height = 16u;
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    int passed = 0;
    int rc;

    if (!fixture_name || !read_fixture(fixture_name, &jp2, &jp2_size)) {
        fputs("Unable to read a JP2 alpha fixture\n", stderr);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK || page.kind != QW_PAGE_IMAGE_RGB24 ||
        page.width != width || page.height != height || page.size != 768u) {
        fprintf(stderr,
                "Internal JP2 alpha decode failed: rc=%d, kind=%d, "
                "dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    for (uint32_t y = 0u; y < height; ++y) {
        for (uint32_t x = 0u; x < width; ++x) {
            size_t pixel = (size_t)y * width + x;
            uint8_t red = (uint8_t)(x * 13u + y * 3u);
            uint8_t green = (uint8_t)(x * 5u + y * 17u + 31u);
            uint8_t blue = (uint8_t)(x * 11u + y * 7u + 43u);
            uint8_t alpha = (uint8_t)(x * 17u + y * 29u);
            uint8_t expected[3];
            if (premultiplied) {
                red = (uint8_t)(((uint32_t)red * alpha + 127u) / 255u);
                green = (uint8_t)(((uint32_t)green * alpha + 127u) / 255u);
                blue = (uint8_t)(((uint32_t)blue * alpha + 127u) / 255u);
            }
            expected[0] = jp2_expected_composite_white(
                red, alpha, premultiplied);
            expected[1] = jp2_expected_composite_white(
                green, alpha, premultiplied);
            expected[2] = jp2_expected_composite_white(
                blue, alpha, premultiplied);
            for (size_t channel = 0u; channel < 3u; ++channel) {
                if (page.data[pixel * 3u + channel] != expected[channel]) {
                    fprintf(stderr,
                            "JP2 alpha pixel mismatch: file=%s, x=%u, y=%u, "
                            "channel=%zu, got=%u, expected=%u\n",
                            fixture_name, x, y, channel,
                            page.data[pixel * 3u + channel],
                            expected[channel]);
                    goto cleanup;
                }
            }
        }
    }
    qw_image_release_page(&page);
    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK && page.kind == QW_PAGE_IMAGE_RGB24 &&
             page.width == width && page.height == height && page.size == 768u;
    if (!passed) {
        fprintf(stderr,
                "Dispatched JP2 alpha decode failed: rc=%d, kind=%d, "
                "dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
    } else {
        for (uint32_t y = 0u; y < height && passed; ++y) {
            for (uint32_t x = 0u; x < width && passed; ++x) {
                size_t pixel = (size_t)y * width + x;
                uint8_t red = (uint8_t)(x * 13u + y * 3u);
                uint8_t green = (uint8_t)(x * 5u + y * 17u + 31u);
                uint8_t blue = (uint8_t)(x * 11u + y * 7u + 43u);
                uint8_t alpha = (uint8_t)(x * 17u + y * 29u);
                uint8_t expected[3];
                if (premultiplied) {
                    red = (uint8_t)(((uint32_t)red * alpha + 127u) / 255u);
                    green = (uint8_t)(((uint32_t)green * alpha + 127u) / 255u);
                    blue = (uint8_t)(((uint32_t)blue * alpha + 127u) / 255u);
                }
                expected[0] = jp2_expected_composite_white(
                    red, alpha, premultiplied);
                expected[1] = jp2_expected_composite_white(
                    green, alpha, premultiplied);
                expected[2] = jp2_expected_composite_white(
                    blue, alpha, premultiplied);
                for (size_t channel = 0u; channel < 3u; ++channel) {
                    if (page.data[pixel * 3u + channel] != expected[channel]) {
                        fprintf(stderr,
                                "Dispatched JP2 alpha pixel mismatch: "
                                "file=%s, x=%u, y=%u, channel=%zu\n",
                                fixture_name, x, y, channel);
                        passed = 0;
                        break;
                    }
                }
            }
        }
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_rgb_alpha(void)
{
    return check_internal_rgba_alpha_fixture(
               "synthetic_rgba8_alpha_16x16.jp2", 0) &&
           check_internal_rgba_alpha_fixture(
               "synthetic_rgba8_premultiplied_16x16.jp2", 1) &&
           check_internal_rgba_alpha_fixture(
               "synthetic_rgba8_mct_alpha_16x16.jp2", 0);
}

static int jp2_rgba_cdef_permuted_page_matches(const QwPageImage *page)
{
    if (!page || !page->data || page->kind != QW_PAGE_IMAGE_RGB24 ||
        page->width != 16u || page->height != 16u || page->size != 768u) {
        return 0;
    }
    for (uint32_t y = 0u; y < 16u; ++y) {
        for (uint32_t x = 0u; x < 16u; ++x) {
            size_t pixel = (size_t)y * 16u + x;
            uint8_t red = (uint8_t)(x * 13u + y * 3u);
            uint8_t green = (uint8_t)(x * 5u + y * 17u + 31u);
            uint8_t blue = (uint8_t)(x * 11u + y * 7u + 43u);
            uint8_t alpha = (uint8_t)(x * 17u + y * 29u);
            uint8_t expected[3] = {
                jp2_expected_composite_white(blue, alpha, 0),
                jp2_expected_composite_white(red, alpha, 0),
                jp2_expected_composite_white(green, alpha, 0)
            };
            for (size_t channel = 0u; channel < 3u; ++channel) {
                if (page->data[pixel * 3u + channel] != expected[channel]) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int jp2_rgba_cdef_alpha_component2_page_matches(
    const QwPageImage *page)
{
    if (!page || !page->data || page->kind != QW_PAGE_IMAGE_RGB24 ||
        page->width != 16u || page->height != 16u || page->size != 768u) {
        return 0;
    }
    for (uint32_t y = 0u; y < 16u; ++y) {
        for (uint32_t x = 0u; x < 16u; ++x) {
            size_t pixel = (size_t)y * 16u + x;
            uint8_t component0 = (uint8_t)(x * 13u + y * 3u);
            uint8_t component1 = (uint8_t)(x * 5u + y * 17u + 31u);
            uint8_t alpha = (uint8_t)(x * 11u + y * 7u + 43u);
            uint8_t component3 = (uint8_t)(x * 17u + y * 29u);
            uint8_t expected[3] = {
                jp2_expected_composite_white(component3, alpha, 0),
                jp2_expected_composite_white(component0, alpha, 0),
                jp2_expected_composite_white(component1, alpha, 0)
            };
            for (size_t channel = 0u; channel < 3u; ++channel) {
                if (page->data[pixel * 3u + channel] != expected[channel]) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int jp2_rgba_cdef_premultiplied_alpha_component2_page_matches(
    const QwPageImage *page)
{
    if (!page || !page->data || page->kind != QW_PAGE_IMAGE_RGB24 ||
        page->width != 16u || page->height != 16u || page->size != 768u) {
        return 0;
    }
    for (uint32_t y = 0u; y < 16u; ++y) {
        for (uint32_t x = 0u; x < 16u; ++x) {
            size_t pixel = (size_t)y * 16u + x;
            uint8_t red = (uint8_t)(x * 13u + y * 3u);
            uint8_t green = (uint8_t)(x * 5u + y * 17u + 31u);
            uint8_t blue = (uint8_t)(x * 11u + y * 7u + 43u);
            uint8_t alpha = (uint8_t)(x * 17u + y * 29u);
            uint8_t premultiplied_red = (uint8_t)(
                ((uint32_t)red * alpha + 127u) / 255u);
            uint8_t premultiplied_green = (uint8_t)(
                ((uint32_t)green * alpha + 127u) / 255u);
            uint8_t premultiplied_blue = (uint8_t)(
                ((uint32_t)blue * alpha + 127u) / 255u);
            uint8_t expected[3] = {
                jp2_expected_composite_white(premultiplied_red, alpha, 1),
                jp2_expected_composite_white(premultiplied_green, alpha, 1),
                jp2_expected_composite_white(premultiplied_blue, alpha, 1)
            };
            for (size_t channel = 0u; channel < 3u; ++channel) {
                if (page->data[pixel * 3u + channel] != expected[channel]) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int jp2_rgba_cdef_premultiplied_alpha_page_matches(
    const QwPageImage *page)
{
    if (!page || !page->data || page->kind != QW_PAGE_IMAGE_RGB24 ||
        page->width != 16u || page->height != 16u || page->size != 768u) {
        return 0;
    }
    for (uint32_t y = 0u; y < 16u; ++y) {
        for (uint32_t x = 0u; x < 16u; ++x) {
            size_t pixel = (size_t)y * 16u + x;
            uint8_t red = (uint8_t)(x * 13u + y * 3u);
            uint8_t green = (uint8_t)(x * 5u + y * 17u + 31u);
            uint8_t blue = (uint8_t)(x * 11u + y * 7u + 43u);
            uint8_t alpha = (uint8_t)(x * 17u + y * 29u);
            uint8_t premultiplied_red = (uint8_t)(
                ((uint32_t)red * alpha + 127u) / 255u);
            uint8_t premultiplied_green = (uint8_t)(
                ((uint32_t)green * alpha + 127u) / 255u);
            uint8_t premultiplied_blue = (uint8_t)(
                ((uint32_t)blue * alpha + 127u) / 255u);
            uint8_t expected[3] = {
                jp2_expected_composite_white(premultiplied_red, alpha, 1),
                jp2_expected_composite_white(premultiplied_green, alpha, 1),
                jp2_expected_composite_white(premultiplied_blue, alpha, 1)
            };
            for (size_t channel = 0u; channel < 3u; ++channel) {
                if (page->data[pixel * 3u + channel] != expected[channel]) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int jp2_rgba_cdef_straight_alpha_page_matches(const QwPageImage *page)
{
    if (!page || !page->data || page->kind != QW_PAGE_IMAGE_RGB24 ||
        page->width != 16u || page->height != 16u || page->size != 768u) {
        return 0;
    }
    for (uint32_t y = 0u; y < 16u; ++y) {
        for (uint32_t x = 0u; x < 16u; ++x) {
            size_t pixel = (size_t)y * 16u + x;
            uint8_t red = (uint8_t)(x * 13u + y * 3u);
            uint8_t green = (uint8_t)(x * 5u + y * 17u + 31u);
            uint8_t blue = (uint8_t)(x * 11u + y * 7u + 43u);
            uint8_t alpha = (uint8_t)(x * 17u + y * 29u);
            uint8_t expected[3] = {
                jp2_expected_composite_white(red, alpha, 0),
                jp2_expected_composite_white(green, alpha, 0),
                jp2_expected_composite_white(blue, alpha, 0)
            };
            for (size_t channel = 0u; channel < 3u; ++channel) {
                if (page->data[pixel * 3u + channel] != expected[channel]) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int jp2_public_rgb24_matches_page(const QwImage *image,
                                         const QwPageImage *page)
{
    size_t expected_stride;
    if (!image || !page || !image->pixels || !page->data ||
        page->kind != QW_PAGE_IMAGE_RGB24 ||
        (size_t)page->width > SIZE_MAX / 3u) {
        return 0;
    }
    expected_stride = (size_t)page->width * 3u;
    return image->format == QW_IMAGE_PIXEL_RGB24 &&
           image->width == page->width && image->height == page->height &&
           image->stride == expected_stride && image->size == page->size &&
           memcmp(image->pixels, page->data, page->size) == 0;
}

static int jp2_test_read_box(const uint8_t *data,
                             size_t parent_end,
                             size_t offset,
                             uint32_t *out_type,
                             size_t *out_payload_offset,
                             size_t *out_payload_size,
                             size_t *out_end_offset)
{
    uint32_t box_length;
    uint64_t resolved_length;
    size_t header_size = 8u;

    if (!data || !out_type || !out_payload_offset || !out_payload_size ||
        !out_end_offset || offset > parent_end ||
        parent_end - offset < 8u) {
        return 0;
    }
    box_length = read_be32(data + offset);
    *out_type = read_be32(data + offset + 4u);
    if (box_length == 1u) {
        if (parent_end - offset < 16u) return 0;
        resolved_length = ((uint64_t)read_be32(data + offset + 8u) << 32) |
                          (uint64_t)read_be32(data + offset + 12u);
        header_size = 16u;
    } else if (box_length == 0u) {
        resolved_length = (uint64_t)(parent_end - offset);
    } else {
        resolved_length = (uint64_t)box_length;
    }
    if (resolved_length < (uint64_t)header_size ||
        resolved_length > (uint64_t)(parent_end - offset) ||
        resolved_length > (uint64_t)SIZE_MAX) {
        return 0;
    }
    *out_payload_offset = offset + header_size;
    *out_payload_size = (size_t)resolved_length - header_size;
    *out_end_offset = offset + (size_t)resolved_length;
    return 1;
}

static int jp2_test_find_cdef_payload(const uint8_t *data,
                                      size_t size,
                                      size_t *out_payload_offset,
                                      size_t *out_payload_size)
{
    size_t offset = 0u;

    if (!data || !out_payload_offset || !out_payload_size) return 0;
    while (offset < size) {
        uint32_t type;
        size_t payload_offset;
        size_t payload_size;
        size_t end_offset;
        if (!jp2_test_read_box(data, size, offset, &type, &payload_offset,
                               &payload_size, &end_offset)) {
            return 0;
        }
        if (type == UINT32_C(0x6a703268)) {
            size_t child_offset = payload_offset;
            size_t parent_end = end_offset;
            while (child_offset < parent_end) {
                uint32_t child_type;
                size_t child_payload_offset;
                size_t child_payload_size;
                size_t child_end_offset;
                if (!jp2_test_read_box(data, parent_end, child_offset,
                                       &child_type, &child_payload_offset,
                                       &child_payload_size,
                                       &child_end_offset)) {
                    return 0;
                }
                if (child_type == UINT32_C(0x63646566)) {
                    *out_payload_offset = child_payload_offset;
                    *out_payload_size = child_payload_size;
                    return 1;
                }
                child_offset = child_end_offset;
            }
        }
        offset = end_offset;
    }
    return 0;
}

static int jp2_test_set_component3_alpha_type(uint8_t *data,
                                              size_t size,
                                              uint16_t expected_type,
                                              uint16_t replacement_type)
{
    size_t payload_offset;
    size_t payload_size;
    uint16_t entry_count;
    size_t changed_count = 0u;

    if (!data || !jp2_test_find_cdef_payload(data, size, &payload_offset,
                                              &payload_size) ||
        payload_size < 2u) {
        return 0;
    }
    entry_count = read_be16(data + payload_offset);
    if (entry_count == 0u || entry_count > 4u ||
        payload_size != 2u + (size_t)entry_count * 6u) {
        return 0;
    }
    for (uint16_t entry = 0u; entry < entry_count; ++entry) {
        size_t record_offset = payload_offset + 2u + (size_t)entry * 6u;
        uint16_t channel = read_be16(data + record_offset);
        uint16_t type = read_be16(data + record_offset + 2u);
        uint16_t association = read_be16(data + record_offset + 4u);
        if (channel == 3u && type == expected_type && association == 0u) {
            data[record_offset + 2u] = (uint8_t)(replacement_type >> 8u);
            data[record_offset + 3u] = (uint8_t)replacement_type;
            ++changed_count;
        }
    }
    return changed_count == 1u;
}

enum {
    JP2_TEST_CDEF_MUTATION_DUPLICATE_CHANNEL = 0,
    JP2_TEST_CDEF_MUTATION_OUT_OF_RANGE_CHANNEL,
    JP2_TEST_CDEF_MUTATION_COLOR_ASSOC_ZERO,
    JP2_TEST_CDEF_MUTATION_COLOR_ASSOC_OUT_OF_RANGE,
    JP2_TEST_CDEF_MUTATION_DUPLICATE_ALPHA
};

static int jp2_test_mutate_noncanonical_cdef(uint8_t *data,
                                             size_t size,
                                             unsigned mutation)
{
    size_t payload_offset;
    size_t payload_size;
    uint16_t definition_count;

    if (!data ||
        !jp2_test_find_cdef_payload(data, size, &payload_offset,
                                    &payload_size) ||
        payload_size != 26u) {
        return 0;
    }
    definition_count = read_be16(data + payload_offset);
    if (definition_count != 4u) return 0;

    switch (mutation) {
    case JP2_TEST_CDEF_MUTATION_DUPLICATE_CHANNEL: {
        uint16_t first_channel = read_be16(data + payload_offset + 2u);
        write_be16(data + payload_offset + 8u, first_channel);
        return 1;
    }
    case JP2_TEST_CDEF_MUTATION_OUT_OF_RANGE_CHANNEL:
        write_be16(data + payload_offset + 2u, definition_count);
        return 1;
    case JP2_TEST_CDEF_MUTATION_COLOR_ASSOC_ZERO:
    case JP2_TEST_CDEF_MUTATION_COLOR_ASSOC_OUT_OF_RANGE:
        for (uint16_t i = 0u; i < definition_count; ++i) {
            size_t entry_offset = payload_offset + 2u + (size_t)i * 6u;
            if (read_be16(data + entry_offset + 2u) == 0u) {
                uint16_t association =
                    mutation == JP2_TEST_CDEF_MUTATION_COLOR_ASSOC_ZERO
                        ? 0u : 4u;
                write_be16(data + entry_offset + 4u, association);
                return 1;
            }
        }
        return 0;
    case JP2_TEST_CDEF_MUTATION_DUPLICATE_ALPHA:
        for (uint16_t i = 0u; i < definition_count; ++i) {
            size_t entry_offset = payload_offset + 2u + (size_t)i * 6u;
            if (read_be16(data + entry_offset + 2u) == 0u) {
                write_be16(data + entry_offset + 2u, 1u);
                write_be16(data + entry_offset + 4u, 0u);
                return 1;
            }
        }
        return 0;
    default:
        return 0;
    }
}

static int jp2_test_permute_cdef_entries(uint8_t *data, size_t size)
{
    static const uint16_t source_order[4] = {2u, 0u, 3u, 1u};
    uint8_t entries[24];
    size_t payload_offset;
    size_t payload_size;

    if (!data ||
        !jp2_test_find_cdef_payload(data, size, &payload_offset,
                                    &payload_size) ||
        payload_size != 26u || read_be16(data + payload_offset) != 4u) {
        return 0;
    }
    memcpy(entries, data + payload_offset + 2u, sizeof(entries));
    for (size_t i = 0u; i < 4u; ++i) {
        memcpy(data + payload_offset + 2u + i * 6u,
               entries + (size_t)source_order[i] * 6u, 6u);
    }
    return 1;
}

static int check_internal_rgb_mct_ict_premultiplied_alpha_cdef_order(void)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    uint8_t *reference = NULL;
    size_t reference_size = 0u;
    int passed = 0;

    if (!read_fixture(
            "synthetic_rgba8_mct_ict_premultiplied_alpha_16x16.jp2",
            &jp2, &jp2_size) ||
        !read_fixture(
            "synthetic_rgba8_mct_ict_premultiplied_alpha_16x16.rgb24",
            &reference, &reference_size)) {
        fputs("Unable to read the ICT plus premultiplied-alpha CDEF vector\n",
              stderr);
        goto cleanup;
    }
    if (!jp2_test_permute_cdef_entries(jp2, jp2_size)) {
        fputs("Unable to permute the ICT plus alpha CDEF descriptions\n",
              stderr);
        goto cleanup;
    }
    passed = check_internal_rgb_mct_ict_alpha_data(
        jp2, jp2_size, reference, reference_size,
        "permuted premultiplied-alpha");

cleanup:
    free(reference);
    free(jp2);
    return passed;
}

static int check_internal_rgb_mct_ict_premultiplied_alpha_tiled(void)
{
    return check_internal_rgb_mct_ict_alpha_fixture(
        "synthetic_rgba8_mct_ict_premultiplied_alpha_16x16_tiles_8x8.jp2",
        "synthetic_rgba8_mct_ict_premultiplied_alpha_16x16_tiles_8x8.rgb24",
        "tiled premultiplied-alpha");
}

static int jp2_test_append_header_boxes(const uint8_t *data,
                                        size_t size,
                                        const uint8_t *boxes,
                                        size_t boxes_size,
                                        uint8_t **out_data,
                                        size_t *out_size)
{
    size_t offset = 0u;
    size_t header_offset = SIZE_MAX;
    size_t header_end = 0u;
    uint8_t *updated;

    if (!data || !size || !boxes || !boxes_size || !out_data || !out_size ||
        size > SIZE_MAX - boxes_size) {
        return 0;
    }
    *out_data = NULL;
    *out_size = 0u;
    while (offset < size) {
        uint32_t type;
        size_t payload_offset;
        size_t payload_size;
        size_t end_offset;
        if (!jp2_test_read_box(data, size, offset, &type, &payload_offset,
                               &payload_size, &end_offset)) {
            return 0;
        }
        if (type == UINT32_C(0x6a703268)) {
            if (header_offset != SIZE_MAX) return 0;
            header_offset = offset;
            header_end = end_offset;
        }
        offset = end_offset;
    }
    if (header_offset == SIZE_MAX ||
        read_be32(data + header_offset) != header_end - header_offset ||
        boxes_size > (size_t)UINT32_MAX - (header_end - header_offset)) {
        return 0;
    }
    updated = (uint8_t *)malloc(size + boxes_size);
    if (!updated) return 0;
    memcpy(updated, data, header_end);
    memcpy(updated + header_end, boxes, boxes_size);
    memcpy(updated + header_end + boxes_size, data + header_end,
           size - header_end);
    write_be32(updated + header_offset,
               (uint32_t)(header_end - header_offset + boxes_size));
    *out_data = updated;
    *out_size = size + boxes_size;
    return 1;
}

static void jp2_test_build_direct_component_mapping(uint8_t box[24])
{
    write_be32(box, 24u);
    write_be32(box + 4u, UINT32_C(0x636d6170));
    for (uint16_t component = 0u; component < 4u; ++component) {
        size_t entry_offset = 8u + (size_t)component * 4u;
        write_be16(box + entry_offset, component);
        box[entry_offset + 2u] = 0u;
        box[entry_offset + 3u] = 0u;
    }
}

static void jp2_test_build_palette_mapping_boxes(uint8_t boxes[37])
{
    uint8_t *palette = boxes;
    uint8_t *mapping = boxes + 13u;

    write_be32(palette, 13u);
    write_be32(palette + 4u, UINT32_C(0x70636c72));
    write_be16(palette + 8u, 1u);
    palette[10] = 1u;
    palette[11] = 7u;
    palette[12] = 128u;

    write_be32(mapping, 24u);
    write_be32(mapping + 4u, UINT32_C(0x636d6170));
    for (uint16_t component = 0u; component < 4u; ++component) {
        size_t entry_offset = 8u + (size_t)component * 4u;
        write_be16(mapping + entry_offset, component);
        mapping[entry_offset + 2u] = component == 3u ? 0u : 1u;
        mapping[entry_offset + 3u] = 0u;
    }
}

static int check_internal_rgba_cdef_permutation(void)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    int passed = 0;
    int rc;

    if (!read_fixture("synthetic_rgba8_cdef_permuted_16x16.jp2",
                      &jp2, &jp2_size)) {
        fputs("Unable to read the permuted CDEF JP2 fixture\n", stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "Permuted CDEF structure validation failed: rc=%d\n", rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK || !jp2_rgba_cdef_permuted_page_matches(&page)) {
        fprintf(stderr,
                "Internal permuted CDEF decode failed: rc=%d, kind=%d, "
                "dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);

    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK &&
             jp2_rgba_cdef_permuted_page_matches(&page);
    if (!passed) {
        fprintf(stderr,
                "Dispatched permuted CDEF decode failed: rc=%d, kind=%d, "
                "dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_rgba_cdef_alpha_component2(void)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    int passed = 0;
    int rc;

    if (!read_fixture("synthetic_rgba8_cdef_alpha_component2_16x16.jp2",
                      &jp2, &jp2_size)) {
        fputs("Unable to read the non-last alpha CDEF fixture\n", stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "Non-last alpha CDEF structure validation failed: rc=%d\n",
                rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK ||
        !jp2_rgba_cdef_alpha_component2_page_matches(&page)) {
        fprintf(stderr,
                "Internal non-last alpha CDEF decode failed: rc=%d, kind=%d, "
                "dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);

    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK &&
             jp2_rgba_cdef_alpha_component2_page_matches(&page);
    if (!passed) {
        fprintf(stderr,
                "Dispatched non-last alpha CDEF decode failed: rc=%d, kind=%d, "
                "dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_rgba_cdef_straight_alpha_fixture(
    const char *fixture_name)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    QwImage image = {0};
    int passed = 0;
    int rc;

    if (!fixture_name || !read_fixture(fixture_name, &jp2, &jp2_size)) {
        fputs("Unable to read a straight-alpha CDEF fixture\n", stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "Straight-alpha CDEF structure validation failed: file=%s, "
                "rc=%d\n", fixture_name, rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK ||
        !jp2_rgba_cdef_straight_alpha_page_matches(&page)) {
        fprintf(stderr,
                "Internal straight-alpha CDEF decode failed: file=%s, "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                fixture_name, rc, page.kind, page.width, page.height,
                page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);

    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK &&
             jp2_rgba_cdef_straight_alpha_page_matches(&page);
    if (!passed) {
        fprintf(stderr,
                "Dispatched straight-alpha CDEF decode failed: file=%s, "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                fixture_name, rc, page.kind, page.width, page.height,
                page.size);
    }
    if (passed) {
        rc = qw_image_decode(jp2, jp2_size, &image);
        passed = rc == QW_IMAGE_OK &&
                 jp2_public_rgb24_matches_page(&image, &page);
        if (!passed) {
            fprintf(stderr,
                    "Public straight-alpha CDEF decode failed: file=%s, "
                    "rc=%d, format=%d, dimensions=%ux%u, stride=%zu, "
                    "size=%zu\n",
                    fixture_name, rc, image.format, image.width, image.height,
                    image.stride, image.size);
        }
    }

cleanup:
    qw_image_free(&image);
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_rgba_cdef_straight_alpha_component0(void)
{
    return check_internal_rgba_cdef_straight_alpha_fixture(
        "synthetic_rgba8_cdef_straight_alpha_component0_16x16.jp2");
}

static int check_internal_rgba_cdef_straight_alpha_component1(void)
{
    return check_internal_rgba_cdef_straight_alpha_fixture(
        "synthetic_rgba8_cdef_straight_alpha_component1_16x16.jp2");
}

static int check_reduced_rgba_cdef_layout(const char *fixture_name)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    int rc;
    int passed = 0;

    if (!fixture_name || !read_fixture(fixture_name, &jp2, &jp2_size)) {
        fprintf(stderr, "Unable to read reduced CDEF fixture: %s\n",
                fixture_name ? fixture_name : "(null)");
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8_reduced(jp2, jp2_size, 1u, &page);
    passed = rc == QW_STATUS_OK && page.data &&
             page.kind == QW_PAGE_IMAGE_RGB24 && page.width == 8u &&
             page.height == 8u && page.size == 8u * 8u * 3u;
    if (!passed) {
        fprintf(stderr,
                "Reduced CDEF decode has an unexpected layout: file=%s, "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                fixture_name, rc, page.kind, page.width, page.height,
                page.size);
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_reduced_rgba_cdef_reference(
    const char *fixture_name,
    uint16_t expected_alpha_type)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    size_t cdef_payload_offset = 0u;
    size_t cdef_payload_size = 0u;
    uint16_t rgb_components[3] = {UINT16_MAX, UINT16_MAX, UINT16_MAX};
    uint16_t alpha_component = UINT16_MAX;
    uint16_t alpha_type = 0u;
    QwPageImage page = {0};
    TestOpenJpegMemoryStream memory = {0};
    opj_stream_t *stream = NULL;
    opj_codec_t *codec = NULL;
    opj_image_t *reference = NULL;
    opj_dparameters_t parameters;
    int rc;
    int passed = 0;

    if (!fixture_name || !read_fixture(fixture_name, &jp2, &jp2_size) ||
        !jp2_test_find_cdef_payload(jp2, jp2_size, &cdef_payload_offset,
                                    &cdef_payload_size) ||
        cdef_payload_size < 2u) {
        fprintf(stderr, "Unable to read reduced CDEF reference fixture: %s\n",
                fixture_name ? fixture_name : "(null)");
        goto cleanup;
    }
    {
        uint16_t entry_count = read_be16(jp2 + cdef_payload_offset);
        if (entry_count != 4u ||
            cdef_payload_size != 2u + (size_t)entry_count * 6u) {
            fputs("Reduced CDEF reference has an unexpected description "
                  "count\n", stderr);
            goto cleanup;
        }
        for (uint16_t entry = 0u; entry < entry_count; ++entry) {
            size_t offset = cdef_payload_offset + 2u + (size_t)entry * 6u;
            uint16_t component = read_be16(jp2 + offset);
            uint16_t type = read_be16(jp2 + offset + 2u);
            uint16_t association = read_be16(jp2 + offset + 4u);
            if (type == 0u && association >= 1u && association <= 3u &&
                rgb_components[association - 1u] == UINT16_MAX) {
                rgb_components[association - 1u] = component;
            } else if ((type == 1u || type == 2u) && association == 0u &&
                       alpha_component == UINT16_MAX) {
                alpha_component = component;
                alpha_type = type;
            } else {
                fputs("Reduced CDEF reference has unsupported channel "
                      "associations\n", stderr);
                goto cleanup;
            }
        }
    }
    if (alpha_component >= 4u || alpha_type != expected_alpha_type ||
        rgb_components[0] >= 4u || rgb_components[1] >= 4u ||
        rgb_components[2] >= 4u ||
        rgb_components[0] == rgb_components[1] ||
        rgb_components[0] == rgb_components[2] ||
        rgb_components[1] == rgb_components[2] ||
        alpha_component == rgb_components[0] ||
        alpha_component == rgb_components[1] ||
        alpha_component == rgb_components[2]) {
        fputs("Reduced CDEF reference channel map is incomplete\n", stderr);
        goto cleanup;
    }

    rc = qw_jp2_decode_internal_gray8_reduced(jp2, jp2_size, 1u, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != 8u ||
        page.height != 8u || page.size != 8u * 8u * 3u) {
        fprintf(stderr,
                "Reduced CDEF internal decode has an unexpected layout: "
                "file=%s, rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                fixture_name, rc, page.kind, page.width, page.height,
                page.size);
        goto cleanup;
    }

    memory.data = jp2;
    memory.size = jp2_size;
    stream = opj_stream_default_create(OPJ_TRUE);
    codec = opj_create_decompress(OPJ_CODEC_JP2);
    if (!stream || !codec) goto reference_decode_failed;
    opj_stream_set_user_data(stream, &memory, NULL);
    opj_stream_set_user_data_length(stream, (OPJ_UINT64)jp2_size);
    opj_stream_set_read_function(stream, test_openjpeg_read);
    opj_stream_set_skip_function(stream, test_openjpeg_skip);
    opj_stream_set_seek_function(stream, test_openjpeg_seek);
    opj_set_error_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_warning_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_info_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_default_decoder_parameters(&parameters);
    parameters.cp_reduce = 1u;
    if (!opj_setup_decoder(codec, &parameters) ||
        !opj_read_header(stream, codec, &reference) || !reference ||
        !opj_decode(codec, stream, reference) ||
        !opj_end_decompress(codec, stream)) {
        goto reference_decode_failed;
    }
    if (reference->x0 != 0u || reference->y0 != 0u ||
        reference->x1 != 16u || reference->y1 != 16u ||
        reference->numcomps != 4u) {
        fputs("Reduced CDEF OpenJPEG reference has an unexpected image "
              "canvas\n", stderr);
        goto cleanup;
    }
    for (OPJ_UINT32 component = 0u; component < reference->numcomps;
         ++component) {
        const opj_image_comp_t *plane = &reference->comps[component];
        if (!plane->data || plane->w != 8u || plane->h != 8u ||
            plane->prec != 8u || plane->sgnd) {
            fputs("Reduced CDEF OpenJPEG reference has an unsupported "
                  "component layout\n", stderr);
            goto cleanup;
        }
    }
    /* OpenJPEG applies JP2 CDEF ordering to opj_image_t: color channels are
     * exposed in RGB association order and opacity follows them. The CDEF
     * component indices above refer to the original codestream components. */
    for (size_t pixel = 0u; pixel < 8u * 8u; ++pixel) {
        int alpha_sample = reference->comps[3].data[pixel];
        uint8_t alpha;
        if (alpha_sample < 0 || alpha_sample > 255) {
            fputs("Reduced CDEF OpenJPEG alpha sample is out of range\n",
                  stderr);
            goto cleanup;
        }
        alpha = (uint8_t)alpha_sample;
        for (size_t channel = 0u; channel < 3u; ++channel) {
            int color_sample = reference->comps[channel].data[pixel];
            uint8_t expected;
            uint8_t actual = page.data[pixel * 3u + channel];
            if (color_sample < 0 || color_sample > 255) {
                fputs("Reduced CDEF OpenJPEG color sample is out of range\n",
                      stderr);
                goto cleanup;
            }
            expected = jp2_expected_composite_white(
                (uint8_t)color_sample, alpha, alpha_type == 2u);
            if (actual != expected) {
                fprintf(stderr,
                        "Reduced CDEF internal/OpenJPEG mismatch: file=%s, "
                        "pixel=%zu, channel=%zu, actual=%u, expected=%u, "
                        "reference-color=%d, alpha=%u, alpha-type=%u, "
                        "rgb-component=%u, alpha-component=%u, "
                        "planes=[%d,%d,%d,%d], output=[%u,%u,%u]\n",
                        fixture_name, pixel, channel, (unsigned)actual,
                        (unsigned)expected, color_sample, (unsigned)alpha,
                        (unsigned)alpha_type,
                        (unsigned)rgb_components[channel],
                        (unsigned)alpha_component,
                        reference->comps[0].data[pixel],
                        reference->comps[1].data[pixel],
                        reference->comps[2].data[pixel],
                        reference->comps[3].data[pixel],
                        (unsigned)page.data[pixel * 3u],
                        (unsigned)page.data[pixel * 3u + 1u],
                        (unsigned)page.data[pixel * 3u + 2u]);
                goto cleanup;
            }
        }
    }
    printf("Reduced CDEF %s matches OpenJPEG exactly at 8x8 RGB24\n",
           expected_alpha_type == 1u ? "straight alpha" :
                                       "associated alpha");
    passed = 1;
    goto cleanup;

reference_decode_failed:
    fprintf(stderr, "Could not decode reduced CDEF fixture %s with OpenJPEG\n",
            fixture_name);

cleanup:
    if (reference) opj_image_destroy(reference);
    if (codec) opj_destroy_codec(codec);
    if (stream) opj_stream_destroy(stream);
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_reduced_rgba_cdef_layouts(void)
{
    return check_reduced_rgba_cdef_layout(
               "synthetic_rgba8_cdef_straight_alpha_component0_16x16.jp2") &&
           check_reduced_rgba_cdef_reference(
               "synthetic_rgba8_cdef_straight_alpha_component0_16x16.jp2",
               1u) &&
           check_reduced_rgba_cdef_layout(
               "synthetic_rgba8_cdef_premultiplied_alpha_component2_16x16.jp2") &&
           check_reduced_rgba_cdef_reference(
               "synthetic_rgba8_cdef_premultiplied_alpha_component2_16x16.jp2",
               2u);
}

static uint32_t jp2_test_ceil_div_u32(uint32_t value, uint32_t divisor)
{
    return value / divisor + (value % divisor != 0u ? 1u : 0u);
}

static int check_internal_reduced_rgb_subsampling_fixture(
    const char *fixture_name,
    uint8_t chroma_x_sampling,
    uint8_t chroma_y_sampling,
    uint32_t image_x_origin,
    uint32_t image_y_origin)
{
    const uint32_t full_width = 16u;
    const uint32_t full_height = 16u;
    const uint8_t component_x_sampling[3] = {
        1u, chroma_x_sampling, chroma_x_sampling
    };
    const uint8_t component_y_sampling[3] = {
        1u, chroma_y_sampling, chroma_y_sampling
    };
    uint32_t full_component_x0[3];
    uint32_t full_component_y0[3];
    uint32_t full_component_width[3];
    uint32_t full_component_height[3];
    uint32_t reduced_component_width[3];
    uint32_t reduced_component_height[3];
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    uint8_t reduced_expected[8u * 8u * 3u];
    uint8_t full_expected[16u * 16u * 3u];
    QwPageImage page = {0};
    QwImage image = {0};
    TestOpenJpegMemoryStream memory = {0};
    opj_stream_t *stream = NULL;
    opj_codec_t *codec = NULL;
    opj_image_t *reference = NULL;
    opj_dparameters_t parameters;
    int rc;
    int passed = 0;

    if (!fixture_name || !chroma_x_sampling || !chroma_y_sampling ||
        !read_fixture(fixture_name, &jp2, &jp2_size)) {
        fputs("Unable to read the reduced subsampled-RGB fixture\n", stderr);
        goto cleanup;
    }
    for (size_t component = 0u; component < 3u; ++component) {
        uint32_t full_component_x1 = jp2_test_ceil_div_u32(
            image_x_origin + full_width, component_x_sampling[component]);
        uint32_t full_component_y1 = jp2_test_ceil_div_u32(
            image_y_origin + full_height, component_y_sampling[component]);
        full_component_x0[component] = jp2_test_ceil_div_u32(
            image_x_origin, component_x_sampling[component]);
        full_component_y0[component] = jp2_test_ceil_div_u32(
            image_y_origin, component_y_sampling[component]);
        full_component_width[component] =
            full_component_x1 - full_component_x0[component];
        full_component_height[component] =
            full_component_y1 - full_component_y0[component];
        reduced_component_width[component] =
            jp2_test_ceil_div_u32(full_component_x1, 2u) -
            jp2_test_ceil_div_u32(full_component_x0[component], 2u);
        reduced_component_height[component] =
            jp2_test_ceil_div_u32(full_component_y1, 2u) -
            jp2_test_ceil_div_u32(full_component_y0[component], 2u);
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "Subsampled-RGB structure validation failed: rc=%d\n", rc);
        goto cleanup;
    }

    memory.data = jp2;
    memory.size = jp2_size;
    stream = opj_stream_default_create(OPJ_TRUE);
    codec = opj_create_decompress(OPJ_CODEC_JP2);
    if (!stream || !codec) goto reference_decode_failed;
    opj_stream_set_user_data(stream, &memory, NULL);
    opj_stream_set_user_data_length(stream, (OPJ_UINT64)jp2_size);
    opj_stream_set_read_function(stream, test_openjpeg_read);
    opj_stream_set_skip_function(stream, test_openjpeg_skip);
    opj_stream_set_seek_function(stream, test_openjpeg_seek);
    opj_set_error_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_warning_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_info_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_default_decoder_parameters(&parameters);
    parameters.cp_reduce = 1u;
    if (!opj_setup_decoder(codec, &parameters) ||
        !opj_read_header(stream, codec, &reference) || !reference ||
        !opj_decode(codec, stream, reference) ||
        !opj_end_decompress(codec, stream)) {
        goto reference_decode_failed;
    }
    if (reference->x0 != image_x_origin ||
        reference->y0 != image_y_origin ||
        reference->x1 != image_x_origin + full_width ||
        reference->y1 != image_y_origin + full_height ||
        reference->numcomps != 3u ||
        !reference->comps[0].data || !reference->comps[1].data ||
        !reference->comps[2].data ||
        reference->comps[0].w != reduced_component_width[0] ||
        reference->comps[0].h != reduced_component_height[0] ||
        reference->comps[0].x0 != full_component_x0[0] ||
        reference->comps[0].y0 != full_component_y0[0] ||
        reference->comps[0].dx != component_x_sampling[0] ||
        reference->comps[0].dy != component_y_sampling[0] ||
        reference->comps[1].w != reduced_component_width[1] ||
        reference->comps[1].h != reduced_component_height[1] ||
        reference->comps[1].x0 != full_component_x0[1] ||
        reference->comps[1].y0 != full_component_y0[1] ||
        reference->comps[1].dx != component_x_sampling[1] ||
        reference->comps[1].dy != component_y_sampling[1] ||
        reference->comps[2].w != reduced_component_width[2] ||
        reference->comps[2].h != reduced_component_height[2] ||
        reference->comps[2].x0 != full_component_x0[2] ||
        reference->comps[2].y0 != full_component_y0[2] ||
        reference->comps[2].dx != component_x_sampling[2] ||
        reference->comps[2].dy != component_y_sampling[2]) {
        fputs("OpenJPEG reduced subsampled-RGB reference has an unexpected "
              "component layout\n", stderr);
        goto cleanup;
    }
    for (uint32_t y = 0u; y < 8u; ++y) {
        for (uint32_t x = 0u; x < 8u; ++x) {
            size_t output_pixel = (size_t)y * 8u + x;
            for (size_t channel = 0u; channel < 3u; ++channel) {
                const opj_image_comp_t *component = &reference->comps[channel];
                size_t sample_index =
                    (size_t)(y / component->dy) * component->w +
                    x / component->dx;
                int sample = component->data[sample_index];
                if (sample < 0 || sample > 255) {
                    fputs("OpenJPEG reduced subsampled-RGB sample is out of range\n",
                          stderr);
                    goto cleanup;
                }
                reduced_expected[output_pixel * 3u + channel] =
                    (uint8_t)sample;
            }
        }
    }
    opj_image_destroy(reference);
    reference = NULL;
    opj_destroy_codec(codec);
    codec = NULL;
    opj_stream_destroy(stream);
    stream = NULL;
    memory.position = 0u;
    stream = opj_stream_default_create(OPJ_TRUE);
    codec = opj_create_decompress(OPJ_CODEC_JP2);
    if (!stream || !codec) goto reference_decode_failed;
    opj_stream_set_user_data(stream, &memory, NULL);
    opj_stream_set_user_data_length(stream, (OPJ_UINT64)jp2_size);
    opj_stream_set_read_function(stream, test_openjpeg_read);
    opj_stream_set_skip_function(stream, test_openjpeg_skip);
    opj_stream_set_seek_function(stream, test_openjpeg_seek);
    opj_set_error_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_warning_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_info_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_default_decoder_parameters(&parameters);
    parameters.cp_reduce = 0u;
    if (!opj_setup_decoder(codec, &parameters) ||
        !opj_read_header(stream, codec, &reference) || !reference ||
        !opj_decode(codec, stream, reference) ||
        !opj_end_decompress(codec, stream)) {
        goto reference_decode_failed;
    }
    if (reference->x0 != image_x_origin ||
        reference->y0 != image_y_origin ||
        reference->x1 != image_x_origin + full_width ||
        reference->y1 != image_y_origin + full_height ||
        reference->numcomps != 3u ||
        !reference->comps[0].data || !reference->comps[1].data ||
        !reference->comps[2].data ||
        reference->comps[0].w != full_component_width[0] ||
        reference->comps[0].h != full_component_height[0] ||
        reference->comps[0].dx != component_x_sampling[0] ||
        reference->comps[0].dy != component_y_sampling[0] ||
        reference->comps[0].x0 != full_component_x0[0] ||
        reference->comps[0].y0 != full_component_y0[0] ||
        reference->comps[0].prec != 8u || reference->comps[0].sgnd != 0u ||
        reference->comps[1].w != full_component_width[1] ||
        reference->comps[1].h != full_component_height[1] ||
        reference->comps[1].dx != component_x_sampling[1] ||
        reference->comps[1].dy != component_y_sampling[1] ||
        reference->comps[1].x0 != full_component_x0[1] ||
        reference->comps[1].y0 != full_component_y0[1] ||
        reference->comps[1].prec != 8u || reference->comps[1].sgnd != 0u ||
        reference->comps[2].w != full_component_width[2] ||
        reference->comps[2].h != full_component_height[2] ||
        reference->comps[2].dx != component_x_sampling[2] ||
        reference->comps[2].dy != component_y_sampling[2] ||
        reference->comps[2].x0 != full_component_x0[2] ||
        reference->comps[2].y0 != full_component_y0[2] ||
        reference->comps[2].prec != 8u || reference->comps[2].sgnd != 0u) {
        fputs("OpenJPEG full-resolution subsampled-RGB reference has an "
              "unexpected component layout\n", stderr);
        goto cleanup;
    }

    for (uint32_t y = 0u; y < full_height; ++y) {
        for (uint32_t x = 0u; x < full_width; ++x) {
            size_t output_pixel = (size_t)y * full_width + x;
            for (size_t channel = 0u; channel < 3u; ++channel) {
                const opj_image_comp_t *component =
                    &reference->comps[channel];
                size_t sample_index =
                    (size_t)(y / component->dy) * component->w +
                    x / component->dx;
                int sample = component->data[sample_index];
                if (sample < 0 || sample > 255) {
                    fputs("OpenJPEG full-resolution subsampled-RGB sample "
                          "is out of range\n", stderr);
                    goto cleanup;
                }
                full_expected[output_pixel * 3u + channel] =
                    (uint8_t)sample;
            }
        }
    }

    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != full_width ||
        page.height != full_height || page.size != sizeof(full_expected) ||
        memcmp(page.data, full_expected, sizeof(full_expected)) != 0) {
        fprintf(stderr,
                "Full-resolution subsampled-RGB internal output differs "
                "from OpenJPEG: rc=%d, kind=%d, dimensions=%ux%u, "
                "size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);

    rc = qw_jp2_decode_internal_gray8_reduced(jp2, jp2_size, 1u, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != 8u ||
        page.height != 8u || page.size != sizeof(reduced_expected)) {
        fprintf(stderr,
                "Reduced subsampled-RGB internal decode failed: rc=%d, "
                "kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    if (memcmp(page.data, reduced_expected, sizeof(reduced_expected)) != 0) {
        fputs("Reduced subsampled-RGB internal output differs from OpenJPEG\n",
              stderr);
        goto cleanup;
    }
    qw_image_release_page(&page);

    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != 16u ||
        page.height != 16u || page.size != 16u * 16u * 3u) {
        fprintf(stderr,
                "Full-resolution subsampled-RGB page dispatch failed: rc=%d, "
                "kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    if (memcmp(page.data, full_expected, sizeof(full_expected)) != 0) {
        fputs("Full-resolution subsampled-RGB page output differs from "
              "OpenJPEG\n", stderr);
        goto cleanup;
    }
    rc = qw_image_decode(jp2, jp2_size, &image);
    passed = rc == QW_IMAGE_OK && image.format == QW_IMAGE_PIXEL_RGB24 &&
             image.width == 16u && image.height == 16u &&
             jp2_public_rgb24_matches_page(&image, &page);
    if (!passed) {
        fprintf(stderr,
                "Public subsampled-RGB output did not match the page decode: "
                "rc=%d, format=%d, dimensions=%ux%u, stride=%zu, size=%zu\n",
                rc, image.format, image.width, image.height,
                image.stride, image.size);
    } else {
        puts("Reduced and full-resolution subsampled-RGB internal output, "
             "page dispatch, and public RGB24 match OpenJPEG's "
             "component-grid expansion exactly");
    }
    goto cleanup;

reference_decode_failed:
    fputs("Could not decode the subsampled-RGB fixture with OpenJPEG\n",
          stderr);

cleanup:
    qw_image_free(&image);
    qw_image_release_page(&page);
    if (reference) opj_image_destroy(reference);
    if (codec) opj_destroy_codec(codec);
    if (stream) opj_stream_destroy(stream);
    free(jp2);
    return passed;
}

static int check_internal_reduced_rgb_subsampling_parity(void)
{
    return check_internal_reduced_rgb_subsampling_fixture(
               "synthetic_rgb8_no_mct_420_16x16.jp2", 2u, 2u, 0u, 0u) &&
           check_internal_reduced_rgb_subsampling_fixture(
               "synthetic_rgb8_no_mct_422_16x16.jp2", 2u, 1u, 0u, 0u) &&
           check_internal_reduced_rgb_subsampling_fixture(
               "synthetic_rgb8_no_mct_dx4_dy2_16x16.jp2", 4u, 2u, 0u, 0u) &&
           check_internal_reduced_rgb_subsampling_fixture(
               "synthetic_rgb8_no_mct_420_origin_8_8_16x16.jp2",
               2u, 2u, 8u, 8u) &&
           check_internal_reduced_rgb_subsampling_fixture(
               "synthetic_rgb8_no_mct_dx3_dy2_16x16.jp2",
               3u, 2u, 0u, 0u) &&
           check_internal_reduced_rgb_subsampling_fixture(
               "synthetic_rgb8_no_mct_dx5_dy3_16x16.jp2",
               5u, 3u, 0u, 0u);
}

static opj_image_t *openjpeg_test_decode_memory(const uint8_t *data,
                                                size_t size,
                                                OPJ_UINT32 reduction)
{
    TestOpenJpegMemoryStream memory = {data, size, 0u};
    opj_dparameters_t parameters;
    opj_image_t *image = NULL;
    opj_stream_t *stream = NULL;
    opj_codec_t *codec = NULL;
    OPJ_CODEC_FORMAT format;
    int decoded = 0;

    if (!data || !size) return NULL;
    format = size >= 2u && data[0] == 0xffu && data[1] == 0x4fu
        ? OPJ_CODEC_J2K : OPJ_CODEC_JP2;
    stream = opj_stream_default_create(OPJ_TRUE);
    codec = opj_create_decompress(format);
    if (!stream || !codec) goto cleanup;
    opj_stream_set_user_data(stream, &memory, NULL);
    opj_stream_set_user_data_length(stream, (OPJ_UINT64)size);
    opj_stream_set_read_function(stream, test_openjpeg_read);
    opj_stream_set_skip_function(stream, test_openjpeg_skip);
    opj_stream_set_seek_function(stream, test_openjpeg_seek);
    opj_set_error_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_warning_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_info_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_default_decoder_parameters(&parameters);
    parameters.cp_reduce = reduction;
    decoded = opj_setup_decoder(codec, &parameters) &&
              opj_read_header(stream, codec, &image) && image &&
              opj_decode(codec, stream, image) &&
              opj_end_decompress(codec, stream);

cleanup:
    if (codec) opj_destroy_codec(codec);
    if (stream) opj_stream_destroy(stream);
    if (!decoded) {
        if (image) opj_image_destroy(image);
        return NULL;
    }
    return image;
}

static opj_image_t *jp2_test_openjpeg_decode_memory(const uint8_t *data,
                                                    size_t size,
                                                    OPJ_UINT32 reduction)
{
    return openjpeg_test_decode_memory(data, size, reduction);
}

static int check_internal_rgb_reversible_mct(const char *fixture_name)
{
    uint8_t *data = NULL;
    size_t data_size = 0u;
    opj_image_t *reference = NULL;
    QwPageImage internal_page = {0};
    QwPageImage dispatched_page = {0};
    uint32_t width;
    uint32_t height;
    size_t pixel_count;
    int rc;
    int passed = 0;

    if (!fixture_name || !read_fixture(fixture_name, &data, &data_size)) {
        fprintf(stderr, "Unable to read the reversible RGB fixture: %s\n",
                fixture_name ? fixture_name : "(null)");
        goto cleanup;
    }
    reference = openjpeg_test_decode_memory(data, data_size, 0u);
    if (!reference || reference->numcomps != 3u ||
        reference->x1 <= reference->x0 || reference->y1 <= reference->y0) {
        fprintf(stderr, "OpenJPEG could not decode the reversible RGB fixture: %s\n",
                fixture_name);
        goto cleanup;
    }
    width = reference->x1 - reference->x0;
    height = reference->y1 - reference->y0;
    if (width != 31u || height != 32u ||
        (size_t)width > SIZE_MAX / (size_t)height) {
        fprintf(stderr, "Unexpected RGB reference dimensions for %s: %ux%u\n",
                fixture_name, width, height);
        goto cleanup;
    }
    pixel_count = (size_t)width * (size_t)height;
    for (uint32_t component = 0u; component < 3u; ++component) {
        const opj_image_comp_t *plane = &reference->comps[component];
        if (!plane->data || plane->w != width || plane->h != height ||
            plane->dx != 1u || plane->dy != 1u || plane->prec != 8u ||
            plane->sgnd != 0u) {
            fprintf(stderr,
                    "Unexpected RGB component %u metadata for %s\n",
                    component, fixture_name);
            goto cleanup;
        }
    }

    rc = qw_jp2_decode_internal_gray8(data, data_size, &internal_page);
    if (rc != QW_STATUS_OK ||
        internal_page.kind != QW_PAGE_IMAGE_RGB24 ||
        internal_page.width != width || internal_page.height != height ||
        internal_page.size != pixel_count * 3u || !internal_page.data) {
        fprintf(stderr,
                "Internal reversible RGB decode failed for %s: rc=%d, "
                "kind=%d, dimensions=%ux%u, size=%zu\n",
                fixture_name, rc, internal_page.kind, internal_page.width,
                internal_page.height, internal_page.size);
        goto cleanup;
    }
    {
        const uint8_t *pixels = (const uint8_t *)internal_page.data;
        for (size_t pixel = 0u; pixel < pixel_count; ++pixel) {
            for (uint32_t component = 0u; component < 3u; ++component) {
                int32_t sample = reference->comps[component].data[pixel];
                if (sample < 0 || sample > 255 ||
                    pixels[3u * pixel + component] != (uint8_t)sample) {
                    fprintf(stderr,
                            "Reversible RGB fixture %s differs from OpenJPEG "
                            "at pixel %zu component %u\n",
                            fixture_name, pixel, component);
                    goto cleanup;
                }
            }
        }
    }

    rc = qw_jp2_decode_page(data, data_size, &dispatched_page);
    if (rc != QW_STATUS_OK || dispatched_page.kind != internal_page.kind ||
        dispatched_page.width != width || dispatched_page.height != height ||
        dispatched_page.size != internal_page.size ||
        !dispatched_page.data ||
        memcmp(dispatched_page.data, internal_page.data, internal_page.size) !=
            0) {
        fprintf(stderr,
                "Reversible RGB page dispatch failed for %s: rc=%d, "
                "kind=%d, dimensions=%ux%u, size=%zu\n",
                fixture_name, rc, dispatched_page.kind, dispatched_page.width,
                dispatched_page.height, dispatched_page.size);
        goto cleanup;
    }
    printf("Internal reversible RGB fixture %s matches OpenJPEG and page "
           "dispatch (%zu bytes)\n", fixture_name, internal_page.size);
    passed = 1;

cleanup:
    if (reference) opj_image_destroy(reference);
    qw_image_release_page(&dispatched_page);
    qw_image_release_page(&internal_page);
    free(data);
    return passed;
}

static int jp2_test_rgba_sample_grid_reference(
    const opj_image_t *reference,
    uint32_t width,
    uint32_t height,
    const uint32_t component_dx[4],
    const uint32_t component_dy[4],
    uint32_t reference_x_origin,
    uint32_t reference_y_origin,
    uint8_t *out_rgb)
{
    if (!reference || !component_dx || !component_dy || !out_rgb ||
        reference->numcomps != 4u) {
        return 0;
    }
    for (size_t component_index = 0u; component_index < 4u;
         ++component_index) {
        const opj_image_comp_t *component =
            &reference->comps[component_index];
        if (!component->data || component_dx[component_index] == 0u ||
            component_dy[component_index] == 0u ||
            component->dx != component_dx[component_index] ||
            component->dy != component_dy[component_index] ||
            component->x0 != jp2_test_ceil_div_u32(
                                 reference_x_origin,
                                 component_dx[component_index]) ||
            component->y0 != jp2_test_ceil_div_u32(
                                 reference_y_origin,
                                 component_dy[component_index]) ||
            component->w != jp2_test_ceil_div_u32(
                               width, component_dx[component_index]) ||
            component->h != jp2_test_ceil_div_u32(
                               height, component_dy[component_index]) ||
            component->prec != 8u || component->sgnd != 0u ||
            component->alpha != (component_index == 3u ? 1u : 0u)) {
            fprintf(stderr,
                    "OpenJPEG component %zu grid: dx/dy=%u/%u, "
                    "x0/y0=%u/%u (expected %u/%u), dimensions=%ux%u, "
                    "precision=%u, signed=%u, alpha=%u\n",
                    component_index, component->dx, component->dy,
                    component->x0, component->y0,
                    jp2_test_ceil_div_u32(reference_x_origin,
                                          component_dx[component_index]),
                    jp2_test_ceil_div_u32(reference_y_origin,
                                          component_dy[component_index]),
                    component->w, component->h, component->prec,
                    component->sgnd, component->alpha);
            return 0;
        }
    }
    for (uint32_t y = 0u; y < height; ++y) {
        for (uint32_t x = 0u; x < width; ++x) {
            size_t output_pixel = (size_t)y * width + x;
            int color[3];
            int alpha;
            uint8_t expected[3];
            for (size_t channel = 0u; channel < 3u; ++channel) {
                const opj_image_comp_t *component =
                    &reference->comps[channel];
                size_t sample_index =
                    (size_t)(y / component->dy) * component->w +
                    x / component->dx;
                color[channel] = component->data[sample_index];
                if (color[channel] < 0 || color[channel] > 255) return 0;
            }
            {
                const opj_image_comp_t *component = &reference->comps[3];
                size_t sample_index =
                    (size_t)(y / component->dy) * component->w +
                    x / component->dx;
                alpha = component->data[sample_index];
            }
            if (alpha < 0 || alpha > 255) return 0;
            for (size_t channel = 0u; channel < 3u; ++channel) {
                expected[channel] = jp2_expected_composite_white(
                    (uint8_t)color[channel], (uint8_t)alpha, 0);
                out_rgb[output_pixel * 3u + channel] = expected[channel];
            }
        }
    }
    return 1;
}

static int check_internal_rgba_alpha_sampling_fixture(
    const char *fixture_name,
    uint32_t alpha_dx,
    uint32_t alpha_dy,
    uint32_t reference_x_origin,
    uint32_t reference_y_origin)
{
    const uint32_t width = 256u;
    const uint32_t height = 192u;
    const uint32_t reduced_width = 128u;
    const uint32_t reduced_height = 96u;
    const uint32_t component_dx[4] = {1u, 1u, 1u, alpha_dx};
    const uint32_t component_dy[4] = {1u, 1u, 1u, alpha_dy};
    uint8_t reduced_expected[128u * 96u * 3u];
    uint8_t full_expected[256u * 192u * 3u];
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    QwImage image = {0};
    opj_image_t *reference = NULL;
    int passed = 0;
    int rc;

    if (!fixture_name || alpha_dx == 0u || alpha_dy == 0u ||
        !read_fixture(fixture_name, &jp2, &jp2_size)) {
        fprintf(stderr, "Unable to read sampled-alpha fixture: %s\n",
                fixture_name ? fixture_name : "(null)");
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "Sampled-alpha structure validation failed: rc=%d\n",
                rc);
        goto cleanup;
    }
    reference = jp2_test_openjpeg_decode_memory(jp2, jp2_size, 1u);
    if (!reference || !jp2_test_rgba_sample_grid_reference(
            reference, reduced_width, reduced_height, component_dx,
            component_dy, reference_x_origin, reference_y_origin,
            reduced_expected)) {
        fputs("OpenJPEG reduced sampled-alpha reference has an unexpected "
              "component grid\n", stderr);
        goto cleanup;
    }
    opj_image_destroy(reference);
    reference = NULL;
    rc = qw_jp2_decode_internal_gray8_reduced(jp2, jp2_size, 1u, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != reduced_width ||
        page.height != reduced_height ||
        page.size != sizeof(reduced_expected) ||
        memcmp(page.data, reduced_expected, sizeof(reduced_expected)) != 0) {
        fprintf(stderr,
                "Reduced sampled-alpha output differs from OpenJPEG: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);

    reference = jp2_test_openjpeg_decode_memory(jp2, jp2_size, 0u);
    if (!reference || !jp2_test_rgba_sample_grid_reference(
            reference, width, height, component_dx, component_dy,
            reference_x_origin, reference_y_origin, full_expected)) {
        fputs("OpenJPEG full-resolution sampled-alpha reference has an "
              "unexpected component grid\n", stderr);
        goto cleanup;
    }
    opj_image_destroy(reference);
    reference = NULL;
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != width ||
        page.height != height || page.size != sizeof(full_expected) ||
        memcmp(page.data, full_expected, sizeof(full_expected)) != 0) {
        fprintf(stderr,
                "Internal full-resolution sampled-alpha output differs "
                "from OpenJPEG: rc=%d, kind=%d, dimensions=%ux%u, "
                "size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);
    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != width ||
        page.height != height || page.size != sizeof(full_expected) ||
        memcmp(page.data, full_expected, sizeof(full_expected)) != 0) {
        fprintf(stderr,
                "Full-resolution sampled-alpha output differs from OpenJPEG: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_image_decode(jp2, jp2_size, &image);
    passed = rc == QW_IMAGE_OK && image.format == QW_IMAGE_PIXEL_RGB24 &&
             image.width == width && image.height == height &&
             jp2_public_rgb24_matches_page(&image, &page);
    if (!passed) {
        fprintf(stderr,
                "Public sampled-alpha output differs from the page: "
                "rc=%d, format=%d, dimensions=%ux%u, stride=%zu, size=%zu\n",
                rc, image.format, image.width, image.height,
                image.stride, image.size);
    } else {
        printf("sampled-alpha dx/dy=%u/%u reduced, page, and public outputs "
               "match OpenJPEG exactly\n", alpha_dx, alpha_dy);
    }

cleanup:
    qw_image_free(&image);
    qw_image_release_page(&page);
    if (reference) opj_image_destroy(reference);
    free(jp2);
    return passed;
}

static int check_internal_rgba_alpha_sampling_5x3(void)
{
    return check_internal_rgba_alpha_sampling_fixture(
        "synthetic_rgba8_no_mct_alpha_dx5_dy3_256x192.jp2", 5u, 3u,
        0u, 0u);
}

static int check_internal_rgba_alpha_sampling_nonzero_origin(void)
{
    return check_internal_rgba_alpha_sampling_fixture(
        "synthetic_rgba8_no_mct_alpha_dx5_dy3_origin_precinct_aligned_256x192.jp2",
        5u, 3u, UINT32_C(5242880), UINT32_C(3145728));
}

static int check_internal_rgba_alpha_sampling_3x2(void)
{
    return check_internal_rgba_alpha_sampling_fixture(
        "synthetic_rgba8_no_mct_alpha_dx3_dy2_256x192.jp2", 3u, 2u,
        0u, 0u);
}

static int check_internal_rgba_alpha_sampling_4x2(void)
{
    return check_internal_rgba_alpha_sampling_fixture(
        "synthetic_rgba8_no_mct_alpha_dx4_dy2_256x192.jp2", 4u, 2u,
        0u, 0u);
}

static int check_internal_rgba_subsampled_alpha(void)
{
    const char *fixture_name =
        "synthetic_rgba8_no_mct_420_alpha_420_16x16.jp2";
    const uint32_t component_dx[4] = {1u, 2u, 2u, 2u};
    const uint32_t component_dy[4] = {1u, 2u, 2u, 2u};
    uint8_t reduced_expected[8u * 8u * 3u];
    uint8_t full_expected[16u * 16u * 3u];
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    QwImage image = {0};
    opj_image_t *reference = NULL;
    int passed = 0;
    int rc;

    if (!read_fixture(fixture_name, &jp2, &jp2_size)) {
        fprintf(stderr, "Unable to read sampled-alpha fixture: %s\n",
                fixture_name);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "Sampled-alpha structure validation failed: rc=%d\n",
                rc);
        goto cleanup;
    }

    reference = jp2_test_openjpeg_decode_memory(jp2, jp2_size, 1u);
    if (!reference ||
        !jp2_test_rgba_sample_grid_reference(reference, 8u, 8u,
                                             component_dx, component_dy,
                                             0u, 0u,
                                             reduced_expected)) {
        fputs("OpenJPEG reduced sampled-alpha reference has an unexpected "
              "component grid\n", stderr);
        goto cleanup;
    }
    opj_image_destroy(reference);
    reference = NULL;
    rc = qw_jp2_decode_internal_gray8_reduced(jp2, jp2_size, 1u, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != 8u ||
        page.height != 8u || page.size != sizeof(reduced_expected) ||
        memcmp(page.data, reduced_expected, sizeof(reduced_expected)) != 0) {
        fprintf(stderr,
                "Reduced sampled-alpha decode differs from OpenJPEG: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);

    reference = jp2_test_openjpeg_decode_memory(jp2, jp2_size, 0u);
    if (!reference ||
        !jp2_test_rgba_sample_grid_reference(reference, 16u, 16u,
                                             component_dx, component_dy,
                                             0u, 0u,
                                             full_expected)) {
        fputs("OpenJPEG full-resolution sampled-alpha reference has an "
              "unexpected component grid\n", stderr);
        goto cleanup;
    }
    opj_image_destroy(reference);
    reference = NULL;
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != 16u ||
        page.height != 16u || page.size != sizeof(full_expected) ||
        memcmp(page.data, full_expected, sizeof(full_expected)) != 0) {
        fprintf(stderr,
                "Full-resolution sampled-alpha internal decode differs "
                "from OpenJPEG: rc=%d, kind=%d, dimensions=%ux%u, "
                "size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);
    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != 16u ||
        page.height != 16u || page.size != sizeof(full_expected) ||
        memcmp(page.data, full_expected, sizeof(full_expected)) != 0) {
        fprintf(stderr,
                "Full-resolution sampled-alpha page differs from OpenJPEG: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_image_decode(jp2, jp2_size, &image);
    passed = rc == QW_IMAGE_OK && image.format == QW_IMAGE_PIXEL_RGB24 &&
             image.width == 16u && image.height == 16u &&
             jp2_public_rgb24_matches_page(&image, &page);
    if (!passed) {
        fprintf(stderr,
                "Public sampled-alpha decode differs from the page output: "
                "rc=%d, format=%d, dimensions=%ux%u, stride=%zu, size=%zu\n",
                rc, image.format, image.width, image.height,
                image.stride, image.size);
    } else {
        puts("Reduced and full-resolution sampled-alpha internal RGB, page "
             "dispatch, and public RGB24 match OpenJPEG's sampled "
             "component grid exactly");
    }

cleanup:
    qw_image_free(&image);
    qw_image_release_page(&page);
    if (reference) opj_image_destroy(reference);
    free(jp2);
    return passed;
}

static int check_internal_reduced_rgb_misaligned_origins_rejected(void)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    QwImage image = {0};
    TestOpenJpegMemoryStream memory = {0};
    opj_stream_t *stream = NULL;
    opj_codec_t *codec = NULL;
    opj_image_t *reference = NULL;
    opj_dparameters_t parameters;
    int rc;
    int passed = 0;

    if (!read_fixture("synthetic_rgb8_no_mct_420_origin_1_1_16x16.jp2",
                      &jp2, &jp2_size)) {
        fputs("Unable to read the misaligned-origin RGB fixture\n", stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "Misaligned-origin RGB fixture failed structural validation: "
                "rc=%d\n", rc);
        goto cleanup;
    }

    memory.data = jp2;
    memory.size = jp2_size;
    stream = opj_stream_default_create(OPJ_TRUE);
    codec = opj_create_decompress(OPJ_CODEC_JP2);
    if (!stream || !codec) goto reference_decode_failed;
    opj_stream_set_user_data(stream, &memory, NULL);
    opj_stream_set_user_data_length(stream, (OPJ_UINT64)jp2_size);
    opj_stream_set_read_function(stream, test_openjpeg_read);
    opj_stream_set_skip_function(stream, test_openjpeg_skip);
    opj_stream_set_seek_function(stream, test_openjpeg_seek);
    opj_set_error_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_warning_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_info_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_default_decoder_parameters(&parameters);
    if (!opj_setup_decoder(codec, &parameters) ||
        !opj_read_header(stream, codec, &reference) || !reference ||
        !opj_decode(codec, stream, reference) ||
        !opj_end_decompress(codec, stream)) {
        goto reference_decode_failed;
    }
    if (reference->x0 != 1u || reference->y0 != 1u ||
        reference->x1 != 17u || reference->y1 != 17u ||
        reference->numcomps != 3u ||
        !reference->comps[0].data || !reference->comps[1].data ||
        !reference->comps[2].data ||
        reference->comps[0].x0 == 0u || reference->comps[0].y0 == 0u ||
        reference->comps[1].x0 == 0u || reference->comps[1].y0 == 0u ||
        reference->comps[2].x0 == 0u || reference->comps[2].y0 == 0u) {
        fputs("OpenJPEG misaligned-origin RGB reference has an unexpected "
              "component layout\n", stderr);
        goto cleanup;
    }

    rc = qw_jp2_decode_internal_gray8_reduced(jp2, jp2_size, 1u, &page);
    if (rc != QW_STATUS_ERR_UNSUPPORTED || page.data || page.size != 0u ||
        page.width != 0u || page.height != 0u) {
        fprintf(stderr,
                "Reduced misaligned-origin RGB was not rejected cleanly: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);
    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    if (rc != QW_STATUS_ERR_UNSUPPORTED || page.data || page.size != 0u) {
        fprintf(stderr,
                "Misaligned-origin RGB page decode was not rejected cleanly: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_image_decode(jp2, jp2_size, &image);
    passed = rc == QW_IMAGE_ERR_UNSUPPORTED && !image.pixels &&
             image.size == 0u && image.width == 0u && image.height == 0u;
    if (!passed) {
        fprintf(stderr,
                "Public misaligned-origin RGB decode was not rejected cleanly: "
                "rc=%d, format=%d, dimensions=%ux%u, size=%zu\n",
                rc, image.format, image.width, image.height, image.size);
    } else {
        puts("Misaligned-origin RGB decodes in OpenJPEG and is cleanly rejected "
             "without output by internal reduction, page decode, and public "
             "dispatch");
    }
    goto cleanup;

reference_decode_failed:
    fputs("Could not decode the misaligned-origin RGB fixture with OpenJPEG\n",
          stderr);

cleanup:
    qw_image_free(&image);
    qw_image_release_page(&page);
    if (reference) opj_image_destroy(reference);
    if (codec) opj_destroy_codec(codec);
    if (stream) opj_stream_destroy(stream);
    free(jp2);
    return passed;
}

static int check_internal_reduced_rgb_tiles_rejected(void)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    int rc;
    int passed = 0;

    if (!read_fixture(
            "synthetic_rgba8_mct_ict_premultiplied_alpha_16x16_tiles_8x8.jp2",
            &jp2, &jp2_size)) {
        fputs("Unable to read tiled RGB/CDEF reduction fixture\n", stderr);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8_reduced(jp2, jp2_size, 1u, &page);
    passed = rc == QW_STATUS_ERR_UNSUPPORTED && !page.data;
    if (!passed) {
        fprintf(stderr,
                "Reduced tiled RGB/CDEF profile was not rejected cleanly: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_rgba_cdef_premultiplied_alpha_component2(void)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    QwImage image = {0};
    int passed = 0;
    int rc;

    if (!read_fixture(
            "synthetic_rgba8_cdef_premultiplied_alpha_component2_16x16.jp2",
            &jp2, &jp2_size)) {
        fputs("Unable to read the premultiplied non-last alpha CDEF fixture\n",
              stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "Premultiplied non-last alpha CDEF structure validation "
                "failed: rc=%d\n", rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK ||
        !jp2_rgba_cdef_premultiplied_alpha_component2_page_matches(&page)) {
        fprintf(stderr,
                "Internal premultiplied non-last alpha CDEF decode failed: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);

    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK &&
             jp2_rgba_cdef_premultiplied_alpha_component2_page_matches(&page);
    if (!passed) {
        fprintf(stderr,
                "Dispatched premultiplied non-last alpha CDEF decode failed: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
    }
    if (passed) {
        rc = qw_image_decode(jp2, jp2_size, &image);
        passed = rc == QW_IMAGE_OK &&
                 jp2_public_rgb24_matches_page(&image, &page);
        if (!passed) {
            fprintf(stderr,
                    "Public premultiplied non-last alpha CDEF decode failed: "
                    "rc=%d, format=%d, dimensions=%ux%u, stride=%zu, "
                    "size=%zu\n",
                    rc, image.format, image.width, image.height,
                    image.stride, image.size);
        }
    }

cleanup:
    qw_image_free(&image);
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_rgba_cdef_premultiplied_alpha_component1(void)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    QwImage image = {0};
    int passed = 0;
    int rc;

    if (!read_fixture(
            "synthetic_rgba8_cdef_premultiplied_alpha_component1_16x16.jp2",
            &jp2, &jp2_size)) {
        fputs("Unable to read the premultiplied alpha-component-1 CDEF fixture\n",
              stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "Premultiplied alpha-component-1 CDEF structure validation "
                "failed: rc=%d\n", rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK ||
        !jp2_rgba_cdef_premultiplied_alpha_page_matches(&page)) {
        fprintf(stderr,
                "Internal premultiplied alpha-component-1 CDEF decode failed: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);

    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK &&
             jp2_rgba_cdef_premultiplied_alpha_page_matches(&page);
    if (!passed) {
        fprintf(stderr,
                "Dispatched premultiplied alpha-component-1 CDEF decode "
                "failed: rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
    }
    if (passed) {
        rc = qw_image_decode(jp2, jp2_size, &image);
        passed = rc == QW_IMAGE_OK &&
                 jp2_public_rgb24_matches_page(&image, &page);
        if (!passed) {
            fprintf(stderr,
                    "Public premultiplied alpha-component-1 CDEF decode "
                    "failed: rc=%d, format=%d, dimensions=%ux%u, "
                    "stride=%zu, size=%zu\n",
                    rc, image.format, image.width, image.height,
                    image.stride, image.size);
        }
    }

cleanup:
    qw_image_free(&image);
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_rgba_cdef_premultiplied_alpha_component0(void)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    QwImage image = {0};
    int passed = 0;
    int rc;

    if (!read_fixture(
            "synthetic_rgba8_cdef_premultiplied_alpha_component0_16x16.jp2",
            &jp2, &jp2_size)) {
        fputs("Unable to read the premultiplied alpha-component-0 CDEF fixture\n",
              stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "Premultiplied alpha-component-0 CDEF structure validation "
                "failed: rc=%d\n", rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK ||
        !jp2_rgba_cdef_premultiplied_alpha_page_matches(&page)) {
        fprintf(stderr,
                "Internal premultiplied alpha-component-0 CDEF decode failed: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);

    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK &&
             jp2_rgba_cdef_premultiplied_alpha_page_matches(&page);
    if (!passed) {
        fprintf(stderr,
                "Dispatched premultiplied alpha-component-0 CDEF decode "
                "failed: rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
    }
    if (passed) {
        rc = qw_image_decode(jp2, jp2_size, &image);
        passed = rc == QW_IMAGE_OK &&
                 jp2_public_rgb24_matches_page(&image, &page);
        if (!passed) {
            fprintf(stderr,
                    "Public premultiplied alpha-component-0 CDEF decode "
                    "failed: rc=%d, format=%d, dimensions=%ux%u, "
                    "stride=%zu, size=%zu\n",
                    rc, image.format, image.width, image.height,
                    image.stride, image.size);
        }
    }

cleanup:
    qw_image_free(&image);
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_rgb_cdef_rejects_duplicate_association(void)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    size_t cdef_payload_offset;
    size_t cdef_payload_size;
    QwPageImage page = {0};
    int passed = 0;
    int rc;

    if (!read_fixture("synthetic_rgba8_cdef_permuted_16x16.jp2",
                      &jp2, &jp2_size)) {
        fputs("Unable to read the CDEF mutation fixture\n", stderr);
        goto cleanup;
    }
    if (!jp2_test_find_cdef_payload(jp2, jp2_size, &cdef_payload_offset,
                                    &cdef_payload_size) ||
        cdef_payload_size != 26u) {
        fputs("Unable to locate the CDEF payload in the mutation fixture\n",
              stderr);
        goto cleanup;
    }
    /* Entry 1 claims Asoc=1; change it to Asoc=2, already used by entry 2. */
    write_be16(jp2 + cdef_payload_offset + 2u + 6u + 4u, 2u);
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "CDEF association mutation damaged JP2 structure: rc=%d\n",
                rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_ERR_UNSUPPORTED && !page.data &&
             page.size == 0u;
    if (!passed) {
        fprintf(stderr,
                "Duplicate CDEF association was not rejected: rc=%d, "
                "kind=%d, size=%zu\n",
                rc, page.kind, page.size);
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_rgb_cdef_rejects_unknown_channel_type(void)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    size_t cdef_payload_offset;
    size_t cdef_payload_size;
    QwPageImage page = {0};
    QwImage public_image = {0};
    int passed = 0;
    int rc;

    if (!read_fixture("synthetic_rgba8_cdef_permuted_16x16.jp2",
                      &jp2, &jp2_size)) {
        fputs("Unable to read the CDEF type-mutation fixture\n", stderr);
        goto cleanup;
    }
    if (!jp2_test_find_cdef_payload(jp2, jp2_size, &cdef_payload_offset,
                                    &cdef_payload_size) ||
        cdef_payload_size != 26u) {
        fputs("Unable to locate the CDEF payload for type mutation\n", stderr);
        goto cleanup;
    }
    /* Entry 0 is the opacity channel; type 3 is outside this decoder subset. */
    write_be16(jp2 + cdef_payload_offset + 2u + 2u, 3u);
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "CDEF channel-type mutation damaged JP2 structure: rc=%d\n",
                rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_ERR_UNSUPPORTED && !page.data &&
             page.size == 0u;
    if (!passed) {
        fprintf(stderr,
                "Unsupported CDEF channel type was not rejected: rc=%d, "
                "kind=%d, size=%zu\n",
                rc, page.kind, page.size);
        goto cleanup;
    }
    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_ERR_UNSUPPORTED && !page.data &&
             page.size == 0u && page.width == 0u && page.height == 0u;
    if (!passed) {
        fprintf(stderr,
                "JP2 page dispatch did not preserve unsupported CDEF "
                "status: rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_image_decode(jp2, jp2_size, &public_image);
    passed = rc == QW_IMAGE_ERR_UNSUPPORTED && !public_image.pixels &&
             public_image.size == 0u && public_image.width == 0u &&
             public_image.height == 0u;
    if (!passed) {
        fprintf(stderr,
                "Public image API did not preserve unsupported CDEF "
                "status: rc=%d, dimensions=%ux%u, size=%zu\n",
                rc, public_image.width, public_image.height,
                public_image.size);
    }

cleanup:
    qw_image_free(&public_image);
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_rgb_cdef_rejects_nonzero_alpha_association(void)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    size_t cdef_payload_offset;
    size_t cdef_payload_size;
    QwPageImage page = {0};
    int passed = 0;
    int rc;

    if (!read_fixture(
            "synthetic_rgba8_cdef_straight_alpha_component0_16x16.jp2",
            &jp2, &jp2_size)) {
        fputs("Unable to read the CDEF association-mutation fixture\n",
              stderr);
        goto cleanup;
    }
    if (!jp2_test_find_cdef_payload(jp2, jp2_size, &cdef_payload_offset,
                                    &cdef_payload_size) ||
        cdef_payload_size != 26u) {
        fputs("Unable to locate the CDEF payload for association mutation\n",
              stderr);
        goto cleanup;
    }
    /* Entry 1 defines Typ=1 opacity at component 0; Asoc must remain zero. */
    write_be16(jp2 + cdef_payload_offset + 2u + 6u + 4u, 1u);
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "CDEF alpha-association mutation damaged JP2 structure: "
                "rc=%d\n", rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_ERR_UNSUPPORTED && !page.data &&
             page.size == 0u;
    if (!passed) {
        fprintf(stderr,
                "Nonzero CDEF alpha association was not rejected: rc=%d, "
                "kind=%d, size=%zu\n",
                rc, page.kind, page.size);
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_rgb_cdef_rejects_nonzero_premultiplied_alpha_association(
    void)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    size_t cdef_payload_offset;
    size_t cdef_payload_size;
    QwPageImage page = {0};
    int found_alpha = 0;
    int passed = 0;
    int rc;

    if (!read_fixture(
            "synthetic_rgba8_cdef_premultiplied_alpha_component0_16x16.jp2",
            &jp2, &jp2_size)) {
        fputs("Unable to read the premultiplied CDEF association fixture\n",
              stderr);
        goto cleanup;
    }
    if (!jp2_test_find_cdef_payload(jp2, jp2_size, &cdef_payload_offset,
                                    &cdef_payload_size) ||
        cdef_payload_size != 26u ||
        read_be16(jp2 + cdef_payload_offset) != 4u) {
        fputs("Unable to locate the premultiplied CDEF payload\n", stderr);
        goto cleanup;
    }
    for (uint16_t entry_index = 0u; entry_index < 4u; ++entry_index) {
        size_t entry_offset = cdef_payload_offset + 2u +
                              (size_t)entry_index * 6u;
        if (read_be16(jp2 + entry_offset + 2u) == 2u) {
            write_be16(jp2 + entry_offset + 4u, 1u);
            found_alpha = 1;
            break;
        }
    }
    if (!found_alpha) {
        fputs("Premultiplied opacity CDEF entry was not found\n", stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "Premultiplied CDEF alpha-association mutation damaged JP2 "
                "structure: rc=%d\n", rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_ERR_UNSUPPORTED && !page.data &&
             page.size == 0u;
    if (!passed) {
        fprintf(stderr,
                "Nonzero premultiplied CDEF alpha association was not "
                "rejected: rc=%d, kind=%d, size=%zu\n",
                rc, page.kind, page.size);
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_rgb_cdef_rejects_noncanonical_edges(void)
{
    static const struct {
        unsigned mutation;
        const char *name;
    } mutations[] = {
        {JP2_TEST_CDEF_MUTATION_DUPLICATE_CHANNEL,
         "duplicate channel index"},
        {JP2_TEST_CDEF_MUTATION_OUT_OF_RANGE_CHANNEL,
         "out-of-range channel index"},
        {JP2_TEST_CDEF_MUTATION_COLOR_ASSOC_ZERO,
         "zero color association"},
        {JP2_TEST_CDEF_MUTATION_COLOR_ASSOC_OUT_OF_RANGE,
         "out-of-range color association"},
        {JP2_TEST_CDEF_MUTATION_DUPLICATE_ALPHA,
         "multiple opacity channels"}
    };
    uint8_t *jp2 = NULL;
    uint8_t *original = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    int passed = 0;

    if (!read_fixture("synthetic_rgba8_cdef_permuted_16x16.jp2",
                      &jp2, &jp2_size)) {
        fputs("Unable to read the noncanonical CDEF fixture\n", stderr);
        goto cleanup;
    }
    original = (uint8_t *)malloc(jp2_size);
    if (!original) {
        fputs("Unable to copy the noncanonical CDEF fixture\n", stderr);
        goto cleanup;
    }
    memcpy(original, jp2, jp2_size);

    for (size_t i = 0u; i < sizeof(mutations) / sizeof(mutations[0]); ++i) {
        int rc;
        memcpy(jp2, original, jp2_size);
        if (!jp2_test_mutate_noncanonical_cdef(jp2, jp2_size,
                                               mutations[i].mutation)) {
            fprintf(stderr, "Unable to apply CDEF mutation: %s\n",
                    mutations[i].name);
            goto cleanup;
        }
        rc = qw_jp2_validate_structure(jp2, jp2_size);
        if (rc != QW_STATUS_OK) {
            fprintf(stderr,
                    "CDEF mutation damaged JP2 structure: case=%s, rc=%d\n",
                    mutations[i].name, rc);
            goto cleanup;
        }
        rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
        if (rc != QW_STATUS_ERR_UNSUPPORTED || page.data || page.size != 0u) {
            fprintf(stderr,
                    "Noncanonical CDEF was not rejected without output: "
                    "case=%s, rc=%d, kind=%d, size=%zu\n",
                    mutations[i].name, rc, page.kind, page.size);
            goto cleanup;
        }
        qw_image_release_page(&page);
        memset(&page, 0, sizeof(page));
    }
    passed = 1;

cleanup:
    qw_image_release_page(&page);
    free(original);
    free(jp2);
    return passed;
}

static uint8_t jp2_expected_precision_to_u8(unsigned sample,
                                            unsigned precision)
{
    unsigned maximum = (1u << precision) - 1u;
    return (uint8_t)((sample * 255u + maximum / 2u) / maximum);
}

static int jp2_rgb_mixed_precision_page_matches(const QwPageImage *page)
{
    if (!page || !page->data || page->kind != QW_PAGE_IMAGE_RGB24 ||
        page->width != 16u || page->height != 16u || page->size != 768u) {
        return 0;
    }
    for (unsigned y = 0u; y < 16u; ++y) {
        for (unsigned x = 0u; x < 16u; ++x) {
            size_t pixel = (size_t)y * 16u + x;
            uint8_t expected[3] = {
                jp2_expected_precision_to_u8((x * 3u + y * 5u + 1u) % 32u,
                                             5u),
                jp2_expected_precision_to_u8((x * 13u + y * 7u + 31u) % 256u,
                                             8u),
                jp2_expected_precision_to_u8((x * 31u + y * 17u + 83u) % 1024u,
                                             10u)
            };
            for (size_t channel = 0u; channel < 3u; ++channel) {
                if (page->data[pixel * 3u + channel] != expected[channel]) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int check_internal_rgba_mapping_rejected(const char *mapping_name,
                                                const uint8_t *boxes,
                                                size_t boxes_size)
{
    uint8_t *jp2 = NULL;
    uint8_t *mapped_jp2 = NULL;
    size_t jp2_size = 0u;
    size_t mapped_jp2_size = 0u;
    QwPageImage page = {0};
    int passed = 0;
    int rc;

    if (!mapping_name || !boxes || !boxes_size ||
        !read_fixture(
            "synthetic_rgba8_cdef_straight_alpha_component0_16x16.jp2",
            &jp2, &jp2_size)) {
        fputs("Unable to read an RGBA component-mapping fixture\n", stderr);
        goto cleanup;
    }
    if (!jp2_test_append_header_boxes(jp2, jp2_size, boxes, boxes_size,
                                      &mapped_jp2, &mapped_jp2_size)) {
        fprintf(stderr, "Unable to append %s boxes to the JP2 header\n",
                mapping_name);
        goto cleanup;
    }
    free(jp2);
    jp2 = mapped_jp2;
    jp2_size = mapped_jp2_size;
    mapped_jp2 = NULL;

    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "Valid %s mapping boxes failed JP2 structure validation: "
                "rc=%d\n", mapping_name, rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_ERR_UNSUPPORTED && !page.data &&
             page.size == 0u && page.width == 0u && page.height == 0u;
    if (!passed) {
        fprintf(stderr,
                "Internal decode did not reject %s mapping without output: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                mapping_name, rc, page.kind, page.width, page.height,
                page.size);
    }

cleanup:
    qw_image_release_page(&page);
    free(mapped_jp2);
    free(jp2);
    return passed;
}

static uint8_t jp2_test_openjpeg_sample_to_u8(
    const opj_image_comp_t *component,
    size_t pixel)
{
    uint64_t maximum;
    uint64_t value;
    int64_t sample;
    unsigned precision;

    if (!component || !component->data || component->prec == 0u ||
        component->prec > 32u) {
        return 0u;
    }
    precision = component->prec;
    maximum = (UINT64_C(1) << precision) - 1u;
    sample = component->data[pixel];
    if (component->sgnd) {
        int64_t center = (int64_t)(UINT64_C(1) << (precision - 1u));
        sample += center;
        if (sample < 0) sample = 0;
        value = (uint64_t)sample;
    } else {
        value = sample < 0 ? 0u : (uint64_t)sample;
    }
    if (value > maximum) value = maximum;
    return (uint8_t)((value * 255u + maximum / 2u) / maximum);
}

static int jp2_test_rgb24_reference_from_openjpeg(
    const opj_image_t *reference,
    uint32_t width,
    uint32_t height,
    uint8_t *out_rgb)
{
    if (!reference || !out_rgb || reference->numcomps != 3u ||
        reference->x1 < reference->x0 ||
        reference->y1 < reference->y0 ||
        reference->x1 - reference->x0 != width ||
        reference->y1 - reference->y0 != height) {
        return 0;
    }
    for (size_t channel = 0u; channel < 3u; ++channel) {
        const opj_image_comp_t *component = &reference->comps[channel];
        if (!component->data || component->w != width ||
            component->h != height || component->dx != 1u ||
            component->dy != 1u || component->prec == 0u ||
            component->prec > 32u) {
            return 0;
        }
    }
    for (size_t pixel = 0u; pixel < (size_t)width * height; ++pixel) {
        for (size_t channel = 0u; channel < 3u; ++channel) {
            out_rgb[pixel * 3u + channel] =
                jp2_test_openjpeg_sample_to_u8(&reference->comps[channel],
                                               pixel);
        }
    }
    return 1;
}

static int jp2_test_gray8_reference_from_openjpeg(
    const opj_image_t *reference,
    uint32_t width,
    uint32_t height,
    uint8_t *out_gray)
{
    const opj_image_comp_t *component;
    if (!reference || !out_gray || reference->numcomps != 1u ||
        reference->x1 < reference->x0 ||
        reference->y1 < reference->y0 ||
        reference->x1 - reference->x0 != width ||
        reference->y1 - reference->y0 != height) {
        return 0;
    }
    component = &reference->comps[0];
    if (!component->data || component->w != width ||
        component->h != height || component->dx != 1u ||
        component->dy != 1u || component->prec == 0u ||
        component->prec > 32u) {
        return 0;
    }
    for (size_t pixel = 0u; pixel < (size_t)width * height; ++pixel) {
        out_gray[pixel] = jp2_test_openjpeg_sample_to_u8(component, pixel);
    }
    return 1;
}

static int jp2_test_set_palette_mapping_columns(uint8_t *data,
                                                size_t size,
                                                const uint8_t columns[3])
{
    size_t offset = 0u;

    if (!data || !columns) return 0;
    for (size_t channel = 0u; channel < 3u; ++channel) {
        if (columns[channel] >= 3u) return 0;
    }
    while (offset < size) {
        uint32_t type;
        size_t payload_offset;
        size_t payload_size;
        size_t end_offset;
        if (!jp2_test_read_box(data, size, offset, &type, &payload_offset,
                               &payload_size, &end_offset)) {
            return 0;
        }
        if (type == UINT32_C(0x6a703268)) {
            size_t child_offset = payload_offset;
            while (child_offset < end_offset) {
                uint32_t child_type;
                size_t child_payload_offset;
                size_t child_payload_size;
                size_t child_end_offset;
                if (!jp2_test_read_box(data, end_offset, child_offset,
                                       &child_type, &child_payload_offset,
                                       &child_payload_size, &child_end_offset)) {
                    return 0;
                }
                if (child_type == UINT32_C(0x636d6170)) {
                    uint8_t *mapping = data + child_payload_offset;
                    if (child_payload_size != 12u) return 0;
                    for (size_t channel = 0u; channel < 3u; ++channel) {
                        mapping[channel * 4u + 3u] = columns[channel];
                    }
                    return 1;
                }
                child_offset = child_end_offset;
            }
            return 0;
        }
        offset = end_offset;
    }
    return 0;
}

static int jp2_test_set_palette_column_bpc(uint8_t *data,
                                          size_t size,
                                          uint8_t column,
                                          uint8_t bits)
{
    size_t offset = 0u;

    if (!data) return 0;
    while (offset < size) {
        uint32_t type;
        size_t payload_offset;
        size_t payload_size;
        size_t end_offset;
        if (!jp2_test_read_box(data, size, offset, &type, &payload_offset,
                               &payload_size, &end_offset)) {
            return 0;
        }
        if (type == UINT32_C(0x6a703268)) {
            size_t child_offset = payload_offset;
            while (child_offset < end_offset) {
                uint32_t child_type;
                size_t child_payload_offset;
                size_t child_payload_size;
                size_t child_end_offset;
                if (!jp2_test_read_box(data, end_offset, child_offset,
                                       &child_type, &child_payload_offset,
                                       &child_payload_size, &child_end_offset)) {
                    return 0;
                }
                if (child_type == UINT32_C(0x70636c72)) {
                    if (child_payload_size < 3u ||
                        column >= data[child_payload_offset + 2u] ||
                        child_payload_size < 3u + (size_t)column + 1u) {
                        return 0;
                    }
                    data[child_payload_offset + 3u + column] = bits;
                    return 1;
                }
                child_offset = child_end_offset;
            }
            return 0;
        }
        offset = end_offset;
    }
    return 0;
}

static int jp2_test_encode_palette_sample(uint8_t *output,
                                          uint8_t bits,
                                          uint8_t source_value)
{
    unsigned precision = (unsigned)(bits & 0x7fu) + 1u;
    size_t byte_count = ((size_t)precision + 7u) / 8u;
    uint64_t range;
    uint64_t encoded;

    if (!output || precision == 0u || precision > 38u) return 0;
    range = UINT64_C(1) << precision;
    if ((bits & 0x80u) != 0u) {
        int64_t minimum = -(int64_t)(range >> 1);
        int64_t signed_value = minimum +
            (int64_t)(((uint64_t)source_value * (range - 1u) + 127u) /
                      255u);
        encoded = (uint64_t)signed_value & (range - 1u);
    } else {
        encoded = ((uint64_t)source_value * (range - 1u) + 127u) / 255u;
    }
    for (size_t byte = 0u; byte < byte_count; ++byte) {
        output[byte_count - 1u - byte] = (uint8_t)encoded;
        encoded >>= 8;
    }
    return 1;
}

static int jp2_test_make_rgb_palette_variant(
    const uint8_t *data,
    size_t size,
    const uint8_t *palette_bits,
    const int16_t *source_columns,
    uint8_t column_count,
    const uint8_t mapping_columns[3],
    uint8_t **out_data,
    size_t *out_size)
{
    size_t offset = 0u;
    size_t header_start = SIZE_MAX;
    size_t header_end = 0u;
    size_t palette_start = SIZE_MAX;
    size_t palette_payload_offset = 0u;
    size_t palette_payload_size = 0u;
    size_t palette_end = 0u;
    size_t mapping_payload_offset = 0u;
    size_t mapping_payload_size = 0u;
    size_t old_palette_box_size;
    size_t new_palette_payload_size;
    size_t new_palette_box_size;
    size_t new_size;
    size_t new_entry_stride = 0u;
    uint16_t entry_count;
    uint8_t old_column_count;
    const uint8_t *old_entries;
    uint8_t *updated = NULL;
    uint8_t *new_palette = NULL;

    if (!data || !size || !palette_bits || !source_columns ||
        column_count == 0u || !mapping_columns || !out_data || !out_size) {
        return 0;
    }
    *out_data = NULL;
    *out_size = 0u;
    for (size_t channel = 0u; channel < 3u; ++channel) {
        if (mapping_columns[channel] >= column_count) return 0;
    }
    while (offset < size) {
        uint32_t type;
        size_t payload_offset;
        size_t payload_size;
        size_t end_offset;
        if (!jp2_test_read_box(data, size, offset, &type, &payload_offset,
                               &payload_size, &end_offset)) {
            return 0;
        }
        if (type == UINT32_C(0x6a703268)) {
            if (header_start != SIZE_MAX) return 0;
            header_start = offset;
            header_end = end_offset;
            size_t child_offset = payload_offset;
            while (child_offset < end_offset) {
                uint32_t child_type;
                size_t child_payload_offset;
                size_t child_payload_size;
                size_t child_end_offset;
                if (!jp2_test_read_box(data, end_offset, child_offset,
                                       &child_type, &child_payload_offset,
                                       &child_payload_size, &child_end_offset)) {
                    return 0;
                }
                if (child_type == UINT32_C(0x70636c72)) {
                    if (palette_start != SIZE_MAX) return 0;
                    palette_start = child_offset;
                    palette_payload_offset = child_payload_offset;
                    palette_payload_size = child_payload_size;
                    palette_end = child_end_offset;
                } else if (child_type == UINT32_C(0x636d6170)) {
                    if (mapping_payload_offset != 0u) return 0;
                    mapping_payload_offset = child_payload_offset;
                    mapping_payload_size = child_payload_size;
                }
                child_offset = child_end_offset;
            }
        }
        offset = end_offset;
    }
    if (header_start == SIZE_MAX || palette_start == SIZE_MAX ||
        mapping_payload_offset == 0u || mapping_payload_size != 12u ||
        palette_payload_size < 6u ||
        read_be32(data + header_start) != header_end - header_start) {
        return 0;
    }

    entry_count = read_be16(data + palette_payload_offset);
    old_column_count = data[palette_payload_offset + 2u];
    if (entry_count == 0u || old_column_count != 3u ||
        palette_payload_size != 6u + (size_t)entry_count * 3u ||
        data[palette_payload_offset + 3u] != 7u ||
        data[palette_payload_offset + 4u] != 7u ||
        data[palette_payload_offset + 5u] != 7u) {
        return 0;
    }
    old_entries = data + palette_payload_offset + 6u;
    for (uint16_t column = 0u; column < (uint16_t)column_count; ++column) {
        size_t width =
            (((size_t)(palette_bits[column] & 0x7fu) + 1u) + 7u) / 8u;
        if (new_entry_stride > SIZE_MAX - width) return 0;
        new_entry_stride += width;
    }
    if (new_entry_stride == 0u ||
        (size_t)entry_count >
            (SIZE_MAX - 3u - (size_t)column_count) / new_entry_stride) {
        return 0;
    }
    new_palette_payload_size = 3u + (size_t)column_count +
                               (size_t)entry_count * new_entry_stride;
    if (new_palette_payload_size > (size_t)UINT32_MAX - 8u) return 0;
    new_palette_box_size = 8u + new_palette_payload_size;
    old_palette_box_size = palette_end - palette_start;
    if (size < old_palette_box_size ||
        size - old_palette_box_size > SIZE_MAX - new_palette_box_size) {
        return 0;
    }
    new_size = size - old_palette_box_size + new_palette_box_size;
    if (header_end - header_start < old_palette_box_size ||
        header_end - header_start - old_palette_box_size >
            (size_t)UINT32_MAX - new_palette_box_size) {
        return 0;
    }

    new_palette = (uint8_t *)malloc(new_palette_box_size);
    updated = (uint8_t *)malloc(new_size);
    if (!new_palette || !updated) goto cleanup;
    write_be32(new_palette, (uint32_t)new_palette_box_size);
    write_be32(new_palette + 4u, UINT32_C(0x70636c72));
    write_be16(new_palette + 8u, entry_count);
    new_palette[10] = column_count;
    memcpy(new_palette + 11u, palette_bits, column_count);
    size_t output_offset = 11u + (size_t)column_count;
    for (uint16_t entry = 0u; entry < entry_count; ++entry) {
        for (uint16_t column = 0u; column < (uint16_t)column_count; ++column) {
            int16_t source_column = source_columns[column];
            uint8_t source_value = source_column >= 0 && source_column < 3
                ? old_entries[(size_t)entry * 3u + (size_t)source_column]
                : 0u;
            size_t width =
                (((size_t)(palette_bits[column] & 0x7fu) + 1u) + 7u) / 8u;
            if (!jp2_test_encode_palette_sample(
                    new_palette + output_offset, palette_bits[column],
                    source_value)) {
                goto cleanup;
            }
            output_offset += width;
        }
    }
    if (output_offset != new_palette_box_size) {
        goto cleanup;
    }

    memcpy(updated, data, palette_start);
    memcpy(updated + palette_start, new_palette, new_palette_box_size);
    memcpy(updated + palette_start + new_palette_box_size, data + palette_end,
           size - palette_end);
    write_be32(updated + header_start,
               (uint32_t)(header_end - header_start - old_palette_box_size +
                          new_palette_box_size));
    {
        ptrdiff_t delta = (ptrdiff_t)new_palette_box_size -
                          (ptrdiff_t)old_palette_box_size;
        size_t updated_mapping_offset = mapping_payload_offset;
        if (mapping_payload_offset >= palette_end) {
            if (delta >= 0) {
                updated_mapping_offset += (size_t)delta;
            } else {
                updated_mapping_offset -= (size_t)(-delta);
            }
        }
        for (size_t channel = 0u; channel < 3u; ++channel) {
            updated[updated_mapping_offset + channel * 4u + 3u] =
                mapping_columns[channel];
        }
    }
    *out_data = updated;
    *out_size = new_size;
    updated = NULL;

cleanup:
    free(updated);
    free(new_palette);
    return *out_data != NULL;
}

static int jp2_test_rgb_palette_mapping_columns(
    const uint8_t *jp2,
    size_t jp2_size,
    const uint8_t columns[3],
    const uint8_t *base_expected)
{
    const uint32_t width = 13u;
    const uint32_t height = 17u;
    uint8_t expected[13u * 17u * 3u];
    uint8_t *variant = NULL;
    QwPageImage page = {0};
    QwImage image = {0};
    int passed = 0;
    int rc;

    if (!jp2 || !base_expected || !columns) {
        fputs("Invalid RGB palette/cmap column test input\n", stderr);
        goto cleanup;
    }
    variant = (uint8_t *)malloc(jp2_size ? jp2_size : 1u);
    if (!variant) goto cleanup;
    memcpy(variant, jp2, jp2_size);
    if (!jp2_test_set_palette_mapping_columns(variant, jp2_size, columns)) {
        fputs("Unable to set palette/cmap output columns\n", stderr);
        goto cleanup;
    }
    for (size_t pixel = 0u; pixel < (size_t)width * height; ++pixel) {
        for (size_t channel = 0u; channel < 3u; ++channel) {
            expected[pixel * 3u + channel] =
                base_expected[pixel * 3u + columns[channel]];
        }
    }
    rc = qw_jp2_validate_structure(variant, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "RGB palette/cmap column selection failed: %d\n", rc);
        goto cleanup;
    }

    rc = qw_jp2_decode_internal_gray8(variant, jp2_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != width ||
        page.height != height || page.size != sizeof(expected) ||
        memcmp(page.data, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "Internal reordered palette/cmap differs from OpenJPEG: %d\n",
                rc);
        goto cleanup;
    }
    qw_image_release_page(&page);

    rc = qw_jp2_decode_page(variant, jp2_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != width ||
        page.height != height || page.size != sizeof(expected) ||
        memcmp(page.data, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "Page reordered palette/cmap differs from OpenJPEG: %d\n",
                rc);
        goto cleanup;
    }

    rc = qw_image_decode(variant, jp2_size, &image);
    passed = rc == QW_IMAGE_OK && image.width == width &&
             image.height == height &&
             jp2_public_rgb24_matches_page(&image, &page) &&
             memcmp(page.data, expected, sizeof(expected)) == 0;
    if (!passed) {
        fprintf(stderr, "Public reordered palette/cmap differs from OpenJPEG: %d\n",
                rc);
    }

cleanup:
    qw_image_free(&image);
    qw_image_release_page(&page);
    free(variant);
    return passed;
}

static int check_internal_rgb_palette_mapping(void)
{
    const uint32_t width = 13u;
    const uint32_t height = 17u;
    uint8_t expected[13u * 17u * 3u];
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    QwImage image = {0};
    opj_image_t *reference = NULL;
    int passed = 0;
    int rc;
    static const uint8_t cmap_column_cases[][3] = {
        {2u, 1u, 0u},
        {2u, 1u, 2u},
    };

    if (!read_fixture("synthetic_gray8_palette_rgb_cmap_13x17.jp2",
                      &jp2, &jp2_size)) {
        fputs("Unable to read the RGB palette/cmap JP2 fixture\n", stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "RGB palette/cmap structure validation failed: %d\n",
                rc);
        goto cleanup;
    }
    reference = jp2_test_openjpeg_decode_memory(jp2, jp2_size, 0u);
    if (!jp2_test_rgb24_reference_from_openjpeg(
            reference, width, height, expected)) {
        fputs("OpenJPEG palette/cmap output has an unexpected RGB layout\n",
              stderr);
        goto cleanup;
    }
    opj_image_destroy(reference);
    reference = NULL;

    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != width ||
        page.height != height || page.size != sizeof(expected) ||
        memcmp(page.data, expected, sizeof(expected)) != 0) {
        fprintf(stderr,
                "Internal RGB palette/cmap output differs from OpenJPEG: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);
    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != width ||
        page.height != height || page.size != sizeof(expected) ||
        memcmp(page.data, expected, sizeof(expected)) != 0) {
        fprintf(stderr,
                "Palette/cmap dispatch output differs from OpenJPEG: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_image_decode(jp2, jp2_size, &image);
    passed = rc == QW_IMAGE_OK && image.format == QW_IMAGE_PIXEL_RGB24 &&
             image.width == width && image.height == height &&
             jp2_public_rgb24_matches_page(&image, &page) &&
             memcmp(page.data, expected, sizeof(expected)) == 0;
    if (!passed) {
        fprintf(stderr,
                "Public palette/cmap output differs from OpenJPEG: "
                "rc=%d, format=%d, dimensions=%ux%u, stride=%zu, size=%zu\n",
                rc, image.format, image.width, image.height,
                image.stride, image.size);
    } else {
        puts("single-component RGB palette/cmap internal, page, and public "
             "outputs match OpenJPEG exactly");
    }
    if (passed) {
        for (size_t i = 0u;
             i < sizeof(cmap_column_cases) / sizeof(cmap_column_cases[0]);
             ++i) {
            passed = jp2_test_rgb_palette_mapping_columns(
                jp2, jp2_size, cmap_column_cases[i], expected);
            if (!passed) break;
        }
        if (passed) puts("RGB palette/cmap column permutations and repeated "
                         "columns match the CMAP-order reference derived "
                         "from OpenJPEG");
    }

cleanup:
    if (reference) opj_image_destroy(reference);
    qw_image_free(&image);
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_rgb_palette_column_precision(void)
{
    const uint32_t width = 13u;
    const uint32_t height = 17u;
    static const uint8_t palette_bits[] = {9u, 0xa5u, 7u, 9u, 0u, 37u};
    static const int16_t source_columns[] = {0, 1, 1, 2, 0, 2};
    static const uint8_t mapping_columns[] = {3u, 1u, 0u};
    static const uint8_t reference_channels[] = {2u, 1u, 0u};
    uint8_t expected[13u * 17u * 3u];
    uint8_t openjpeg_output[13u * 17u * 3u];
    uint8_t *base = NULL;
    size_t base_size = 0u;
    uint8_t *variant = NULL;
    size_t variant_size = 0u;
    QwPageImage page = {0};
    QwImage image = {0};
    opj_image_t *reference = NULL;
    opj_image_t *base_reference = NULL;
    int passed = 0;
    int rc;

    if (!read_fixture("synthetic_gray8_palette_rgb_cmap_13x17.jp2",
                      &base, &base_size)) {
        fputs("Unable to read the RGB palette/cmap precision fixture\n",
              stderr);
        goto cleanup;
    }
    if (!jp2_test_make_rgb_palette_variant(
            base, base_size, palette_bits, source_columns,
            (uint8_t)(sizeof(palette_bits) / sizeof(palette_bits[0])),
            mapping_columns, &variant, &variant_size)) {
        fputs("Unable to construct a mixed-precision RGB palette fixture\n",
              stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(variant, variant_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "Mixed-precision palette structure failed: %d\n", rc);
        goto cleanup;
    }
    base_reference = jp2_test_openjpeg_decode_memory(base, base_size, 0u);
    if (!jp2_test_rgb24_reference_from_openjpeg(
            base_reference, width, height, expected)) {
        fputs("OpenJPEG base palette output has an unexpected RGB layout\n",
              stderr);
        goto cleanup;
    }
    opj_image_destroy(base_reference);
    base_reference = NULL;
    for (size_t pixel = 0u; pixel < (size_t)width * height; ++pixel) {
        uint8_t canonical[3] = {
            expected[pixel * 3u], expected[pixel * 3u + 1u],
            expected[pixel * 3u + 2u]
        };
        for (size_t channel = 0u; channel < 3u; ++channel) {
            expected[pixel * 3u + channel] =
                canonical[reference_channels[channel]];
        }
    }
    reference = jp2_test_openjpeg_decode_memory(variant, variant_size, 0u);
    if (!reference) {
        puts("OpenJPEG cannot decode the legal signed 38-bit palette; "
             "expected RGB is derived from the base palette and T.800 CMAP "
             "channel order");
    } else {
        if (!jp2_test_rgb24_reference_from_openjpeg(
                reference, width, height, openjpeg_output)) {
            puts("OpenJPEG output for the legal signed 38-bit palette has no "
                 "normalizable RGB24 reference layout; expected RGB is "
                 "derived from the base palette and T.800 CMAP channel order");
        } else if (memcmp(openjpeg_output, expected, sizeof(expected)) != 0) {
            fputs("OpenJPEG mixed-precision output disagrees with the "
                  "T.800-derived palette output\n", stderr);
            goto cleanup;
        }
        opj_image_destroy(reference);
        reference = NULL;
    }

    rc = qw_jp2_decode_internal_gray8(variant, variant_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != width ||
        page.height != height || page.size != sizeof(expected) ||
        memcmp(page.data, expected, sizeof(expected)) != 0) {
        fprintf(stderr,
                "Internal mixed-precision palette differs from OpenJPEG: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);

    rc = qw_jp2_decode_page(variant, variant_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != width ||
        page.height != height || page.size != sizeof(expected) ||
        memcmp(page.data, expected, sizeof(expected)) != 0) {
        fprintf(stderr,
                "Dispatched mixed-precision palette differs from OpenJPEG: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_image_decode(variant, variant_size, &image);
    passed = rc == QW_IMAGE_OK && image.format == QW_IMAGE_PIXEL_RGB24 &&
             image.width == width && image.height == height &&
             jp2_public_rgb24_matches_page(&image, &page) &&
             memcmp(page.data, expected, sizeof(expected)) == 0;
    if (!passed) {
        fprintf(stderr,
                "Public mixed-precision palette differs from OpenJPEG: "
                "rc=%d, format=%d, dimensions=%ux%u, stride=%zu, size=%zu\n",
                rc, image.format, image.width, image.height,
                image.stride, image.size);
    } else {
        puts("RGB palette/cmap 1-, 10-, 8-, and signed 38-bit columns "
             "match the T.800-derived reference through internal, page, "
             "and public decode");
    }

cleanup:
    if (reference) opj_image_destroy(reference);
    if (base_reference) opj_image_destroy(base_reference);
    qw_image_free(&image);
    qw_image_release_page(&page);
    free(variant);
    free(base);
    return passed;
}

static int check_internal_rgb_palette_reserved_precision_rejected(void)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    QwImage image = {0};
    int passed = 0;
    int rc;

    if (!read_fixture("synthetic_gray8_palette_rgb_cmap_13x17.jp2",
                      &jp2, &jp2_size)) {
        fputs("Unable to read the RGB palette/cmap reserved-precision fixture\n",
              stderr);
        goto cleanup;
    }
    if (!jp2_test_set_palette_column_bpc(jp2, jp2_size, 2u, 38u)) {
        fputs("Unable to set the reserved 39-bit palette precision\n", stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_ERR_UNSUPPORTED) {
        fprintf(stderr, "Reserved 39-bit palette precision status was %d\n", rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    if (rc != QW_STATUS_ERR_UNSUPPORTED || page.data || page.size != 0u ||
        page.width != 0u || page.height != 0u) {
        fprintf(stderr,
                "Internal decode did not reject reserved palette precision "
                "cleanly: rc=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    if (rc != QW_STATUS_ERR_UNSUPPORTED || page.data || page.size != 0u ||
        page.width != 0u || page.height != 0u) {
        fprintf(stderr,
                "Page dispatch did not reject reserved palette precision "
                "cleanly: rc=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_image_decode(jp2, jp2_size, &image);
    passed = rc == QW_IMAGE_ERR_UNSUPPORTED && !image.pixels &&
             image.size == 0u && image.width == 0u && image.height == 0u;
    if (!passed) {
        fprintf(stderr,
                "Public decode did not reject reserved palette precision "
                "cleanly: rc=%d, dimensions=%ux%u, size=%zu\n",
                rc, image.width, image.height, image.size);
    } else {
        puts("reserved 39-bit PCLR precision is rejected as unsupported "
             "without output through internal, page, and public paths");
    }

cleanup:
    qw_image_free(&image);
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_gray_palette_mapping(void)
{
    const uint32_t width = 13u;
    const uint32_t height = 17u;
    uint8_t expected[13u * 17u];
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    QwImage image = {0};
    opj_image_t *reference = NULL;
    int passed = 0;
    int rc;

    if (!read_fixture("synthetic_gray8_palette_gray_cmap_13x17.jp2",
                      &jp2, &jp2_size)) {
        fputs("Unable to read the gray palette/cmap JP2 fixture\n", stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "gray palette/cmap structure validation failed: %d\n",
                rc);
        goto cleanup;
    }
    reference = jp2_test_openjpeg_decode_memory(jp2, jp2_size, 0u);
    if (!jp2_test_gray8_reference_from_openjpeg(
            reference, width, height, expected)) {
        fputs("OpenJPEG gray palette/cmap output has an unexpected layout\n",
              stderr);
        goto cleanup;
    }
    opj_image_destroy(reference);
    reference = NULL;

    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_GRAY8 || page.width != width ||
        page.height != height || page.size != sizeof(expected) ||
        memcmp(page.data, expected, sizeof(expected)) != 0) {
        fprintf(stderr,
                "Internal gray palette/cmap output differs from OpenJPEG: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);
    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_GRAY8 || page.width != width ||
        page.height != height || page.size != sizeof(expected) ||
        memcmp(page.data, expected, sizeof(expected)) != 0) {
        fprintf(stderr,
                "Gray palette/cmap page output differs from OpenJPEG: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_image_decode(jp2, jp2_size, &image);
    passed = rc == QW_IMAGE_OK && image.format == QW_IMAGE_PIXEL_GRAY8 &&
             image.width == width && image.height == height &&
             image.stride == width && image.size == sizeof(expected) &&
             memcmp(image.pixels, expected, sizeof(expected)) == 0 &&
             memcmp(page.data, expected, sizeof(expected)) == 0;
    if (!passed) {
        fprintf(stderr,
                "Public gray palette/cmap output differs from OpenJPEG: "
                "rc=%d, format=%d, dimensions=%ux%u, stride=%zu, size=%zu\n",
                rc, image.format, image.width, image.height,
                image.stride, image.size);
    } else {
        puts("single-component gray palette/cmap internal, page, and public "
             "outputs match OpenJPEG exactly");
    }

cleanup:
    if (reference) opj_image_destroy(reference);
    qw_image_free(&image);
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_gray_palette_column_precision(void)
{
    const uint32_t width = 13u;
    const uint32_t height = 17u;
    uint8_t expected[13u * 17u];
    uint8_t *base = NULL;
    size_t base_size = 0u;
    uint8_t *variant = NULL;
    size_t variant_size = 0u;
    QwPageImage page = {0};
    QwImage image = {0};
    opj_image_t *reference = NULL;
    int passed = 0;
    int rc;

    if (!read_fixture("synthetic_gray8_palette_gray_cmap_13x17.jp2",
                      &base, &base_size) ||
        !read_fixture("synthetic_gray8_palette_varprec_gray_cmap_13x17.jp2",
                      &variant, &variant_size)) {
        fputs("Unable to read the mixed-precision gray palette fixtures\n",
              stderr);
        goto cleanup;
    }
    reference = jp2_test_openjpeg_decode_memory(base, base_size, 0u);
    if (!jp2_test_gray8_reference_from_openjpeg(
            reference, width, height, expected)) {
        fputs("OpenJPEG base gray palette output has an unexpected layout\n",
              stderr);
        goto cleanup;
    }
    opj_image_destroy(reference);
    reference = NULL;

    rc = qw_jp2_validate_structure(variant, variant_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "Mixed-precision gray palette structure failed: %d\n",
                rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(variant, variant_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_GRAY8 || page.width != width ||
        page.height != height || page.size != sizeof(expected) ||
        memcmp(page.data, expected, sizeof(expected)) != 0) {
        fprintf(stderr,
                "Internal mixed-precision gray palette differs from the "
                "T.800-derived base: rc=%d, kind=%d, dimensions=%ux%u, "
                "size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);

    rc = qw_jp2_decode_page(variant, variant_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_GRAY8 || page.width != width ||
        page.height != height || page.size != sizeof(expected) ||
        memcmp(page.data, expected, sizeof(expected)) != 0) {
        fprintf(stderr,
                "Dispatched mixed-precision gray palette differs from the "
                "T.800-derived base: rc=%d, kind=%d, dimensions=%ux%u, "
                "size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_image_decode(variant, variant_size, &image);
    passed = rc == QW_IMAGE_OK && image.format == QW_IMAGE_PIXEL_GRAY8 &&
             image.width == width && image.height == height &&
             image.stride == width && image.size == sizeof(expected) &&
             memcmp(image.pixels, expected, sizeof(expected)) == 0 &&
             memcmp(page.data, expected, sizeof(expected)) == 0;
    if (!passed) {
        fprintf(stderr,
                "Public mixed-precision gray palette differs from the "
                "T.800-derived base: rc=%d, format=%d, dimensions=%ux%u, "
                "stride=%zu, size=%zu\n",
                rc, image.format, image.width, image.height,
                image.stride, image.size);
    } else {
        puts("gray palette/cmap unsigned 1-, unsigned 10-, and signed 38-bit "
             "columns with CMAP selection of column 2 match through internal, "
             "page, and public decode");
    }

cleanup:
    if (reference) opj_image_destroy(reference);
    qw_image_free(&image);
    qw_image_release_page(&page);
    free(variant);
    free(base);
    return passed;
}

static int check_internal_rgb_cmap_rejected(void)
{
    uint8_t mapping_box[24];
    jp2_test_build_direct_component_mapping(mapping_box);
    return check_internal_rgba_mapping_rejected(
        "direct cmap", mapping_box, sizeof(mapping_box));
}

static int check_internal_rgb_palette_mapping_rejected(void)
{
    uint8_t mapping_boxes[37];
    jp2_test_build_palette_mapping_boxes(mapping_boxes);
    return check_internal_rgba_mapping_rejected(
        "pclr/cmap palette", mapping_boxes, sizeof(mapping_boxes));
}

static int check_internal_rgb_mixed_precision(void)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    int passed = 0;
    int rc;

    if (!read_fixture("synthetic_rgb8_mixed_precision_16x16.jp2",
                      &jp2, &jp2_size)) {
        fputs("Unable to read the mixed-precision RGB JP2 fixture\n", stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "Mixed-precision RGB structure validation failed: rc=%d\n",
                rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK &&
             jp2_rgb_mixed_precision_page_matches(&page);
    if (!passed) {
        fprintf(stderr,
                "Internal mixed-precision RGB failed: rc=%d, kind=%d, "
                "dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);
    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK &&
             jp2_rgb_mixed_precision_page_matches(&page);
    if (!passed) {
        fprintf(stderr,
                "Dispatched mixed-precision RGB failed: rc=%d, kind=%d, "
                "dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int jp2_test_mark_rgb_components_signed(uint8_t *data, size_t size)
{
    const uint32_t jp2_header_type = UINT32_C(0x6a703268);
    const uint32_t image_header_type = UINT32_C(0x69686472);
    const uint32_t component_bits_type = UINT32_C(0x62706363);
    const uint32_t codestream_type = UINT32_C(0x6a703263);
    size_t offset = 0u;
    size_t image_bpc_offset = SIZE_MAX;
    size_t component_bits_offset = SIZE_MAX;
    size_t component_bits_size = 0u;
    size_t codestream_offset = SIZE_MAX;
    size_t codestream_size = 0u;
    uint8_t image_bpc = 0u;

    if (!data || !size) return 0;
    while (offset < size) {
        uint32_t type;
        size_t payload_offset;
        size_t payload_size;
        size_t end_offset;
        if (!jp2_test_read_box(data, size, offset, &type, &payload_offset,
                               &payload_size, &end_offset)) {
            return 0;
        }
        if (type == jp2_header_type) {
            size_t child_offset = payload_offset;
            while (child_offset < end_offset) {
                uint32_t child_type;
                size_t child_payload_offset;
                size_t child_payload_size;
                size_t child_end_offset;
                if (!jp2_test_read_box(
                        data, end_offset, child_offset, &child_type,
                        &child_payload_offset, &child_payload_size,
                        &child_end_offset)) {
                    return 0;
                }
                if (child_type == image_header_type) {
                    if (image_bpc_offset != SIZE_MAX ||
                        child_payload_size < 11u) {
                        return 0;
                    }
                    image_bpc_offset = child_payload_offset + 10u;
                    image_bpc = data[image_bpc_offset];
                } else if (child_type == component_bits_type) {
                    if (component_bits_offset != SIZE_MAX) return 0;
                    component_bits_offset = child_payload_offset;
                    component_bits_size = child_payload_size;
                }
                child_offset = child_end_offset;
            }
            if (child_offset != end_offset) return 0;
        } else if (type == codestream_type) {
            if (codestream_offset != SIZE_MAX) return 0;
            codestream_offset = payload_offset;
            codestream_size = payload_size;
        }
        offset = end_offset;
    }
    if (offset != size || image_bpc_offset == SIZE_MAX ||
        codestream_offset == SIZE_MAX || codestream_size < 42u) {
        return 0;
    }
    {
        uint8_t *codestream = data + codestream_offset;
        size_t siz_offset = find_first_marker(codestream, codestream_size,
                                              0u, 0x51u);
        size_t component_offset;
        if (siz_offset == codestream_size ||
            siz_offset > codestream_size - 42u ||
            read_be16(codestream + siz_offset + 38u) != 3u) {
            return 0;
        }
        component_offset = siz_offset + 40u;
        if (component_offset > codestream_size ||
            codestream_size - component_offset < 9u) {
            return 0;
        }
        if (image_bpc == 0xffu) {
            if (component_bits_offset == SIZE_MAX ||
                component_bits_size != 3u) {
                return 0;
            }
            for (size_t component = 0u; component < 3u; ++component) {
                size_t component_bpc_offset = component_offset + 3u * component;
                uint8_t container_bpc = data[component_bits_offset + component];
                uint8_t codestream_bpc = codestream[component_bpc_offset];
                if ((container_bpc & 0x80u) != 0u ||
                    (codestream_bpc & 0x80u) != 0u ||
                    (container_bpc & 0x7fu) == 0u ||
                    (container_bpc & 0x7fu) != (codestream_bpc & 0x7fu)) {
                    return 0;
                }
                data[component_bits_offset + component] |= 0x80u;
            }
        } else {
            if (component_bits_offset != SIZE_MAX ||
                (image_bpc & 0x80u) != 0u ||
                (image_bpc & 0x7fu) == 0u) {
                return 0;
            }
            for (size_t component = 0u; component < 3u; ++component) {
                size_t component_bpc_offset = component_offset + 3u * component;
                uint8_t codestream_bpc = codestream[component_bpc_offset];
                if ((codestream_bpc & 0x80u) != 0u ||
                    (codestream_bpc & 0x7fu) != (image_bpc & 0x7fu)) {
                    return 0;
                }
            }
            data[image_bpc_offset] |= 0x80u;
        }
        for (size_t component = 0u; component < 3u; ++component) {
            size_t component_bpc_offset = component_offset + 3u * component;
            codestream[component_bpc_offset] |= 0x80u;
        }
    }
    return 1;
}

static int check_internal_rgb_signed_srgb_rejected(
    const char *fixture_name, const char *label)
{
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    QwPageImage page = {0};
    int rc;
    int passed = 0;

    if (!fixture_name || !label || !read_fixture(fixture_name, &jp2, &jp2_size) ||
        !jp2_test_mark_rgb_components_signed(jp2, jp2_size)) {
        fprintf(stderr, "Unable to build the signed %s sRGB vector\n", label);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "Signed %s sRGB structure validation failed: rc=%d\n",
                label, rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_ERR_UNSUPPORTED && !page.data &&
             page.size == 0u && page.width == 0u && page.height == 0u;
    if (!passed) {
        fprintf(stderr,
                "Signed %s sRGB must return unsupported without output: "
                "rc=%d, kind=%d, dimensions=%ux%u, size=%zu\n",
                label, rc, page.kind, page.width, page.height, page.size);
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    return passed;
}

static int check_internal_rgb_signed_components(void)
{
    return check_internal_rgb_signed_srgb_rejected(
        "synthetic_rgb8_no_mct_16x16.jp2", "8-bit");
}

static int check_internal_rgb_signed_mixed_precision(void)
{
    return check_internal_rgb_signed_srgb_rejected(
        "synthetic_rgb8_mixed_precision_16x16.jp2", "mixed-precision");
}

static int check_tiled_ict_straight_alpha(void);
static int check_tiled_ict_partial_edge_tiles(void);

static int check_internal_rgb_mct(void)
{
    return check_internal_rgb_direct() && check_reduced_rgb53_reference() &&
           check_internal_reduced_rgb_subsampling_parity() &&
           check_internal_rgba_subsampled_alpha() &&
           check_internal_rgba_alpha_sampling_5x3() &&
            check_internal_rgba_alpha_sampling_nonzero_origin() &&
           check_internal_rgba_alpha_sampling_3x2() &&
           check_internal_rgba_alpha_sampling_4x2() &&
           check_internal_reduced_rgb_misaligned_origins_rejected() &&
           check_internal_rgb_mct_rct() &&
           check_internal_rgb_mct_ict() &&
           check_internal_rgb_mct_ict_straight_alpha() &&
           check_internal_rgb_mct_ict_premultiplied_alpha() &&
           check_internal_rgb_mct_ict_premultiplied_alpha_cdef_order() &&
           check_internal_rgb_mct_ict_premultiplied_alpha_tiled() &&
           check_tiled_ict_straight_alpha() &&
           check_tiled_ict_partial_edge_tiles() &&
           check_internal_reduced_rgb_tiles_rejected() &&
           check_internal_rgb_alpha() &&
           check_internal_rgba_cdef_permutation() &&
           check_internal_rgba_cdef_alpha_component2() &&
           check_internal_rgba_cdef_straight_alpha_component0() &&
           check_internal_rgba_cdef_straight_alpha_component1() &&
           check_internal_rgba_cdef_premultiplied_alpha_component2() &&
           check_internal_reduced_rgba_cdef_layouts() &&
           check_internal_rgba_cdef_premultiplied_alpha_component1() &&
           check_internal_rgba_cdef_premultiplied_alpha_component0() &&
           check_internal_rgb_cdef_rejects_duplicate_association() &&
           check_internal_rgb_cdef_rejects_unknown_channel_type() &&
           check_internal_rgb_cdef_rejects_nonzero_alpha_association() &&
           check_internal_rgb_cdef_rejects_nonzero_premultiplied_alpha_association() &&
           check_internal_rgb_cdef_rejects_noncanonical_edges() &&
           check_internal_rgb_palette_mapping() &&
           check_internal_rgb_palette_column_precision() &&
           check_internal_rgb_palette_reserved_precision_rejected() &&
           check_internal_gray_palette_mapping() &&
            check_internal_gray_palette_column_precision() &&
           check_internal_rgb_cmap_rejected() &&
           check_internal_rgb_palette_mapping_rejected() &&
           check_internal_rgb_mixed_precision() &&
           check_internal_rgb_signed_components() &&
           check_internal_rgb_signed_mixed_precision();
}

static int check_internal_irreversible_gray8_multitile_phase(void)
{
    const uint32_t width = 20u;
    const uint32_t height = 14u;
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    uint8_t *overridden_codestream = NULL;
    size_t overridden_codestream_size = 0u;
    uint8_t *reference = NULL;
    size_t reference_size = 0u;
    uint8_t *tile_qcd_reference = NULL;
    size_t tile_qcd_reference_size = 0u;
    int passed = 0;

    if (!read_fixture(
            "synthetic_gray8_origin2x2_20x14_tiles8x8_97.j2k",
            &codestream, &codestream_size) ||
        !read_fixture(
            "synthetic_gray8_origin2x2_20x14_tiles8x8_97_reference.gray8",
            &reference, &reference_size) ||
        !read_fixture(
            "synthetic_gray8_origin2x2_20x14_tiles8x8_97_tileqcd_reference.gray8",
            &tile_qcd_reference, &tile_qcd_reference_size) ||
        reference_size != (size_t)width * height ||
        tile_qcd_reference_size != reference_size) {
        fputs("Unable to read synthetic 9/7 multi-tile reference vectors\n",
              stderr);
        goto cleanup;
    }
    passed = check_gray8_codestream_matches_reference(
        codestream, codestream_size, reference, reference_size,
        width, height, "main-QCD reference");
    if (!passed) goto cleanup;

    overridden_codestream = make_t803_quantization_override_codestream(
        codestream, codestream_size, 0x5cu, 1,
        &overridden_codestream_size);
    if (!overridden_codestream ||
        memcmp(reference, tile_qcd_reference, reference_size) == 0) {
        fputs("Unable to create observable tile-QCD override vector\n",
              stderr);
        passed = 0;
        goto cleanup;
    }
    passed = check_gray8_codestream_matches_reference(
        overridden_codestream, overridden_codestream_size,
        tile_qcd_reference, tile_qcd_reference_size,
        width, height, "tile-QCD override");

cleanup:
    free(overridden_codestream);
    free(reference);
    free(codestream);
    free(tile_qcd_reference);
    return passed;
}

static int check_internal_subsampled_gray8_multitile_fixture(
    const char *codestream_name,
    const char *reference_name,
    uint32_t image_width,
    uint32_t image_height,
    uint8_t x_sampling,
    uint8_t y_sampling,
    uint32_t component_width,
    uint32_t component_height)
{
    const size_t expected_size =
        (size_t)component_width * (size_t)component_height;
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    uint8_t *reference = NULL;
    size_t reference_size = 0u;
    uint8_t *jp2 = NULL;
    size_t jp2_size;
    QwPageImage page = {0};
    int passed = 0;
    int rc;

    if (!read_fixture(codestream_name, &codestream, &codestream_size) ||
        !read_fixture(reference_name, &reference, &reference_size) ||
        reference_size != expected_size || codestream_size < 45u ||
        codestream_size > SIZE_MAX - 85u || codestream[43] != x_sampling ||
        codestream[44] != y_sampling) {
        fputs("Unable to read subsampled multi-tile grayscale vectors\n",
              stderr);
        goto cleanup;
    }
    jp2 = (uint8_t *)malloc(85u + codestream_size);
    if (!jp2) goto cleanup;
    jp2_size = make_t803_gray8_jp2(
        codestream, codestream_size, image_width, image_height, jp2,
        85u + codestream_size);
    if (!jp2_size) goto cleanup;
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "Subsampled multi-tile JP2 structure failed: rc=%d\n", rc);
        goto cleanup;
    }

    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK && page.data &&
             page.kind == QW_PAGE_IMAGE_GRAY8 &&
             page.width == component_width &&
             page.height == component_height && page.size == expected_size &&
             memcmp(page.data, reference, expected_size) == 0;
    if (!passed) {
        fprintf(stderr,
                "Internal %ux%u subsampled multi-tile decode failed: "
                "rc=%d, dimensions=%ux%u, size=%zu\n",
                (unsigned)x_sampling, (unsigned)y_sampling, rc, page.width,
                page.height, page.size);
    }
    qw_image_release_page(&page);
    if (!passed) goto cleanup;

    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_OK && page.data &&
             page.kind == QW_PAGE_IMAGE_GRAY8 &&
             page.width == component_width &&
             page.height == component_height && page.size == expected_size &&
             memcmp(page.data, reference, expected_size) == 0;
    if (!passed) {
        fprintf(stderr,
                "Dispatched %ux%u subsampled multi-tile decode failed: "
                "rc=%d, dimensions=%ux%u, size=%zu\n",
                (unsigned)x_sampling, (unsigned)y_sampling, rc, page.width,
                page.height, page.size);
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    free(reference);
    free(codestream);
    return passed;
}

static int check_internal_subsampled_gray8_multitile(void)
{
    return check_internal_subsampled_gray8_multitile_fixture(
               "synthetic_gray8_subsampled_16x16_dx2x1_tiles8x8.j2k",
               "synthetic_gray8_subsampled_16x16_dx2x1_reference.gray8",
               16u, 16u, 2u, 1u, 8u, 16u) &&
           check_internal_subsampled_gray8_multitile_fixture(
               "synthetic_gray8_subsampled_16x16_dx2x2_tiles8x8.j2k",
               "synthetic_gray8_subsampled_16x16_dx2x2_reference.gray8",
               16u, 16u, 2u, 2u, 8u, 8u) &&
           check_internal_subsampled_gray8_multitile_fixture(
               "synthetic_gray8_subsampled_20x14_dx2x1_tiles8x8.j2k",
               "synthetic_gray8_subsampled_20x14_dx2x1_reference.gray8",
               20u, 14u, 2u, 1u, 10u, 14u);
}

static int check_internal_gray8_roi_shift_unsupported_case(
    const char *fixture_name,
    uint8_t roi_shift)
{
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    uint8_t *jp2 = NULL;
    size_t jp2_size;
    size_t rgn_offset;
    QwPageImage page = {0};
    int rc = QW_STATUS_ERR_FORMAT;
    int passed = 0;

    if (!fixture_name ||
        !read_fixture(fixture_name, &codestream, &codestream_size)) {
        fputs("Unable to read the high-shift ROI boundary fixture\n", stderr);
        goto cleanup;
    }
    rgn_offset = find_first_marker(codestream, codestream_size, 0u, 0x5eu);
    if (rgn_offset == codestream_size || codestream_size - rgn_offset < 7u ||
        read_be16(codestream + rgn_offset + 2u) != 5u ||
        codestream[rgn_offset + 4u] != 0u ||
        codestream[rgn_offset + 5u] != 0u ||
        codestream_size > SIZE_MAX - 85u) {
        fputs("Could not locate the main-header ROI shift marker\n", stderr);
        goto cleanup;
    }
    codestream[rgn_offset + 6u] = roi_shift;
    jp2 = (uint8_t *)malloc(85u + codestream_size);
    if (!jp2) goto cleanup;
    jp2_size = make_t803_gray8_jp2(
        codestream, codestream_size, 16u, 16u,
        jp2, 85u + codestream_size);
    if (!jp2_size) goto cleanup;

    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    passed = rc == QW_STATUS_ERR_UNSUPPORTED && !page.data &&
             page.size == 0u;
    if (!passed) {
        fprintf(stderr,
                "High ROI shift %u was not reported unsupported: "
                "rc=%d, size=%zu\n",
                (unsigned)roi_shift, rc, page.size);
    }

cleanup:
    qw_image_release_page(&page);
    free(jp2);
    free(codestream);
    return passed;
}

static int check_internal_gray8_roi_shift_unsupported(void)
{
    return check_internal_gray8_roi_shift_unsupported_case(
               "synthetic_gray8_16x16_tiles_8x8_roi_u30.j2k", 53u) &&
           check_internal_gray8_roi_shift_unsupported_case(
               "synthetic_gray8_16x16_tiles_8x8_97_roi_u31.j2k", 52u) &&
           check_internal_gray8_roi_shift_unsupported_case(
               "synthetic_gray8_16x16_tiles_8x8_roi_u30.j2k", 63u) &&
           check_internal_gray8_roi_shift_unsupported_case(
               "synthetic_gray8_16x16_tiles_8x8_roi_u30.j2k", 64u);
}

static int check_internal_irreversible_gray8_roi_maxshift(void)
{
    const uint32_t width = 16u;
    const uint32_t height = 16u;
    uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    uint8_t *reference = NULL;
    size_t reference_size = 0u;
    int passed = 0;

    if (!read_fixture("synthetic_gray8_16x16_tiles_8x8_97_no_roi.j2k",
                      &codestream, &codestream_size) ||
        !read_fixture("synthetic_gray8_16x16_tiles_8x8_97_no_roi_reference.gray8",
                      &reference, &reference_size)) {
        fputs("Unable to read synthetic 9/7 no-ROI control vectors\n", stderr);
        goto cleanup;
    }
    passed = check_gray8_codestream_matches_reference(
        codestream, codestream_size, reference, reference_size,
        width, height, "no-ROI control");
    if (!passed) goto cleanup;
    free(reference);
    free(codestream);
    reference = NULL;
    reference_size = 0u;
    codestream = NULL;
    codestream_size = 0u;
    if (!read_fixture("synthetic_gray8_16x16_tiles_8x8_97_roi_u2.j2k",
                      &codestream, &codestream_size) ||
        !read_fixture("synthetic_gray8_16x16_tiles_8x8_97_roi_u2_reference.gray8",
                      &reference, &reference_size)) {
        fputs("Unable to read synthetic 9/7 ROI reference vectors\n", stderr);
        goto cleanup;
    }
    passed = check_gray8_codestream_matches_reference(
        codestream, codestream_size, reference, reference_size,
        width, height, "ROI maxshift");
    if (!passed) goto cleanup;
    free(reference);
    free(codestream);
    reference = NULL;
    reference_size = 0u;
    codestream = NULL;
    codestream_size = 0u;
    if (!read_fixture("synthetic_gray8_16x16_tiles_8x8_97_roi_u4.j2k",
                      &codestream, &codestream_size) ||
        !read_fixture("synthetic_gray8_16x16_tiles_8x8_97_roi_u4_reference.gray8",
                      &reference, &reference_size)) {
        fputs("Unable to read synthetic 9/7 ROI-u4 reference vectors\n", stderr);
        goto cleanup;
    }
    passed = check_gray8_codestream_matches_reference(
        codestream, codestream_size, reference, reference_size,
        width, height, "ROI maxshift u4");
    if (!passed) goto cleanup;
    free(reference);
    free(codestream);
    reference = NULL;
    reference_size = 0u;
    codestream = NULL;
    codestream_size = 0u;
    if (!read_fixture("synthetic_gray8_16x16_tiles_8x8_97_roi_u31.j2k",
                      &codestream, &codestream_size) ||
        !read_fixture(
            "synthetic_gray8_16x16_tiles_8x8_97_no_roi_reference.gray8",
            &reference, &reference_size)) {
        fputs("Unable to read synthetic 9/7 ROI-u31 reference vectors\n",
              stderr);
        goto cleanup;
    }
    passed = check_gray8_codestream_matches_reference(
        codestream, codestream_size, reference, reference_size,
        width, height, "ROI maxshift u31 against no-ROI quantized reference");
    if (!passed) goto cleanup;
    free(reference);
    free(codestream);
    reference = NULL;
    reference_size = 0u;
    codestream = NULL;
    codestream_size = 0u;
    if (!read_fixture("synthetic_gray8_16x16_tiles_8x8_97_roi_u51.j2k",
                      &codestream, &codestream_size) ||
        !read_fixture(
            "synthetic_gray8_16x16_tiles_8x8_97_no_roi_reference.gray8",
            &reference, &reference_size)) {
        fputs("Unable to read synthetic 9/7 ROI-u51 reference vectors\n",
              stderr);
        goto cleanup;
    }
    passed = check_gray8_codestream_matches_reference(
        codestream, codestream_size, reference, reference_size,
        width, height, "ROI maxshift u51 against no-ROI quantized reference");

cleanup:
    free(reference);
    free(codestream);
    return passed;
}

static int check_jp2_resource_limit_variant(const char *label,
                                            const uint8_t *codestream,
                                            size_t codestream_size,
                                            uint32_t width,
                                            uint32_t height,
                                            int wrap_in_jp2)
{
    uint8_t *mutated = NULL;
    uint8_t *container = NULL;
    const uint8_t *input;
    size_t input_size;
    size_t siz_offset;
    uint32_t x_origin;
    uint32_t y_origin;
    QwPageImage page = {0};
    QwImage image = {0};
    int rc;
    int passed = 0;

    if (!label || !codestream || !codestream_size || !width || !height) {
        return 0;
    }
    siz_offset = find_first_marker(codestream, codestream_size, 0u, 0x51u);
    if (siz_offset == codestream_size ||
        codestream_size - siz_offset < 22u ||
        read_be16(codestream + siz_offset + 2u) < 41u) {
        fprintf(stderr, "%s: could not locate a complete SIZ marker\n", label);
        return 0;
    }
    x_origin = read_be32(codestream + siz_offset + 14u);
    y_origin = read_be32(codestream + siz_offset + 18u);
    if (width > UINT32_MAX - x_origin || height > UINT32_MAX - y_origin ||
        codestream_size > SIZE_MAX - 85u) {
        fprintf(stderr, "%s: requested SIZ dimensions overflow\n", label);
        return 0;
    }

    mutated = (uint8_t *)malloc(codestream_size);
    if (!mutated) goto cleanup;
    memcpy(mutated, codestream, codestream_size);
    write_be32(mutated + siz_offset + 6u, x_origin + width);
    write_be32(mutated + siz_offset + 10u, y_origin + height);

    input = mutated;
    input_size = codestream_size;
    if (wrap_in_jp2) {
        container = (uint8_t *)malloc(codestream_size + 85u);
        if (!container) goto cleanup;
        input_size = make_t803_gray8_jp2(
            mutated, codestream_size, width, height,
            container, codestream_size + 85u);
        if (!input_size) {
            fprintf(stderr, "%s: failed to construct JP2 wrapper\n", label);
            goto cleanup;
        }
        input = container;
    }

    rc = qw_jp2_validate_structure(input, input_size);
    if (rc != QW_STATUS_ERR_RESOURCE_LIMIT) {
        fprintf(stderr,
                "%s: structure preflight returned %d instead of resource limit\n",
                label, rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(input, input_size, &page);
    if (rc != QW_STATUS_ERR_RESOURCE_LIMIT || page.data || page.size ||
        page.width || page.height) {
        fprintf(stderr,
                "%s: internal decode did not reject transactionally "
                "(rc=%d, kind=%d, dimensions=%ux%u, size=%zu)\n",
                label, rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_jp2_decode_page(input, input_size, &page);
    if (rc != QW_STATUS_ERR_RESOURCE_LIMIT || page.data || page.size ||
        page.width || page.height) {
        fprintf(stderr,
                "%s: public dispatch did not reject transactionally "
                "(rc=%d, kind=%d, dimensions=%ux%u, size=%zu)\n",
                label, rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_image_decode(input, input_size, &image);
    if (rc != QW_IMAGE_ERR_RESOURCE_LIMIT || image.pixels || image.size ||
        image.width || image.height) {
        fprintf(stderr,
                "%s: public image API did not reject transactionally "
                "(rc=%d, dimensions=%ux%u, size=%zu)\n",
                label, rc, image.width, image.height, image.size);
        goto cleanup;
    }
    passed = 1;

cleanup:
    qw_image_free(&image);
    qw_image_release_page(&page);
    free(container);
    free(mutated);
    return passed;
}

static int check_jp2_tile_grid_resource_limit(const uint8_t *codestream,
                                              size_t codestream_size)
{
    enum { TEST_TILE_GRID_WIDTH = 513u, TEST_TILE_GRID_HEIGHT = 512u };
    uint8_t *mutated = NULL;
    size_t siz_offset;
    QwPageImage page = {0};
    QwImage image = {0};
    int rc;
    int passed = 0;

    if (!codestream || !codestream_size) return 0;
    siz_offset = find_first_marker(codestream, codestream_size, 0u, 0x51u);
    if (siz_offset == codestream_size ||
        codestream_size - siz_offset < 40u) {
        fputs("tile-grid resource fixture has no complete SIZ marker\n",
              stderr);
        return 0;
    }

    mutated = (uint8_t *)malloc(codestream_size);
    if (!mutated) return 0;
    memcpy(mutated, codestream, codestream_size);
    write_be32(mutated + siz_offset + 6u, TEST_TILE_GRID_WIDTH);
    write_be32(mutated + siz_offset + 10u, TEST_TILE_GRID_HEIGHT);
    write_be32(mutated + siz_offset + 14u, 0u);
    write_be32(mutated + siz_offset + 18u, 0u);
    write_be32(mutated + siz_offset + 22u, 1u);
    write_be32(mutated + siz_offset + 26u, 1u);
    write_be32(mutated + siz_offset + 30u, 0u);
    write_be32(mutated + siz_offset + 34u, 0u);

    rc = qw_jp2_validate_structure(mutated, codestream_size);
    if (rc != QW_STATUS_ERR_RESOURCE_LIMIT) {
        fprintf(stderr,
                "tile grid %u x %u returned %d instead of resource limit\n",
                TEST_TILE_GRID_WIDTH, TEST_TILE_GRID_HEIGHT, rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(mutated, codestream_size, &page);
    if (rc != QW_STATUS_ERR_RESOURCE_LIMIT || page.data || page.size ||
        page.width || page.height) {
        fprintf(stderr,
                "tile-grid internal decode did not reject transactionally "
                "(rc=%d, dimensions=%ux%u, size=%zu)\n",
                rc, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_jp2_decode_page(mutated, codestream_size, &page);
    if (rc != QW_STATUS_ERR_RESOURCE_LIMIT || page.data || page.size ||
        page.width || page.height) {
        fprintf(stderr,
                "tile-grid page dispatch did not reject transactionally "
                "(rc=%d, dimensions=%ux%u, size=%zu)\n",
                rc, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_image_decode(mutated, codestream_size, &image);
    if (rc != QW_IMAGE_ERR_RESOURCE_LIMIT || image.pixels || image.size ||
        image.width || image.height) {
        fprintf(stderr,
                "tile-grid public API did not reject transactionally "
                "(rc=%d, dimensions=%ux%u, size=%zu)\n",
                rc, image.width, image.height, image.size);
        goto cleanup;
    }
    passed = 1;

cleanup:
    qw_image_free(&image);
    qw_image_release_page(&page);
    free(mutated);
    return passed;
}

static int check_jp2_resource_limits(void)
{
    static const char *const grayscale_fixture = "t803_profile0_p0_01.j2k";
    static const char *const subsampled_fixture = "t803_profile0_p0_02.j2k";
    uint8_t *grayscale = NULL;
    uint8_t *subsampled = NULL;
    size_t grayscale_size = 0u;
    size_t subsampled_size = 0u;
    uint32_t over_dimension = (uint32_t)QW_IMAGE_MAX_DIMENSION + 1u;
    uint32_t pixel_limit_width = (uint32_t)QW_IMAGE_MAX_DIMENSION;
    uint32_t over_pixel_height = (uint32_t)(
        QW_IMAGE_MAX_DECODED_PIXELS / (size_t)pixel_limit_width + 1u);
    QwPageImage page = {0};
    int passed = 0;
    int rc;

    if (!read_fixture(grayscale_fixture, &grayscale, &grayscale_size) ||
        !read_fixture(subsampled_fixture, &subsampled, &subsampled_size)) {
        fputs("could not load JP2 resource-limit fixtures\n", stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(subsampled, subsampled_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "subsampled-profile resource-limit baseline structure was "
                "rejected: rc=%d\n", rc);
        goto cleanup;
    }

    if (!check_jp2_tile_grid_resource_limit(grayscale, grayscale_size) ||
        !check_jp2_resource_limit_variant(
            "raw J2K dimension limit", grayscale, grayscale_size,
            over_dimension, 128u, 0) ||
        !check_jp2_resource_limit_variant(
            "JP2 dimension limit", grayscale, grayscale_size,
            over_dimension, 128u, 1) ||
        !check_jp2_resource_limit_variant(
            "raw J2K decoded-pixel limit", grayscale, grayscale_size,
            pixel_limit_width, over_pixel_height, 0) ||
        !check_jp2_resource_limit_variant(
            "JP2 decoded-pixel limit", grayscale, grayscale_size,
            pixel_limit_width, over_pixel_height, 1) ||
        !check_jp2_resource_limit_variant(
            "subsampled raw J2K dimension limit", subsampled,
            subsampled_size, over_dimension, 126u, 0) ||
        !check_jp2_resource_limit_variant(
            "subsampled raw J2K decoded-pixel limit", subsampled,
            subsampled_size, pixel_limit_width, over_pixel_height, 0)) {
        goto cleanup;
    }
    passed = 1;

cleanup:
    qw_image_release_page(&page);
    free(subsampled);
    free(grayscale);
    return passed;
}

static int check_tiled_ict_straight_alpha(void)
{
    static const char *const fixture_name =
        "synthetic_rgba8_mct_ict_premultiplied_alpha_16x16_tiles_8x8.jp2";
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    uint8_t reference_pixels[768u];
    TestOpenJpegMemoryStream memory = {0};
    opj_stream_t *stream = NULL;
    opj_codec_t *codec = NULL;
    opj_image_t *reference = NULL;
    opj_dparameters_t parameters;
    int passed = 0;
    int rc;

    if (!read_fixture(fixture_name, &jp2, &jp2_size)) {
        fprintf(stderr, "could not load tiled alpha fixture: %s\n",
                fixture_name);
        goto cleanup;
    }
    if (!jp2_test_set_component3_alpha_type(jp2, jp2_size, 2u, 1u)) {
        fputs("could not change component 3 CDEF from Typ=2 to Typ=1\n",
              stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "tiled straight-alpha JP2 structure rejected: rc=%d\n", rc);
        goto cleanup;
    }

    memory.data = jp2;
    memory.size = jp2_size;
    stream = opj_stream_default_create(OPJ_TRUE);
    codec = opj_create_decompress(OPJ_CODEC_JP2);
    if (!stream || !codec) goto reference_decode_failed;
    opj_stream_set_user_data(stream, &memory, NULL);
    opj_stream_set_user_data_length(stream, (OPJ_UINT64)jp2_size);
    opj_stream_set_read_function(stream, test_openjpeg_read);
    opj_stream_set_skip_function(stream, test_openjpeg_skip);
    opj_stream_set_seek_function(stream, test_openjpeg_seek);
    opj_set_error_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_warning_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_info_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_default_decoder_parameters(&parameters);
    if (!opj_setup_decoder(codec, &parameters) ||
        !opj_read_header(stream, codec, &reference) || !reference ||
        !opj_decode(codec, stream, reference) ||
        !opj_end_decompress(codec, stream)) {
        goto reference_decode_failed;
    }
    if (reference->x0 != 0u || reference->y0 != 0u ||
        reference->x1 != 16u || reference->y1 != 16u ||
        reference->numcomps != 4u) {
        fputs("OpenJPEG tiled straight-alpha reference layout mismatch\n",
              stderr);
        goto cleanup;
    }
    for (OPJ_UINT32 component = 0u; component < 4u; ++component) {
        const opj_image_comp_t *plane = &reference->comps[component];
        if (!plane->data || plane->w != 16u || plane->h != 16u ||
            plane->prec != 8u || plane->sgnd) {
            fprintf(stderr,
                    "OpenJPEG tiled straight-alpha component %u is invalid\n",
                    component);
            goto cleanup;
        }
    }
    for (size_t pixel = 0u; pixel < 256u; ++pixel) {
        int red = reference->comps[0].data[pixel];
        int green = reference->comps[1].data[pixel];
        int blue = reference->comps[2].data[pixel];
        int alpha = reference->comps[3].data[pixel];
        if (red < 0 || red > 255 || green < 0 || green > 255 ||
            blue < 0 || blue > 255 || alpha < 0 || alpha > 255) {
            fputs("OpenJPEG tiled straight-alpha sample is out of range\n",
                  stderr);
            goto cleanup;
        }
        reference_pixels[pixel * 3u] = jp2_expected_composite_white(
            (uint8_t)red, (uint8_t)alpha, 0);
        reference_pixels[pixel * 3u + 1u] = jp2_expected_composite_white(
            (uint8_t)green, (uint8_t)alpha, 0);
        reference_pixels[pixel * 3u + 2u] = jp2_expected_composite_white(
            (uint8_t)blue, (uint8_t)alpha, 0);
    }
    passed = check_internal_rgb_mct_ict_alpha_data(
        jp2, jp2_size, reference_pixels, sizeof(reference_pixels),
        "tiled straight-alpha");
    if (passed) {
        puts("2x2 tiled ICT/9-7 straight-alpha CDEF profile matches "
             "the OpenJPEG-derived RGB24 reference within 1 LSB");
    }
    goto cleanup;

reference_decode_failed:
    fputs("could not decode tiled straight-alpha JP2 with OpenJPEG\n",
          stderr);

cleanup:
    if (reference) opj_image_destroy(reference);
    if (codec) opj_destroy_codec(codec);
    if (stream) opj_stream_destroy(stream);
    free(jp2);
    return passed;
}

static int check_tiled_ict_partial_edge_tiles(void)
{
    enum { WIDTH = 13u, HEIGHT = 11u, PIXELS = WIDTH * HEIGHT,
           RGB24_SIZE = PIXELS * 3u };
    static const char *const fixture_name =
        "synthetic_rgb8_mct_ict_13x11_tiles_8x8.jp2";
    uint8_t *jp2 = NULL;
    size_t jp2_size = 0u;
    uint8_t reference_pixels[RGB24_SIZE];
    uint8_t internal_pixels[RGB24_SIZE];
    TestOpenJpegMemoryStream memory = {0};
    opj_stream_t *stream = NULL;
    opj_codec_t *codec = NULL;
    opj_image_t *reference = NULL;
    opj_dparameters_t parameters;
    QwPageImage page = {0};
    QwImage image = {0};
    int passed = 0;
    int rc;

    if (!read_fixture(fixture_name, &jp2, &jp2_size)) {
        fprintf(stderr, "could not load partial-edge tile fixture: %s\n",
                fixture_name);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(jp2, jp2_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "partial-edge tile JP2 structure rejected: rc=%d\n",
                rc);
        goto cleanup;
    }

    memory.data = jp2;
    memory.size = jp2_size;
    stream = opj_stream_default_create(OPJ_TRUE);
    codec = opj_create_decompress(OPJ_CODEC_JP2);
    if (!stream || !codec) goto reference_decode_failed;
    opj_stream_set_user_data(stream, &memory, NULL);
    opj_stream_set_user_data_length(stream, (OPJ_UINT64)jp2_size);
    opj_stream_set_read_function(stream, test_openjpeg_read);
    opj_stream_set_skip_function(stream, test_openjpeg_skip);
    opj_stream_set_seek_function(stream, test_openjpeg_seek);
    opj_set_error_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_warning_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_info_handler(codec, test_openjpeg_quiet_message, NULL);
    opj_set_default_decoder_parameters(&parameters);
    if (!opj_setup_decoder(codec, &parameters) ||
        !opj_read_header(stream, codec, &reference) || !reference ||
        !opj_decode(codec, stream, reference) ||
        !opj_end_decompress(codec, stream)) {
        goto reference_decode_failed;
    }
    if (reference->x0 != 0u || reference->y0 != 0u ||
        reference->x1 != WIDTH || reference->y1 != HEIGHT ||
        reference->numcomps != 3u) {
        fputs("OpenJPEG partial-edge tile reference layout mismatch\n",
              stderr);
        goto cleanup;
    }
    for (OPJ_UINT32 component = 0u; component < 3u; ++component) {
        const opj_image_comp_t *plane = &reference->comps[component];
        if (!plane->data || plane->w != WIDTH || plane->h != HEIGHT ||
            plane->prec != 8u || plane->sgnd) {
            fprintf(stderr,
                    "OpenJPEG partial-edge tile component %u is invalid\n",
                    component);
            goto cleanup;
        }
    }
    for (size_t pixel = 0u; pixel < PIXELS; ++pixel) {
        for (size_t component = 0u; component < 3u; ++component) {
            int sample = reference->comps[component].data[pixel];
            if (sample < 0 || sample > 255) {
                fputs("OpenJPEG partial-edge tile sample is out of range\n",
                      stderr);
                goto cleanup;
            }
            reference_pixels[pixel * 3u + component] = (uint8_t)sample;
        }
    }

    rc = qw_jp2_decode_internal_gray8(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != WIDTH ||
        page.height != HEIGHT || page.size != RGB24_SIZE) {
        fprintf(stderr,
                "Internal partial-edge tile decode failed: rc=%d, kind=%d, "
                "dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    for (size_t i = 0u; i < RGB24_SIZE; ++i) {
        int difference = (int)page.data[i] - (int)reference_pixels[i];
        if (difference < -1 || difference > 1) {
            fprintf(stderr,
                    "Partial-edge tile ICT reference mismatch at byte %zu: "
                    "got %u, expected %u\n",
                    i, page.data[i], reference_pixels[i]);
            goto cleanup;
        }
    }
    memcpy(internal_pixels, page.data, sizeof(internal_pixels));
    qw_image_release_page(&page);

    rc = qw_jp2_decode_page(jp2, jp2_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != WIDTH ||
        page.height != HEIGHT || page.size != RGB24_SIZE ||
        memcmp(page.data, internal_pixels, sizeof(internal_pixels)) != 0) {
        fprintf(stderr,
                "Dispatched partial-edge tile decode failed: rc=%d, "
                "kind=%d, dimensions=%ux%u, size=%zu\n",
                rc, page.kind, page.width, page.height, page.size);
        goto cleanup;
    }
    qw_image_release_page(&page);

    rc = qw_image_decode(jp2, jp2_size, &image);
    if (rc != QW_IMAGE_OK || !image.pixels ||
        image.format != QW_IMAGE_PIXEL_RGB24 || image.width != WIDTH ||
        image.height != HEIGHT || image.stride != WIDTH * 3u ||
        image.size != RGB24_SIZE ||
        memcmp(image.pixels, internal_pixels, sizeof(internal_pixels)) != 0) {
        fprintf(stderr,
                "Public partial-edge tile decode failed: rc=%d, "
                "format=%d, dimensions=%ux%u, stride=%zu, size=%zu\n",
                rc, image.format, image.width, image.height, image.stride,
                image.size);
        goto cleanup;
    }
    passed = 1;
    puts("2x2 tiled ICT/9-7 partial-edge profile matches the OpenJPEG "
         "RGB24 reference within 1 LSB; internal/page/public outputs agree");
    goto cleanup;

reference_decode_failed:
    fputs("could not decode partial-edge tile JP2 with OpenJPEG\n", stderr);

cleanup:
    qw_image_free(&image);
    qw_image_release_page(&page);
    if (reference) opj_image_destroy(reference);
    if (codec) opj_destroy_codec(codec);
    if (stream) opj_stream_destroy(stream);
    free(jp2);
    return passed;
}

static int mutate_jp2_enumerated_color_space(uint8_t *data,
                                             size_t size,
                                             uint32_t color_space)
{
    const uint32_t jp2h_type = UINT32_C(0x6a703268);
    const uint32_t colr_type = UINT32_C(0x636f6c72);
    size_t offset = 0u;

    if (!data || size < 12u) return 0;
    while (offset < size) {
        uint32_t box_length;
        uint32_t box_type;
        size_t box_size;

        if (size - offset < 8u) return 0;
        box_length = read_be32(data + offset);
        box_type = read_be32(data + offset + 4u);
        if (box_length == 1u) return 0;
        box_size = box_length == 0u ? size - offset : (size_t)box_length;
        if (box_size < 8u || box_size > size - offset) return 0;

        if (box_type == jp2h_type) {
            size_t child_offset = offset + 8u;
            size_t parent_end = offset + box_size;
            while (child_offset < parent_end) {
                uint32_t child_length;
                uint32_t child_type;
                size_t child_size;

                if (parent_end - child_offset < 8u) return 0;
                child_length = read_be32(data + child_offset);
                child_type = read_be32(data + child_offset + 4u);
                if (child_length == 1u) return 0;
                child_size = child_length == 0u
                    ? parent_end - child_offset : (size_t)child_length;
                if (child_size < 8u || child_size > parent_end - child_offset) {
                    return 0;
                }
                if (child_type == colr_type) {
                    size_t payload_size = child_size - 8u;
                    uint8_t *payload = data + child_offset + 8u;
                    if (payload_size < 7u || payload[0] != 1u) return 0;
                    write_be32(payload + 3u, color_space);
                    return 1;
                }
                child_offset += child_size;
            }
            return 0;
        }
        offset += box_size;
    }
    return 0;
}

static uint8_t jp2_test_sycc_clamp_u8(int64_t sample)
{
    return sample <= 0 ? 0u : sample >= 255 ? 255u : (uint8_t)sample;
}

static int jp2_test_sycc_rgb24_reference_from_openjpeg(
    const opj_image_t *reference,
    uint32_t width,
    uint32_t height,
    uint8_t *out_rgb)
{
    if (!reference || !out_rgb || reference->color_space != OPJ_CLRSPC_SYCC ||
        reference->numcomps != 3u || reference->x1 < reference->x0 ||
        reference->y1 < reference->y0 || reference->x1 - reference->x0 != width ||
        reference->y1 - reference->y0 != height) {
        return 0;
    }
    for (size_t component = 0u; component < 3u; ++component) {
        const opj_image_comp_t *plane = &reference->comps[component];
        if (!plane->data || plane->w != width || plane->h != height ||
            plane->dx != 1u || plane->dy != 1u || plane->prec != 8u ||
            plane->sgnd != 0u) {
            return 0;
        }
    }
    for (size_t pixel = 0u; pixel < (size_t)width * height; ++pixel) {
        int y = reference->comps[0].data[pixel];
        int cb = reference->comps[1].data[pixel] - 128;
        int cr = reference->comps[2].data[pixel] - 128;
        int red = y + (int)(1.402 * (float)cr);
        int green = y - (int)(0.344 * (float)cb + 0.714 * (float)cr);
        int blue = y + (int)(1.772 * (float)cb);
        out_rgb[pixel * 3u] = jp2_test_sycc_clamp_u8(red);
        out_rgb[pixel * 3u + 1u] = jp2_test_sycc_clamp_u8(green);
        out_rgb[pixel * 3u + 2u] = jp2_test_sycc_clamp_u8(blue);
    }
    return 1;
}

static int check_jp2_sycc_color_space_444(void)
{
    uint8_t *input = NULL;
    size_t input_size = 0u;
    uint8_t expected[16u * 16u * 3u];
    opj_image_t *reference = NULL;
    QwPageImage page = {0};
    QwImage image = {0};
    int rc;
    int passed = 0;

    if (!read_fixture("synthetic_rgb8_no_mct_16x16.jp2",
                      &input, &input_size) ||
        !mutate_jp2_enumerated_color_space(input, input_size, 18u)) {
        fputs("could not construct the JP2 sYCC 4:4:4 vector\n", stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(input, input_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr,
                "valid JP2 sYCC structure returned %d instead of success\n",
                rc);
        goto cleanup;
    }
    reference = openjpeg_test_decode_memory(input, input_size, 0u);
    if (!jp2_test_sycc_rgb24_reference_from_openjpeg(
            reference, 16u, 16u, expected)) {
        fputs("OpenJPEG did not provide three unsigned 8-bit sYCC planes\n",
              stderr);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(input, input_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != 16u ||
        page.height != 16u || page.size != sizeof(expected) ||
        memcmp(page.data, expected, sizeof(expected)) != 0) {
        fprintf(stderr,
                "internal JP2 sYCC 4:4:4 decode differs from OpenJPEG "
                "(rc=%d, dimensions=%ux%u, size=%zu)\n",
                rc, page.width, page.height, page.size);
        goto cleanup;
    }
    opj_image_destroy(reference);
    reference = NULL;
    qw_image_release_page(&page);
    rc = qw_jp2_decode_page(input, input_size, &page);
    if (rc != QW_STATUS_OK || !page.data ||
        page.kind != QW_PAGE_IMAGE_RGB24 || page.width != 16u ||
        page.height != 16u || page.size != sizeof(expected) ||
        memcmp(page.data, expected, sizeof(expected)) != 0) {
        fprintf(stderr,
                "JP2 sYCC 4:4:4 page dispatch differs from OpenJPEG "
                "(rc=%d, dimensions=%ux%u, size=%zu)\n",
                rc, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_image_decode(input, input_size, &image);
    if (rc != QW_IMAGE_OK || !image.pixels ||
        image.format != QW_IMAGE_PIXEL_RGB24 || image.width != 16u ||
        image.height != 16u || image.stride != 16u * 3u ||
        image.size != sizeof(expected) ||
        memcmp(image.pixels, expected, sizeof(expected)) != 0) {
        fprintf(stderr,
                "public JP2 sYCC 4:4:4 decode differs from OpenJPEG "
                "(rc=%d, dimensions=%ux%u, size=%zu)\n",
                rc, image.width, image.height, image.size);
        goto cleanup;
    }
    puts("JP2 sYCC 4:4:4 matches OpenJPEG through internal, page, and public decode");
    passed = 1;

cleanup:
    if (reference) opj_image_destroy(reference);
    qw_image_free(&image);
    qw_image_release_page(&page);
    free(input);
    return passed;
}

static int check_jp2_sycc_alpha_unsupported(void)
{
    uint8_t *input = NULL;
    size_t input_size = 0u;
    QwPageImage page = {0};
    QwImage image = {0};
    int rc;
    int passed = 0;

    if (!read_fixture("synthetic_rgba8_cdef_straight_alpha_component0_16x16.jp2",
                      &input, &input_size) ||
        !mutate_jp2_enumerated_color_space(input, input_size, 18u)) {
        fputs("could not construct the JP2 sYCC alpha variant\n", stderr);
        goto cleanup;
    }
    rc = qw_jp2_validate_structure(input, input_size);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "valid JP2 sYCC alpha structure returned %d\n", rc);
        goto cleanup;
    }
    rc = qw_jp2_decode_internal_gray8(input, input_size, &page);
    if (rc != QW_STATUS_ERR_UNSUPPORTED || page.data || page.size ||
        page.width || page.height) {
        fprintf(stderr,
                "internal JP2 sYCC alpha variant was not transactionally "
                "unsupported (rc=%d, dimensions=%ux%u, size=%zu)\n",
                rc, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_jp2_decode_page(input, input_size, &page);
    if (rc != QW_STATUS_ERR_UNSUPPORTED || page.data || page.size ||
        page.width || page.height) {
        fprintf(stderr,
                "JP2 sYCC alpha page dispatch was not transactionally "
                "unsupported (rc=%d, dimensions=%ux%u, size=%zu)\n",
                rc, page.width, page.height, page.size);
        goto cleanup;
    }
    rc = qw_image_decode(input, input_size, &image);
    if (rc != QW_IMAGE_ERR_UNSUPPORTED || image.pixels || image.size ||
        image.width || image.height || image.stride) {
        fprintf(stderr,
                "public JP2 sYCC alpha decode was not transactionally "
                "unsupported (rc=%d, dimensions=%ux%u, size=%zu)\n",
                rc, image.width, image.height, image.size);
        goto cleanup;
    }
    passed = 1;

cleanup:
    qw_image_free(&image);
    qw_image_release_page(&page);
    free(input);
    return passed;
}

int main(void)
{
    const uint64_t jp2_large_hash = UINT64_C(8095060991222383269);
    const uint64_t j2k_large_hash = UINT64_C(7153348325438446820);
    if (!check_fixture("rgb_u1.jp2") || !check_fixture("rgb_u1.j2k") ||
        !check_internal_rgb_reversible_mct("rgb_u1.jp2") ||
        !check_internal_rgb_reversible_mct("rgb_u1.j2k") ||
        !check_tier1_packet_decode() ||
        !check_packet_body_accumulation() ||
        !check_internal_reversible_gray8_decode() ||
        !check_internal_irreversible_gray8_decode() ||
        !check_t803_profile0_p0_01() ||
        !check_t803_profile0_p0_08_component_qcc() ||
        !check_t803_profile0_p0_08_reduced() ||
        !check_t803_profile0_p0_09_lossy() ||
        !check_t803_profile0_p0_09_quantization_overrides() ||
        !check_t803_profile0_p0_02_subsampled() ||
        !check_t803_profile0_p0_11_one_dimensional() ||
        !check_t803_profile0_p0_12_tiny() ||
        !check_t803_profile0_p0_03_signed4_dispatch_mapping() ||
        !check_t803_profile0_p0_16_multilayer() ||
        !check_internal_reversible_gray8_multilevel() ||
        !check_internal_reversible_gray8_multitile() ||
        !check_internal_gray8_roi_shift_unsupported() ||
        !check_internal_irreversible_gray8_multitile_phase() ||
        !check_internal_irreversible_gray8_roi_maxshift() ||
        !check_internal_subsampled_gray8_multitile() ||
        !check_internal_rgb_mct() ||
        !check_large_fixture("balloon.jp2", jp2_large_hash) ||
        !check_balloon_rgb_quality("balloon.jp2", OPJ_CODEC_JP2,
                                   0u, 1) ||
        !check_balloon_internal_public_parity("balloon.jp2") ||
        !check_balloon_rgb_quality("balloon.jp2", OPJ_CODEC_JP2,
                                   jp2_large_hash, 0) ||
        !check_large_fixture("balloon.j2c", j2k_large_hash) ||
        !check_balloon_rgb_quality("balloon.j2c", OPJ_CODEC_J2K,
                                   j2k_large_hash, 1) ||
        !check_jp2_resource_limits() ||
        !check_jp2_sycc_color_space_444() ||
        !check_jp2_sycc_alpha_unsupported() ||
        !check_malformed_inputs()) {
        return 1;
    }
    puts("JP2/J2K structure and reference vectors passed");
    return 0;
}
