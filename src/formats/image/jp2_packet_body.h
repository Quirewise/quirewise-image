#ifndef QUIREWISE_JP2_PACKET_BODY_H
#define QUIREWISE_JP2_PACKET_BODY_H

#include "jp2_packet_header.h"
#include "jp2_tier1_decoder.h"

enum {
    QW_JP2_PACKET_BODY_MAX_BYTES = 64u * 1024u * 1024u,
    QW_JP2_PACKET_CODEBLOCK_MAX_BYTES = 64u * 1024u * 1024u,
    QW_JP2_PACKET_SEGMENT_METADATA_MAX_BYTES = 16u * 1024u * 1024u
};

typedef struct QwJp2PacketBodyCursor {
    const uint8_t *data;
    size_t size;
    size_t position;
} QwJp2PacketBodyCursor;

/* Caller-owned storage for one regular-style code-block codeword segment. */
typedef struct QwJp2PacketCodeBlockBody {
    uint8_t *data;
    size_t capacity;
    size_t size;
    uint32_t *segment_lengths;
    size_t segment_count;
    size_t segment_capacity;
    uint16_t coding_passes;
    uint8_t max_bitplanes;
    uint8_t zero_bitplanes;
    uint8_t initialized;
} QwJp2PacketCodeBlockBody;

int qw_jp2_packet_body_cursor_init(QwJp2PacketBodyCursor *cursor,
                                  const uint8_t *data,
                                  size_t size);
int qw_jp2_packet_codeblock_body_init(QwJp2PacketCodeBlockBody *body,
                                      uint8_t *storage,
                                      size_t capacity);

/*
 * Appends one packet contribution to a code-block's regular codeword segment.
 * header_state is the state after parsing this code-block's packet header.
 * On failure, cursor and body are unchanged. Input packet bytes must not
 * overlap the code-block storage.
 */
int qw_jp2_packet_body_append_codeblock(
    QwJp2PacketBodyCursor *cursor,
    const QwJp2PacketCodeBlockContribution *contribution,
    const QwJp2PacketCodeBlockState *header_state,
    QwJp2PacketCodeBlockBody *body);

int qw_jp2_packet_body_append_codeblock_with_segments(
    QwJp2PacketBodyCursor *cursor,
    const QwJp2PacketCodeBlockContribution *contribution,
    const QwJp2PacketCodeBlockState *header_state,
    QwJp2PacketCodeBlockBody *body,
    uint8_t codeblock_style,
    const uint32_t *segment_lengths,
    size_t segment_length_count,
    size_t *segment_metadata_budget_remaining);

/* Requires all code-block contributions in this packet body to be consumed. */
int qw_jp2_packet_body_cursor_finish(const QwJp2PacketBodyCursor *cursor);

/* Decodes the assembled regular-style code-block segment through MQ/Tier-1. */
int qw_jp2_packet_codeblock_decode(
    const QwJp2PacketCodeBlockBody *body,
    uint32_t width,
    uint32_t height,
    QwJp2Tier1Orientation orientation,
    uint8_t codeblock_style,
    QwJp2Tier1Coefficient *coefficients,
    size_t coefficient_count);

#endif
