#include "jp2_packet_body.h"

#include <stdio.h>
#include <string.h>

static int bytes_equal(const uint8_t *left,
                       const uint8_t *right,
                       size_t size)
{
    return memcmp(left, right, size) == 0;
}

static int test_assembled_segment_reaches_tier1(void)
{
    static const uint8_t first_packet_body[] = {0x08u};
    static const uint8_t second_packet_body[] = {0xa0u, 0x57u, 0xabu};
    static const uint8_t payload[] = {0x08u, 0xa0u, 0x57u, 0xabu};
    uint8_t storage[sizeof(payload)] = {0u};
    QwJp2PacketBodyCursor cursor;
    QwJp2PacketCodeBlockBody body;
    QwJp2PacketCodeBlockContribution contribution = {
        7u, (uint32_t)sizeof(first_packet_body), 1u, 1u, 0u
    };
    QwJp2PacketCodeBlockState header_state = {7u, 6u, 0u, 3u, 1u, 1u};
    QwJp2Tier1Coefficient coefficients[1];

    if (qw_jp2_packet_codeblock_body_init(&body, storage, sizeof(storage)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_body_cursor_init(&cursor, first_packet_body,
                                       sizeof(first_packet_body)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_body_append_codeblock(
            &cursor, &contribution, &header_state, &body) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_body_cursor_finish(&cursor) != QW_JP2_PACKET_OK ||
        body.size != sizeof(first_packet_body) || body.coding_passes != 7u) {
        fputs("first packet code-block contribution did not append\n", stderr);
        return 0;
    }

    contribution.coding_passes = 9u;
    contribution.codeword_segment_length = (uint32_t)sizeof(second_packet_body);
    contribution.first_inclusion = 0u;
    header_state.coding_passes = 16u;
    if (qw_jp2_packet_body_cursor_init(
            &cursor, second_packet_body, sizeof(second_packet_body)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_body_append_codeblock(
            &cursor, &contribution, &header_state, &body) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_body_cursor_finish(&cursor) != QW_JP2_PACKET_OK ||
        body.size != sizeof(payload) || body.coding_passes != 16u ||
        !bytes_equal(body.data, payload, sizeof(payload)) ||
        qw_jp2_packet_codeblock_decode(
            &body, 1u, 1u, QW_JP2_T1_ORIENTATION_HH, 0x20u,
            coefficients, 1u) != QW_JP2_PACKET_OK) {
        fputs("assembled packet code-block did not reach Tier-1\n", stderr);
        return 0;
    }
    if (coefficients[0].magnitude != 32u || coefficients[0].negative != 1u) {
        fputs("assembled Tier-1 coefficients mismatch\n", stderr);
        return 0;
    }
    return 1;
}

static int test_layer_contributions_and_empty_packet(void)
{
    static const uint8_t first_bytes[] = {0xa1u, 0xa2u};
    static const uint8_t second_bytes[] = {0xb1u};
    static const uint8_t expected[] = {0xa1u, 0xa2u, 0xb1u};
    uint8_t storage[sizeof(expected)] = {0u};
    QwJp2PacketBodyCursor cursor;
    QwJp2PacketCodeBlockBody body;
    QwJp2PacketCodeBlockContribution contribution = {1u, 2u, 1u, 1u, 0u};
    QwJp2PacketCodeBlockState header_state = {1u, 2u, 0u, 3u, 1u, 1u};

    if (qw_jp2_packet_codeblock_body_init(&body, storage, sizeof(storage)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_body_cursor_init(&cursor, first_bytes,
                                       sizeof(first_bytes)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_body_append_codeblock(
            &cursor, &contribution, &header_state, &body) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_body_cursor_finish(&cursor) != QW_JP2_PACKET_OK) {
        fputs("first layer contribution assembly failed\n", stderr);
        return 0;
    }

    contribution.coding_passes = 1u;
    contribution.codeword_segment_length = 1u;
    contribution.first_inclusion = 0u;
    header_state.coding_passes = 2u;
    if (qw_jp2_packet_body_cursor_init(&cursor, second_bytes,
                                       sizeof(second_bytes)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_body_append_codeblock(
            &cursor, &contribution, &header_state, &body) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_body_cursor_finish(&cursor) != QW_JP2_PACKET_OK) {
        fputs("later layer contribution assembly failed\n", stderr);
        return 0;
    }

    contribution = (QwJp2PacketCodeBlockContribution){0u, 0u, 0u, 0u, 0u};
    if (qw_jp2_packet_body_cursor_init(&cursor, NULL, 0u) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_body_append_codeblock(
            &cursor, &contribution, &header_state, &body) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_body_cursor_finish(&cursor) != QW_JP2_PACKET_OK ||
        body.size != sizeof(expected) || body.coding_passes != 2u ||
        !bytes_equal(body.data, expected, sizeof(expected))) {
        fputs("empty packet contribution changed accumulated code-block\n",
              stderr);
        return 0;
    }
    return 1;
}

static int test_errors_are_transactional(void)
{
    static const uint8_t short_packet[] = {0x11u};
    uint8_t storage[1] = {0x5au};
    QwJp2PacketBodyCursor cursor;
    QwJp2PacketCodeBlockBody body;
    QwJp2PacketCodeBlockContribution contribution = {1u, 2u, 1u, 1u, 0u};
    QwJp2PacketCodeBlockState header_state = {1u, 2u, 0u, 3u, 1u, 1u};
    size_t size_before;

    if (qw_jp2_packet_codeblock_body_init(&body, storage, sizeof(storage)) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_body_cursor_init(&cursor, short_packet,
                                       sizeof(short_packet)) !=
            QW_JP2_PACKET_OK) {
        return 0;
    }
    size_before = body.size;
    if (qw_jp2_packet_body_append_codeblock(
            &cursor, &contribution, &header_state, &body) !=
            QW_JP2_PACKET_ERR_FORMAT ||
        cursor.position != 0u || body.size != size_before ||
        body.coding_passes != 0u || storage[0] != 0x5au) {
        fputs("truncated packet body changed cursor or output\n", stderr);
        return 0;
    }

    if (qw_jp2_packet_codeblock_body_init(&body, storage, 0u) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_body_cursor_init(&cursor, short_packet,
                                       sizeof(short_packet)) !=
            QW_JP2_PACKET_OK) {
        return 0;
    }
    contribution.codeword_segment_length = 1u;
    if (qw_jp2_packet_body_append_codeblock(
            &cursor, &contribution, &header_state, &body) !=
            QW_JP2_PACKET_ERR_RESOURCE ||
        cursor.position != 0u || body.size != 0u || storage[0] != 0x5au) {
        fputs("small code-block capacity rejection mismatch\n", stderr);
        return 0;
    }

    if (qw_jp2_packet_body_cursor_init(&cursor, short_packet,
                                       sizeof(short_packet)) !=
            QW_JP2_PACKET_OK) {
        return 0;
    }
    header_state.coding_passes = 2u;
    if (qw_jp2_packet_body_append_codeblock(
            &cursor, &contribution, &header_state, &body) !=
            QW_JP2_PACKET_ERR_FORMAT ||
        cursor.position != 0u || body.size != 0u || storage[0] != 0x5au) {
        fputs("header/body pass mismatch rejection was not transactional\n",
              stderr);
        return 0;
    }
    return 1;
}

static int test_resource_limits_and_finish(void)
{
    static const uint8_t byte = 0u;
    QwJp2PacketBodyCursor cursor = {&byte, 1u, 1u};
    QwJp2PacketCodeBlockBody body;
    uint8_t storage[1];

    if (qw_jp2_packet_body_cursor_init(
            &cursor, &byte,
            (size_t)QW_JP2_PACKET_BODY_MAX_BYTES + 1u) !=
            QW_JP2_PACKET_ERR_RESOURCE ||
        cursor.data != &byte || cursor.size != 1u || cursor.position != 1u) {
        fputs("packet body resource limit did not preserve cursor\n", stderr);
        return 0;
    }
    if (qw_jp2_packet_codeblock_body_init(
            &body, storage,
            (size_t)QW_JP2_PACKET_CODEBLOCK_MAX_BYTES + 1u) !=
        QW_JP2_PACKET_ERR_RESOURCE) {
        fputs("code-block storage resource limit mismatch\n", stderr);
        return 0;
    }
    if (qw_jp2_packet_body_cursor_init(&cursor, &byte, 1u) !=
            QW_JP2_PACKET_OK ||
        qw_jp2_packet_body_cursor_finish(&cursor) !=
            QW_JP2_PACKET_ERR_FORMAT) {
        fputs("unconsumed packet body bytes were accepted\n", stderr);
        return 0;
    }
    return 1;
}

int main(void)
{
    if (!test_assembled_segment_reaches_tier1() ||
        !test_layer_contributions_and_empty_packet() ||
        !test_errors_are_transactional() || !test_resource_limits_and_finish()) {
        return 1;
    }
    puts("Quirewise JPEG 2000 packet-body tests passed");
    return 0;
}
