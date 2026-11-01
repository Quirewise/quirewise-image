#include "jp2_packet_body.h"

#include "jp2_mq.h"

#include <stdlib.h>
#include <string.h>

static uint16_t qw_jp2_packet_body_max_passes(uint8_t bitplanes)
{
    return bitplanes ? (uint16_t)(3u * (uint16_t)bitplanes - 2u) : 0u;
}

static int qw_jp2_packet_body_cursor_valid(const QwJp2PacketBodyCursor *cursor)
{
    return cursor && cursor->position <= cursor->size &&
           cursor->size <= (size_t)QW_JP2_PACKET_BODY_MAX_BYTES &&
           (cursor->data || cursor->size == 0u);
}

static int qw_jp2_packet_codeblock_body_valid(
    const QwJp2PacketCodeBlockBody *body)
{
    if (!body || body->capacity >
                     (size_t)QW_JP2_PACKET_CODEBLOCK_MAX_BYTES ||
        body->size > body->capacity || (body->capacity && !body->data) ||
        body->segment_count > body->segment_capacity ||
        body->segment_capacity > QW_JP2_PACKET_MAX_PASSES ||
        (body->segment_capacity != 0u && !body->segment_lengths) ||
        body->initialized > 1u) {
        return 0;
    }
    if (!body->initialized) {
        return body->size == 0u && body->coding_passes == 0u &&
               body->max_bitplanes == 0u && body->zero_bitplanes == 0u &&
               body->segment_count == 0u;
    }
    if (body->max_bitplanes == 0u ||
        body->max_bitplanes > QW_JP2_PACKET_MAX_BIT_PLANES ||
        body->zero_bitplanes >= body->max_bitplanes ||
        body->coding_passes == 0u) {
        return 0;
    }
    return body->coding_passes <= qw_jp2_packet_body_max_passes(
                                      (uint8_t)(body->max_bitplanes -
                                                body->zero_bitplanes));
}

static int qw_jp2_packet_header_state_valid(
    const QwJp2PacketCodeBlockState *state)
{
    if (!state || state->initialized != 1u || state->included > 1u ||
        state->max_bitplanes == 0u ||
        state->max_bitplanes > QW_JP2_PACKET_MAX_BIT_PLANES ||
        state->lblock < 3u ||
        state->lblock > QW_JP2_PACKET_MAX_LENGTH_BITS) {
        return 0;
    }
    if (!state->included) {
        return state->coding_passes == 0u && state->zero_bitplanes == 0u &&
               state->lblock == 3u;
    }
    if (state->zero_bitplanes >= state->max_bitplanes ||
        state->coding_passes == 0u) {
        return 0;
    }
    return state->coding_passes <= qw_jp2_packet_body_max_passes(
                                      (uint8_t)(state->max_bitplanes -
                                                state->zero_bitplanes));
}

int qw_jp2_packet_body_cursor_init(QwJp2PacketBodyCursor *cursor,
                                   const uint8_t *data,
                                   size_t size)
{
    QwJp2PacketBodyCursor next;
    if (!cursor || (!data && size != 0u)) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }
    if (size > (size_t)QW_JP2_PACKET_BODY_MAX_BYTES) {
        return QW_JP2_PACKET_ERR_RESOURCE;
    }
    next.data = data;
    next.size = size;
    next.position = 0u;
    *cursor = next;
    return QW_JP2_PACKET_OK;
}

int qw_jp2_packet_codeblock_body_init(QwJp2PacketCodeBlockBody *body,
                                      uint8_t *storage,
                                      size_t capacity)
{
    QwJp2PacketCodeBlockBody next;
    if (!body || (capacity != 0u && !storage)) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }
    if (capacity > (size_t)QW_JP2_PACKET_CODEBLOCK_MAX_BYTES) {
        return QW_JP2_PACKET_ERR_RESOURCE;
    }
    memset(&next, 0, sizeof(next));
    next.data = storage;
    next.capacity = capacity;
    *body = next;
    return QW_JP2_PACKET_OK;
}

static int qw_jp2_packet_body_reserve_segments(
    QwJp2PacketCodeBlockBody *body,
    size_t additional_count,
    size_t *budget_remaining)
{
    size_t required_count;
    size_t capacity;
    size_t growth_bytes;
    uint32_t *replacement;

    if (!body || !budget_remaining ||
        body->segment_count > QW_JP2_PACKET_MAX_PASSES ||
        additional_count > QW_JP2_PACKET_MAX_PASSES - body->segment_count) {
        return QW_JP2_PACKET_ERR_RESOURCE;
    }
    required_count = body->segment_count + additional_count;
    if (required_count <= body->segment_capacity) return QW_JP2_PACKET_OK;
    capacity = body->segment_capacity ? body->segment_capacity : 1u;
    while (capacity < required_count) {
        capacity *= 2u;
        if (capacity > QW_JP2_PACKET_MAX_PASSES) {
            capacity = QW_JP2_PACKET_MAX_PASSES;
            break;
        }
    }
    growth_bytes = (capacity - body->segment_capacity) *
                   sizeof(*body->segment_lengths);
    if (growth_bytes > *budget_remaining) {
        return QW_JP2_PACKET_ERR_RESOURCE;
    }
    replacement = (uint32_t *)realloc(
        body->segment_lengths, capacity * sizeof(*body->segment_lengths));
    if (!replacement) return QW_JP2_PACKET_ERR_MEMORY;
    body->segment_lengths = replacement;
    body->segment_capacity = capacity;
    *budget_remaining -= growth_bytes;
    return QW_JP2_PACKET_OK;
}

int qw_jp2_packet_body_append_codeblock_with_segments(
    QwJp2PacketBodyCursor *cursor,
    const QwJp2PacketCodeBlockContribution *contribution,
    const QwJp2PacketCodeBlockState *header_state,
    QwJp2PacketCodeBlockBody *body,
    uint8_t codeblock_style,
    const uint32_t *segment_lengths,
    size_t segment_length_count,
    size_t *segment_metadata_budget_remaining)
{
    uint32_t contribution_end_passes;
    size_t segment_size;
    int save_segments;

    if (!cursor || !contribution || !header_state || !body) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }
    if (!qw_jp2_packet_body_cursor_valid(cursor) ||
        !qw_jp2_packet_codeblock_body_valid(body) ||
        !qw_jp2_packet_header_state_valid(header_state) ||
        contribution->included > 1u || contribution->first_inclusion > 1u ||
        (segment_length_count != 0u && !segment_lengths)) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    if ((codeblock_style & (uint8_t)~0x36u) != 0u) {
        return QW_JP2_PACKET_ERR_UNSUPPORTED;
    }
    save_segments = (codeblock_style & 0x04u) != 0u;

    if (!contribution->included) {
        if (contribution->first_inclusion != 0u ||
            contribution->coding_passes != 0u ||
            contribution->segment_count != 0u ||
            contribution->codeword_segment_length != 0u ||
            contribution->zero_bitplanes != 0u ||
            header_state->included != body->initialized ||
            header_state->coding_passes != body->coding_passes ||
            (body->initialized &&
             (header_state->max_bitplanes != body->max_bitplanes ||
              header_state->zero_bitplanes != body->zero_bitplanes))) {
            return QW_JP2_PACKET_ERR_FORMAT;
        }
        return QW_JP2_PACKET_OK;
    }

    if (!header_state->included || contribution->coding_passes == 0u ||
        contribution->segment_count == 0u ||
        contribution->zero_bitplanes != header_state->zero_bitplanes ||
        header_state->coding_passes < contribution->coding_passes) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    contribution_end_passes = (uint32_t)body->coding_passes +
                              contribution->coding_passes;
    if (contribution_end_passes != header_state->coding_passes ||
        (contribution->first_inclusion != 0u) ==
            (body->initialized != 0u)) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    if (contribution->first_inclusion) {
        if (body->size != 0u || body->coding_passes != 0u ||
            contribution_end_passes != contribution->coding_passes) {
            return QW_JP2_PACKET_ERR_FORMAT;
        }
    } else if (body->max_bitplanes != header_state->max_bitplanes ||
               body->zero_bitplanes != header_state->zero_bitplanes) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }

    if (save_segments &&
        (segment_length_count != contribution->segment_count ||
         contribution->segment_count != contribution->coding_passes ||
         !segment_lengths || !segment_metadata_budget_remaining ||
         body->segment_count != body->coding_passes)) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    if (!save_segments && segment_length_count != 0u) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }

    segment_size = (size_t)contribution->codeword_segment_length;
    if (segment_size > cursor->size - cursor->position) {
        return QW_JP2_PACKET_ERR_FORMAT;
    }
    if (segment_size > body->capacity - body->size ||
        segment_size > (size_t)QW_JP2_PACKET_CODEBLOCK_MAX_BYTES - body->size) {
        return QW_JP2_PACKET_ERR_RESOURCE;
    }
    if (save_segments) {
        int reserve_status = qw_jp2_packet_body_reserve_segments(
            body, segment_length_count, segment_metadata_budget_remaining);
        if (reserve_status != QW_JP2_PACKET_OK) return reserve_status;
    }

    if (segment_size != 0u) {
        memcpy(body->data + body->size, cursor->data + cursor->position,
               segment_size);
    }
    cursor->position += segment_size;
    body->size += segment_size;
    if (save_segments) {
        memcpy(body->segment_lengths + body->segment_count, segment_lengths,
               segment_length_count * sizeof(*segment_lengths));
        body->segment_count += segment_length_count;
    }
    body->coding_passes = header_state->coding_passes;
    body->max_bitplanes = header_state->max_bitplanes;
    body->zero_bitplanes = header_state->zero_bitplanes;
    body->initialized = 1u;
    return QW_JP2_PACKET_OK;
}

int qw_jp2_packet_body_append_codeblock(
    QwJp2PacketBodyCursor *cursor,
    const QwJp2PacketCodeBlockContribution *contribution,
    const QwJp2PacketCodeBlockState *header_state,
    QwJp2PacketCodeBlockBody *body)
{
    QwJp2PacketCodeBlockContribution legacy_contribution;
    if (!contribution) return QW_JP2_PACKET_ERR_ARGUMENT;
    legacy_contribution = *contribution;
    if (legacy_contribution.included &&
        legacy_contribution.segment_count == 0u) {
        legacy_contribution.segment_count = 1u;
    }
    return qw_jp2_packet_body_append_codeblock_with_segments(
        cursor, &legacy_contribution, header_state, body, 0u, NULL, 0u,
        NULL);
}

int qw_jp2_packet_body_cursor_finish(const QwJp2PacketBodyCursor *cursor)
{
    if (!cursor) return QW_JP2_PACKET_ERR_ARGUMENT;
    if (!qw_jp2_packet_body_cursor_valid(cursor)) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }
    return cursor->position == cursor->size
               ? QW_JP2_PACKET_OK : QW_JP2_PACKET_ERR_FORMAT;
}

static int qw_jp2_packet_body_tier1_status(int status)
{
    if (status == QW_JP2_T1_OK) return QW_JP2_PACKET_OK;
    if (status == QW_JP2_T1_ERR_ARGUMENT) return QW_JP2_PACKET_ERR_ARGUMENT;
    if (status == QW_JP2_T1_ERR_MEMORY) return QW_JP2_PACKET_ERR_MEMORY;
    if (status == QW_JP2_T1_ERR_UNSUPPORTED) {
        return QW_JP2_PACKET_ERR_UNSUPPORTED;
    }
    return QW_JP2_PACKET_ERR_FORMAT;
}

int qw_jp2_packet_codeblock_decode(
    const QwJp2PacketCodeBlockBody *body,
    uint32_t width,
    uint32_t height,
    QwJp2Tier1Orientation orientation,
    uint8_t codeblock_style,
    QwJp2Tier1Coefficient *coefficients,
    size_t coefficient_count)
{
    QwJp2MqDecoder mq = {0};
    uint8_t bitplanes = 0u;
    uint16_t coding_passes = 0u;
    int rc;

    if (!body || !coefficients || !qw_jp2_packet_codeblock_body_valid(body)) {
        return QW_JP2_PACKET_ERR_ARGUMENT;
    }
    if ((codeblock_style & 0x04u) != 0u) {
        bitplanes = body->initialized
                        ? (uint8_t)(body->max_bitplanes - body->zero_bitplanes)
                        : 0u;
        coding_passes = body->initialized ? body->coding_passes : 0u;
        if (body->segment_count != coding_passes ||
            (coding_passes != 0u && !body->segment_lengths)) {
            return QW_JP2_PACKET_ERR_FORMAT;
        }
        rc = qw_jp2_t1_decode_mq_codeblock_segments(
            body->data, body->size, body->segment_lengths,
            body->segment_count, width, height, bitplanes, orientation,
            codeblock_style, coefficients, coefficient_count);
        return qw_jp2_packet_body_tier1_status(rc);
    }
    if (body->initialized) {
        bitplanes = (uint8_t)(body->max_bitplanes - body->zero_bitplanes);
        coding_passes = body->coding_passes;
        if (qw_jp2_mq_init(&mq, body->data, body->size) != QW_JP2_MQ_OK) {
            return QW_JP2_PACKET_ERR_FORMAT;
        }
    }
    rc = qw_jp2_t1_decode_mq_codeblock(
        &mq, width, height, bitplanes, coding_passes, orientation,
        codeblock_style, coefficients, coefficient_count);
    return qw_jp2_packet_body_tier1_status(rc);
}
