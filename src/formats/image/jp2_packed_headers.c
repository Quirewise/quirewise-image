#include "jp2_packed_headers.h"

#include <string.h>

static uint32_t qw_jp2_packed_header_read_u32_be(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u) |
           ((uint32_t)data[2] << 8u) | (uint32_t)data[3];
}

int qw_jp2_packed_header_collect(
    const QwJp2PackedHeaderSegment *segments,
    size_t segment_count,
    uint8_t *destination,
    size_t destination_capacity,
    size_t *out_size)
{
    size_t total_size = 0u;

    if (!segments || segment_count == 0u ||
        segment_count > QW_JP2_PACKED_HEADER_MAX_SEGMENTS ||
        !destination || !out_size) {
        return QW_JP2_PACKED_HEADER_ERR_ARGUMENT;
    }

    for (size_t i = 0u; i < segment_count; ++i) {
        const QwJp2PackedHeaderSegment *segment = &segments[i];
        if (segment->sequence_index != (uint8_t)i) {
            return QW_JP2_PACKED_HEADER_ERR_FORMAT;
        }
        if (!segment->payload && segment->payload_size != 0u) {
            return QW_JP2_PACKED_HEADER_ERR_ARGUMENT;
        }
        if (segment->payload_size >
            (size_t)QW_JP2_PACKED_HEADER_MAX_BYTES - total_size) {
            return QW_JP2_PACKED_HEADER_ERR_RESOURCE;
        }
        total_size += segment->payload_size;
    }
    if (total_size > destination_capacity) {
        return QW_JP2_PACKED_HEADER_ERR_RESOURCE;
    }

    total_size = 0u;
    for (size_t i = 0u; i < segment_count; ++i) {
        const QwJp2PackedHeaderSegment *segment = &segments[i];
        if (segment->payload_size != 0u) {
            memcpy(destination + total_size, segment->payload,
                   segment->payload_size);
            total_size += segment->payload_size;
        }
    }
    *out_size = total_size;
    return QW_JP2_PACKED_HEADER_OK;
}

int qw_jp2_ppm_tile_part_header(const uint8_t *packed_data,
                                size_t packed_size,
                                size_t tile_part_index,
                                QwJp2PacketHeaderSpan *out_header)
{
    size_t position = 0u;
    size_t remaining_index = tile_part_index;

    if ((!packed_data && packed_size != 0u) || !out_header) {
        return QW_JP2_PACKED_HEADER_ERR_ARGUMENT;
    }
    if (packed_size > (size_t)QW_JP2_PACKED_HEADER_MAX_BYTES) {
        return QW_JP2_PACKED_HEADER_ERR_RESOURCE;
    }

    for (;;) {
        uint32_t header_size;
        size_t available;
        QwJp2PacketHeaderSpan header;

        if (position > packed_size || packed_size - position < 4u) {
            return QW_JP2_PACKED_HEADER_ERR_FORMAT;
        }
        header_size = qw_jp2_packed_header_read_u32_be(packed_data + position);
        position += 4u;
        available = packed_size - position;
        if ((uint64_t)header_size > (uint64_t)available) {
            return QW_JP2_PACKED_HEADER_ERR_FORMAT;
        }
        header.data = packed_data + position;
        header.size = (size_t)header_size;
        if (remaining_index == 0u) {
            *out_header = header;
            return QW_JP2_PACKED_HEADER_OK;
        }
        --remaining_index;
        position += header.size;
    }
}

int qw_jp2_ppm_validate_tile_part_count(const uint8_t *packed_data,
                                        size_t packed_size,
                                        size_t tile_part_count)
{
    size_t position = 0u;

    if ((!packed_data && packed_size != 0u)) {
        return QW_JP2_PACKED_HEADER_ERR_ARGUMENT;
    }
    if (packed_size > (size_t)QW_JP2_PACKED_HEADER_MAX_BYTES) {
        return QW_JP2_PACKED_HEADER_ERR_RESOURCE;
    }
    if (tile_part_count > packed_size / 4u) {
        return QW_JP2_PACKED_HEADER_ERR_FORMAT;
    }

    for (size_t i = 0u; i < tile_part_count; ++i) {
        uint32_t header_size;
        size_t available;

        if (position > packed_size || packed_size - position < 4u) {
            return QW_JP2_PACKED_HEADER_ERR_FORMAT;
        }
        header_size = qw_jp2_packed_header_read_u32_be(packed_data + position);
        position += 4u;
        available = packed_size - position;
        if ((uint64_t)header_size > (uint64_t)available) {
            return QW_JP2_PACKED_HEADER_ERR_FORMAT;
        }
        position += (size_t)header_size;
    }
    return position == packed_size ? QW_JP2_PACKED_HEADER_OK
                                   : QW_JP2_PACKED_HEADER_ERR_FORMAT;
}
