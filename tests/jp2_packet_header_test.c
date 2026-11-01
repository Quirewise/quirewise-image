#include "jp2_packet_header.h"

#include <stdio.h>
#include <string.h>

typedef struct TestBitWriter {
    uint8_t bytes[64];
    size_t size;
    uint8_t current;
    uint8_t bits_in_current;
    uint8_t previous_was_ff;
} TestBitWriter;

static int writer_bit(TestBitWriter *writer, uint8_t bit)
{
    if (!writer || bit > 1u) return 0;
    if (!writer->bits_in_current && writer->previous_was_ff) {
        writer->bits_in_current = 1u;
        writer->current = 0u;
    }
    writer->current |= (uint8_t)(bit << (7u - writer->bits_in_current));
    ++writer->bits_in_current;
    if (writer->bits_in_current == 8u) {
        if (writer->size >= sizeof(writer->bytes)) return 0;
        writer->bytes[writer->size++] = writer->current;
        writer->previous_was_ff = writer->current == 0xffu;
        writer->current = 0u;
        writer->bits_in_current = 0u;
    }
    return 1;
}

static int writer_bits(TestBitWriter *writer, uint32_t value, uint8_t count)
{
    if (!writer || count > 32u) return 0;
    for (uint8_t i = count; i > 0u; --i) {
        if (!writer_bit(writer, (uint8_t)((value >> (i - 1u)) & 1u))) {
            return 0;
        }
    }
    return 1;
}

static int writer_finish(TestBitWriter *writer)
{
    if (!writer) return 0;
    if (writer->bits_in_current) {
        if (writer->size >= sizeof(writer->bytes)) return 0;
        writer->bytes[writer->size++] = writer->current;
        writer->current = 0u;
        writer->bits_in_current = 0u;
    }
    return 1;
}

static int test_bit_stuffing_and_errors(void)
{
    static const uint8_t stuffed[] = {0xffu, 0x00u};
    static const uint8_t invalid[] = {0xffu, 0x80u};
    static const uint8_t terminal_stuffed[] = {
        0xffu, 0x00u, 0xffu, 0x92u
    };
    static const uint8_t terminal_invalid[] = {
        0xffu, 0x80u, 0xffu, 0x92u
    };
    QwJp2PacketBitReader reader;
    QwJp2PacketBitReader before;
    uint32_t value = 0u;
    uint8_t bit = 9u;

    if (qw_jp2_packet_bit_reader_init(&reader, stuffed, sizeof(stuffed)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_read_bits(&reader, 8u, &value) != QW_JP2_PACKET_OK ||
        value != 0xffu ||
        qw_jp2_packet_read_bits(&reader, 7u, &value) != QW_JP2_PACKET_OK ||
        value != 0u || qw_jp2_packet_header_bytes_consumed(&reader) != 2u) {
        fputs("packet-header stuffed-bit decode mismatch\n", stderr);
        return 0;
    }
    if (qw_jp2_packet_bit_reader_init(&reader, invalid, sizeof(invalid)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_read_bits(&reader, 8u, &value) != QW_JP2_PACKET_OK) {
        fputs("packet-header invalid stuffed-bit setup failed\n", stderr);
        return 0;
    }

    if (qw_jp2_packet_bit_reader_init(
            &reader, terminal_stuffed, sizeof(terminal_stuffed)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_read_bits(&reader, 8u, &value) != QW_JP2_PACKET_OK ||
        value != 0xffu ||
        qw_jp2_packet_header_align(&reader) != QW_JP2_PACKET_OK ||
        reader.byte_offset != 2u || reader.bits_remaining != 0u ||
        reader.previous_was_ff != 0u ||
        terminal_stuffed[reader.byte_offset] != 0xffu ||
        terminal_stuffed[reader.byte_offset + 1u] != 0x92u) {
        fputs("packet-header terminal stuffed byte was not consumed\n", stderr);
        return 0;
    }

    if (qw_jp2_packet_bit_reader_init(
            &reader, terminal_invalid, sizeof(terminal_invalid)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_read_bits(&reader, 8u, &value) != QW_JP2_PACKET_OK) {
        fputs("packet-header terminal invalid bit setup failed\n", stderr);
        return 0;
    }
    before = reader;
    if (qw_jp2_packet_header_align(&reader) != QW_JP2_PACKET_ERR_FORMAT ||
        memcmp(&reader, &before, sizeof(reader)) != 0) {
        fputs("packet-header terminal stuffed bit was not rejected\n", stderr);
        return 0;
    }

    before = reader;
    if (qw_jp2_packet_read_bit(&reader, &bit) != QW_JP2_PACKET_ERR_FORMAT ||
        bit != 9u || memcmp(&reader, &before, sizeof(reader)) != 0) {
        fputs("packet-header invalid stuffed bit was not rejected\n", stderr);
        return 0;
    }
    return 1;
}

static int test_coding_pass_codewords(void)
{
    static const char *const codewords[] = {
        "0", "10", "1100", "1101", "1110", "111100000",
        "111111110", "1111111110000000", "1111111111111111"
    };
    static const uint16_t expected[] = {1u, 2u, 3u, 4u, 5u, 6u, 36u, 37u, 164u};
    TestBitWriter writer = {{0u}, 0u, 0u, 0u, 0u};
    QwJp2PacketBitReader reader;
    size_t count = 0u;

    for (size_t i = 0u; i < sizeof(codewords) / sizeof(codewords[0]); ++i) {
        for (size_t bit = 0u; codewords[i][bit] != '\0'; ++bit) {
            if (!writer_bit(&writer,
                            (uint8_t)(codewords[i][bit] - '0'))) return 0;
            ++count;
        }
    }
    if (count != 65u || !writer_finish(&writer) ||
        qw_jp2_packet_bit_reader_init(&reader, writer.bytes, writer.size) !=
            QW_JP2_PACKET_OK) {
        fputs("packet-header pass-code vector setup failed\n", stderr);
        return 0;
    }
    for (size_t i = 0u; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        uint16_t passes = 0u;
        if (qw_jp2_packet_read_coding_passes(&reader, &passes) !=
                QW_JP2_PACKET_OK || passes != expected[i]) {
            fprintf(stderr, "packet-header coding-pass codeword %zu mismatch\n",
                    i);
            return 0;
        }
    }
    return 1;
}

static int test_codeword_segment_lengths(void)
{
    static const uint16_t pass_counts[] = {1u, 2u, 1u, 1u};
    static const uint32_t expected_lengths[] = {6u, 75u, 134u, 192u};
    TestBitWriter writer = {{0u}, 0u, 0u, 0u, 0u};
    QwJp2PacketBitReader reader;
    uint8_t lblock = 3u;
    uint32_t lengths[4] = {0u, 0u, 0u, 0u};

    /* T.800 B.10.7 example: increase Lblock to 8, then signal four lengths. */
    if (!writer_bits(&writer, 0x3eu, 6u) ||
        !writer_bits(&writer, 6u, 8u) ||
        !writer_bits(&writer, 75u, 9u) ||
        !writer_bits(&writer, 134u, 8u) ||
        !writer_bits(&writer, 192u, 8u) || !writer_finish(&writer) ||
        qw_jp2_packet_bit_reader_init(&reader, writer.bytes, writer.size) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_read_segment_lengths(
            &reader, &lblock, pass_counts, 4u, lengths, 4u) !=
            QW_JP2_PACKET_OK || lblock != 8u ||
        memcmp(lengths, expected_lengths, sizeof(lengths)) != 0) {
        fputs("packet-header codeword-segment lengths mismatch\n", stderr);
        return 0;
    }
    return 1;
}

static int test_transactional_truncation(void)
{
    static const uint8_t truncated[] = {0xf8u};
    static const uint16_t pass_counts[] = {1u, 2u, 1u};
    QwJp2PacketBitReader reader;
    uint8_t lblock = 3u;
    uint32_t lengths[] = {0xaaaaaaaau, 0xbbbbbbbbu, 0xccccccccu};
    QwJp2PacketBitReader original;

    if (qw_jp2_packet_bit_reader_init(&reader, truncated,
                                      sizeof(truncated)) != QW_JP2_PACKET_OK) {
        return 0;
    }
    original = reader;
    if (qw_jp2_packet_read_segment_lengths(
            &reader, &lblock, pass_counts, 3u, lengths, 3u) !=
            QW_JP2_PACKET_ERR_FORMAT ||
        memcmp(&reader, &original, sizeof(reader)) != 0 || lblock != 3u ||
        lengths[0] != 0xaaaaaaaau || lengths[1] != 0xbbbbbbbbu ||
        lengths[2] != 0xccccccccu) {
        fputs("packet-header truncated segment parse was not transactional\n",
              stderr);
        return 0;
    }
    return 1;
}

static int test_tag_tree_decode(void)
{
    static const uint8_t encoded[] = {0xb0u};
    static const uint8_t truncated[] = {0x00u};
    static const uint8_t encoded_path[] = {0xf0u};
    QwJp2PacketTagTree *tree = NULL;
    QwJp2PacketBitReader reader;
    QwJp2PacketBitReader original;
    uint8_t below = 7u;
    int rc;

    if (qw_jp2_packet_tag_tree_create(0u, 1u, &tree) !=
            QW_JP2_PACKET_ERR_ARGUMENT || tree ||
        qw_jp2_packet_tag_tree_create(UINT32_MAX, 1u, &tree) !=
            QW_JP2_PACKET_ERR_RESOURCE || tree) {
        fputs("packet-header tag-tree resource bounds mismatch\n", stderr);
        return 0;
    }
    if (qw_jp2_packet_tag_tree_create(2u, 1u, &tree) != QW_JP2_PACKET_OK ||
        !tree || qw_jp2_packet_bit_reader_init(
                     &reader, encoded, sizeof(encoded)) != QW_JP2_PACKET_OK) {
        qw_jp2_packet_tag_tree_free(tree);
        fputs("packet-header tag-tree setup failed\n", stderr);
        return 0;
    }
    if (qw_jp2_packet_tag_tree_decode(&reader, tree, 0u, 0u, 1u,
                                     &below) != QW_JP2_PACKET_OK ||
        below != 0u || reader.bits_remaining != 6u ||
        qw_jp2_packet_tag_tree_decode(&reader, tree, 1u, 0u, 1u,
                                     &below) != QW_JP2_PACKET_OK ||
        below != 1u || reader.bits_remaining != 5u ||
        qw_jp2_packet_tag_tree_decode(&reader, tree, 0u, 0u, 2u,
                                     &below) != QW_JP2_PACKET_OK ||
        below != 1u || reader.bits_remaining != 4u) {
        qw_jp2_packet_tag_tree_free(tree);
        fputs("packet-header tag-tree values mismatch\n", stderr);
        return 0;
    }
    qw_jp2_packet_tag_tree_free(tree);

    tree = NULL;
    if (qw_jp2_packet_tag_tree_create(8u, 1u, &tree) != QW_JP2_PACKET_OK ||
        qw_jp2_packet_bit_reader_init(&reader, truncated,
                                      sizeof(truncated)) != QW_JP2_PACKET_OK) {
        qw_jp2_packet_tag_tree_free(tree);
        fputs("packet-header transactional tag-tree setup failed\n", stderr);
        return 0;
    }
    original = reader;
    below = 7u;
    rc = qw_jp2_packet_tag_tree_decode(&reader, tree, 0u, 0u, 9u, &below);
    if (rc != QW_JP2_PACKET_ERR_FORMAT ||
        memcmp(&reader, &original, sizeof(reader)) != 0 || below != 7u) {
        fprintf(stderr,
                "tag-tree truncated query: rc=%d reader_same=%d below=%u\n",
                rc, memcmp(&reader, &original, sizeof(reader)) == 0,
                (unsigned)below);
        qw_jp2_packet_tag_tree_free(tree);
        fputs("packet-header tag-tree truncation was not transactional\n",
              stderr);
        return 0;
    }
    if (qw_jp2_packet_bit_reader_init(&reader, encoded_path,
                                      sizeof(encoded_path)) != QW_JP2_PACKET_OK) {
        qw_jp2_packet_tag_tree_free(tree);
        fputs("packet-header tag-tree recovery reader setup failed\n", stderr);
        return 0;
    }
    rc = qw_jp2_packet_tag_tree_decode(&reader, tree, 0u, 0u, 1u, &below);
    if (rc != QW_JP2_PACKET_OK || below != 1u ||
        reader.bits_remaining != 4u) {
        fprintf(stderr,
                "tag-tree recovery query: rc=%d below=%u bits_remaining=%u\n",
                rc, (unsigned)below, (unsigned)reader.bits_remaining);
        qw_jp2_packet_tag_tree_free(tree);
        fputs("packet-header tag-tree truncation was not transactional\n",
              stderr);
        return 0;
    }
    qw_jp2_packet_tag_tree_free(tree);
    return 1;
}

static int test_codeblock_header_state(void)
{
    static const uint8_t first_packet[] = {0xcau};
    static const uint8_t second_packet[] = {0x88u};
    static const uint8_t empty_contribution[] = {0x00u};
    static const uint8_t unsupported_style_packet[] = {0x80u};
    static const uint8_t not_yet_included_packet[] = {0x40u};
    static const uint8_t delayed_first_packet[] = {0xc7u};
    static const uint8_t truncated_first_packet[] = {0xe2u};
    static const uint8_t complete_first_packet[] = {0xe2u, 0x80u};
    QwJp2PacketTagTree *inclusion_tree = NULL;
    QwJp2PacketTagTree *zero_bitplane_tree = NULL;
    QwJp2PacketBitReader reader;
    QwJp2PacketBitReader original_reader;
    QwJp2PacketCodeBlockState state;
    QwJp2PacketCodeBlockState original_state;
    QwJp2PacketCodeBlockContribution contribution;
    QwJp2PacketCodeBlockContribution original_contribution;
    int rc;

    if (qw_jp2_packet_tag_tree_create(1u, 1u, &inclusion_tree) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_tag_tree_create(1u, 1u, &zero_bitplane_tree) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_codeblock_state_init(&state, 4u) != QW_JP2_PACKET_OK ||
        qw_jp2_packet_bit_reader_init(&reader, first_packet,
                                      sizeof(first_packet)) !=
            QW_JP2_PACKET_OK) {
        qw_jp2_packet_tag_tree_free(inclusion_tree);
        qw_jp2_packet_tag_tree_free(zero_bitplane_tree);
        fputs("packet-header code-block setup failed\n", stderr);
        return 0;
    }

    rc = qw_jp2_packet_read_codeblock_header(
        &reader, inclusion_tree, zero_bitplane_tree, 0u, 0u, 0u, 0u,
        &state, &contribution);
    if (rc != QW_JP2_PACKET_OK || !contribution.included ||
        !contribution.first_inclusion || contribution.zero_bitplanes != 0u ||
        contribution.coding_passes != 1u ||
        contribution.codeword_segment_length != 5u || !state.included ||
        state.coding_passes != 1u || state.lblock != 3u ||
        state.zero_bitplanes != 0u || reader.bits_remaining != 1u) {
        qw_jp2_packet_tag_tree_free(inclusion_tree);
        qw_jp2_packet_tag_tree_free(zero_bitplane_tree);
        fputs("packet-header first code-block contribution mismatch\n", stderr);
        return 0;
    }

    if (qw_jp2_packet_bit_reader_init(&reader, second_packet,
                                      sizeof(second_packet)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_read_codeblock_header(
            &reader, inclusion_tree, zero_bitplane_tree, 0u, 0u, 1u, 0u,
            &state, &contribution) != QW_JP2_PACKET_OK ||
        !contribution.included || contribution.first_inclusion ||
        contribution.coding_passes != 1u ||
        contribution.codeword_segment_length != 2u ||
        state.coding_passes != 2u || reader.bits_remaining != 2u) {
        qw_jp2_packet_tag_tree_free(inclusion_tree);
        qw_jp2_packet_tag_tree_free(zero_bitplane_tree);
        fputs("packet-header subsequent code-block contribution mismatch\n",
              stderr);
        return 0;
    }

    original_state = state;
    if (qw_jp2_packet_bit_reader_init(&reader, empty_contribution,
                                      sizeof(empty_contribution)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_read_codeblock_header(
            &reader, inclusion_tree, zero_bitplane_tree, 0u, 0u, 2u, 0u,
            &state, &contribution) != QW_JP2_PACKET_OK ||
        contribution.included || contribution.coding_passes != 0u ||
        contribution.codeword_segment_length != 0u ||
        memcmp(&state, &original_state, sizeof(state)) != 0 ||
        reader.bits_remaining != 7u) {
        qw_jp2_packet_tag_tree_free(inclusion_tree);
        qw_jp2_packet_tag_tree_free(zero_bitplane_tree);
        fputs("packet-header empty code-block contribution mismatch\n", stderr);
        return 0;
    }

    original_reader = reader;
    original_state = state;
    original_contribution = contribution;
    rc = qw_jp2_packet_read_codeblock_header(
        &reader, inclusion_tree, zero_bitplane_tree, 0u, 0u, 2u, 1u,
        &state, &contribution);
    if (rc != QW_JP2_PACKET_ERR_UNSUPPORTED ||
        memcmp(&reader, &original_reader, sizeof(reader)) != 0 ||
        memcmp(&state, &original_state, sizeof(state)) != 0 ||
        memcmp(&contribution, &original_contribution,
               sizeof(contribution)) != 0) {
        qw_jp2_packet_tag_tree_free(inclusion_tree);
        qw_jp2_packet_tag_tree_free(zero_bitplane_tree);
        fputs("packet-header unsupported code-block style changed state\n",
              stderr);
        return 0;
    }
    qw_jp2_packet_tag_tree_free(inclusion_tree);
    qw_jp2_packet_tag_tree_free(zero_bitplane_tree);

    inclusion_tree = NULL;
    zero_bitplane_tree = NULL;
    if (qw_jp2_packet_tag_tree_create(1u, 1u, &inclusion_tree) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_tag_tree_create(1u, 1u, &zero_bitplane_tree) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_codeblock_state_init(&state, 4u) != QW_JP2_PACKET_OK ||
        qw_jp2_packet_bit_reader_init(&reader, not_yet_included_packet,
                                      sizeof(not_yet_included_packet)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_read_codeblock_header(
            &reader, inclusion_tree, zero_bitplane_tree, 0u, 0u, 0u, 0u,
            &state, &contribution) != QW_JP2_PACKET_OK ||
        contribution.included || state.included || reader.bits_remaining != 7u ||
        qw_jp2_packet_bit_reader_init(&reader, delayed_first_packet,
                                      sizeof(delayed_first_packet)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_read_codeblock_header(
            &reader, inclusion_tree, zero_bitplane_tree, 0u, 0u, 1u, 0u,
            &state, &contribution) != QW_JP2_PACKET_OK ||
        !contribution.included || !contribution.first_inclusion ||
        contribution.coding_passes != 1u ||
        contribution.codeword_segment_length != 3u ||
        state.coding_passes != 1u) {
        qw_jp2_packet_tag_tree_free(inclusion_tree);
        qw_jp2_packet_tag_tree_free(zero_bitplane_tree);
        fputs("packet-header delayed first inclusion mismatch\n", stderr);
        return 0;
    }
    qw_jp2_packet_tag_tree_free(inclusion_tree);
    qw_jp2_packet_tag_tree_free(zero_bitplane_tree);

    inclusion_tree = NULL;
    zero_bitplane_tree = NULL;
    if (qw_jp2_packet_tag_tree_create(1u, 1u, &inclusion_tree) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_tag_tree_create(1u, 1u, &zero_bitplane_tree) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_codeblock_state_init(&state, 4u) != QW_JP2_PACKET_OK ||
        qw_jp2_packet_bit_reader_init(&reader, truncated_first_packet,
                                      sizeof(truncated_first_packet)) !=
            QW_JP2_PACKET_OK) {
        qw_jp2_packet_tag_tree_free(inclusion_tree);
        qw_jp2_packet_tag_tree_free(zero_bitplane_tree);
        fputs("packet-header transactional code-block setup failed\n", stderr);
        return 0;
    }
    memset(&contribution, 0xa5, sizeof(contribution));
    original_reader = reader;
    original_state = state;
    original_contribution = contribution;
    rc = qw_jp2_packet_read_codeblock_header(
        &reader, inclusion_tree, zero_bitplane_tree, 0u, 0u, 0u, 0u,
        &state, &contribution);
    if (rc != QW_JP2_PACKET_ERR_FORMAT ||
        memcmp(&reader, &original_reader, sizeof(reader)) != 0 ||
        memcmp(&state, &original_state, sizeof(state)) != 0 ||
        memcmp(&contribution, &original_contribution,
               sizeof(contribution)) != 0 ||
        qw_jp2_packet_bit_reader_init(&reader, complete_first_packet,
                                      sizeof(complete_first_packet)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_read_codeblock_header(
            &reader, inclusion_tree, zero_bitplane_tree, 0u, 0u, 0u, 0u,
            &state, &contribution) != QW_JP2_PACKET_OK ||
        !contribution.included || !contribution.first_inclusion ||
        contribution.coding_passes != 2u ||
        contribution.codeword_segment_length != 5u ||
        state.coding_passes != 2u) {
        qw_jp2_packet_tag_tree_free(inclusion_tree);
        qw_jp2_packet_tag_tree_free(zero_bitplane_tree);
        fputs("packet-header code-block parsing was not transactional\n",
              stderr);
        return 0;
    }
    qw_jp2_packet_tag_tree_free(inclusion_tree);
    qw_jp2_packet_tag_tree_free(zero_bitplane_tree);
    return 1;
}

static int test_packet_header_state(void)
{
    static const uint8_t codeblock_header_bits[] = {0xcau};
    static const uint8_t empty_packet[] = {0x00u, 0xa5u};
    static const uint8_t truncated_packet[] = {0xf1u};
    TestBitWriter writer = {{0u}, 0u, 0u, 0u, 0u};
    QwJp2PacketTagTree *inclusion_tree = NULL;
    QwJp2PacketTagTree *zero_bitplane_tree = NULL;
    QwJp2PacketCodeBlockState codeblock;
    QwJp2PacketSubbandHeaderState subband = {0};
    QwJp2PacketCodeBlockContribution contribution;
    QwJp2PacketBitReader reader;
    QwJp2PacketBitReader before_reader;
    uint8_t packet_bytes[2];
    size_t contribution_count = 99u;
    size_t header_bytes = 99u;
    size_t body_bytes = 99u;
    uint8_t is_empty = 9u;
    int ok = 0;

    if (qw_jp2_packet_tag_tree_create(1u, 1u, &inclusion_tree) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_tag_tree_create(1u, 1u, &zero_bitplane_tree) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_codeblock_state_init(&codeblock, 4u) !=
            QW_JP2_PACKET_OK) {
        fputs("packet traversal state setup failed\n", stderr);
        goto cleanup;
    }
    subband.codeblocks_wide = 1u;
    subband.codeblocks_high = 1u;
    subband.codeblock_state_capacity = 1u;
    subband.inclusion_tree = inclusion_tree;
    subband.zero_bitplane_tree = zero_bitplane_tree;
    subband.codeblocks = &codeblock;

    if (!writer_bit(&writer, 1u)) goto setup_failed;
    for (uint8_t bit = 0u; bit < 7u; ++bit) {
        uint8_t value = (uint8_t)((codeblock_header_bits[0] >>
                                   (7u - bit)) & 1u);
        if (!writer_bit(&writer, value)) goto setup_failed;
    }
    if (!writer_finish(&writer) || writer.size != 1u) goto setup_failed;
    packet_bytes[0] = writer.bytes[0];
    packet_bytes[1] = 0xa5u;
    if (qw_jp2_packet_bit_reader_init(&reader, packet_bytes,
                                      sizeof(packet_bytes)) !=
        QW_JP2_PACKET_OK) {
        goto setup_failed;
    }
    if (qw_jp2_packet_read_header(
            &reader, &subband, 1u, 0u, 0u, &contribution, 1u,
            &contribution_count, &is_empty, &header_bytes, &body_bytes) !=
            QW_JP2_PACKET_OK ||
        is_empty != 0u || contribution_count != 1u || header_bytes != 1u ||
        body_bytes != 5u ||
        reader.byte_offset != 1u || reader.bits_remaining != 0u ||
        reader.data[reader.byte_offset] != 0xa5u ||
        !contribution.included || !contribution.first_inclusion ||
        contribution.zero_bitplanes != 0u ||
        contribution.coding_passes != 1u ||
        contribution.codeword_segment_length != 5u ||
        codeblock.coding_passes != 1u || !codeblock.included) {
        fputs("non-empty packet header traversal mismatch\n", stderr);
        goto cleanup;
    }

    writer = (TestBitWriter){{0u}, 0u, 0u, 0u, 0u};
    if (!writer_bit(&writer, 1u) || !writer_bit(&writer, 0u) ||
        !writer_finish(&writer) || writer.size != 1u ||
        qw_jp2_packet_bit_reader_init(&reader, writer.bytes, writer.size) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_read_header(
            &reader, &subband, 1u, 1u, 0u, NULL, 0u,
            &contribution_count, &is_empty, &header_bytes, &body_bytes) !=
            QW_JP2_PACKET_OK ||
        is_empty != 0u || contribution_count != 1u || header_bytes != 1u ||
        body_bytes != 0u ||
        codeblock.coding_passes != 1u) {
        fputs("zero-contribution packet header traversal mismatch\n", stderr);
        goto cleanup;
    }

    if (qw_jp2_packet_bit_reader_init(&reader, empty_packet,
                                      sizeof(empty_packet)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_read_header(
            &reader, &subband, 1u, 2u, 0u, &contribution, 1u,
            &contribution_count, &is_empty, &header_bytes, &body_bytes) !=
            QW_JP2_PACKET_OK ||
        is_empty != 1u || contribution_count != 0u || header_bytes != 1u ||
        body_bytes != 0u ||
        reader.byte_offset != 1u || reader.data[reader.byte_offset] != 0xa5u) {
        fputs("empty packet header traversal mismatch\n", stderr);
        goto cleanup;
    }

    qw_jp2_packet_tag_tree_free(inclusion_tree);
    qw_jp2_packet_tag_tree_free(zero_bitplane_tree);
    inclusion_tree = NULL;
    zero_bitplane_tree = NULL;
    if (qw_jp2_packet_tag_tree_create(1u, 1u, &inclusion_tree) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_tag_tree_create(1u, 1u, &zero_bitplane_tree) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_codeblock_state_init(&codeblock, 4u) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_bit_reader_init(&reader, truncated_packet,
                                      sizeof(truncated_packet)) !=
            QW_JP2_PACKET_OK) {
        fputs("truncated packet traversal setup failed\n", stderr);
        goto cleanup;
    }
    subband.inclusion_tree = inclusion_tree;
    subband.zero_bitplane_tree = zero_bitplane_tree;
    memset(&contribution, 0xa5, sizeof(contribution));
    before_reader = reader;
    contribution_count = 99u;
    is_empty = 9u;
    header_bytes = 99u;
    body_bytes = 99u;
    if (qw_jp2_packet_read_header(
            &reader, &subband, 1u, 0u, 0u, &contribution, 1u,
            &contribution_count, &is_empty, &header_bytes, &body_bytes) !=
            QW_JP2_PACKET_ERR_FORMAT ||
        memcmp(&reader, &before_reader, sizeof(reader)) != 0 ||
        contribution_count != 99u || is_empty != 9u || header_bytes != 99u ||
        body_bytes != 99u ||
        codeblock.coding_passes != 0u || codeblock.included != 0u) {
        fputs("truncated packet header was not rejected transactionally\n",
              stderr);
        goto cleanup;
    }

    ok = 1;
    goto cleanup;

setup_failed:
    fputs("packet header vector setup failed\n", stderr);
cleanup:
    qw_jp2_packet_tag_tree_free(inclusion_tree);
    qw_jp2_packet_tag_tree_free(zero_bitplane_tree);
    return ok;
}

int main(void)
{
    if (!test_bit_stuffing_and_errors() || !test_coding_pass_codewords() ||
        !test_codeword_segment_lengths() || !test_transactional_truncation() ||
        !test_tag_tree_decode() || !test_codeblock_header_state() ||
        !test_packet_header_state()) {
        return 1;
    }
    puts("Quirewise JPEG 2000 packet-header tests passed");
    return 0;
}
