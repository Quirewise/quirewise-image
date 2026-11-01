#ifndef QUIREWISE_JP2_PACKED_HEADERS_H
#define QUIREWISE_JP2_PACKED_HEADERS_H

#include <stddef.h>
#include <stdint.h>

enum {
    QW_JP2_PACKED_HEADER_OK = 0,
    QW_JP2_PACKED_HEADER_ERR_ARGUMENT = -1,
    QW_JP2_PACKED_HEADER_ERR_FORMAT = -2,
    QW_JP2_PACKED_HEADER_ERR_RESOURCE = -3,
    QW_JP2_PACKED_HEADER_MAX_SEGMENTS = 256u,
    QW_JP2_PACKED_HEADER_MAX_BYTES = 64u * 1024u * 1024u
};

/* Payload bytes following Zppm or Zppt in one packed-header marker segment. */
typedef struct QwJp2PackedHeaderSegment {
    uint8_t sequence_index;
    const uint8_t *payload;
    size_t payload_size;
} QwJp2PackedHeaderSegment;

/* Borrowed view into a concatenated PPM stream's Ippm bytes. */
typedef struct QwJp2PacketHeaderSpan {
    const uint8_t *data;
    size_t size;
} QwJp2PacketHeaderSpan;

/*
 * Concatenates marker payloads in Z-index order. For PPM, payloads retain the
 * Nppm/Ippm records; for PPT, payloads are the packed packet-header bytes.
 * The destination must not overlap any source payload. On failure, neither
 * destination nor out_size is modified.
 */
int qw_jp2_packed_header_collect(
    const QwJp2PackedHeaderSegment *segments,
    size_t segment_count,
    uint8_t *destination,
    size_t destination_capacity,
    size_t *out_size);

/* Reads the Nppm/Ippm record for one tile-part from a collected PPM stream. */
int qw_jp2_ppm_tile_part_header(const uint8_t *packed_data,
                                size_t packed_size,
                                size_t tile_part_index,
                                QwJp2PacketHeaderSpan *out_header);

/* Validates exactly one Nppm/Ippm record for each codestream tile-part. */
int qw_jp2_ppm_validate_tile_part_count(const uint8_t *packed_data,
                                        size_t packed_size,
                                        size_t tile_part_count);

#endif
