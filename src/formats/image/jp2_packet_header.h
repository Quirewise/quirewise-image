#ifndef QUIREWISE_JP2_PACKET_HEADER_H
#define QUIREWISE_JP2_PACKET_HEADER_H

#include <stddef.h>
#include <stdint.h>

enum {
    QW_JP2_PACKET_OK = 0,
    QW_JP2_PACKET_ERR_ARGUMENT = -1,
    QW_JP2_PACKET_ERR_FORMAT = -2,
    QW_JP2_PACKET_ERR_MEMORY = -3,
    QW_JP2_PACKET_ERR_RESOURCE = -4,
    QW_JP2_PACKET_ERR_UNSUPPORTED = -5,
    QW_JP2_PACKET_MAX_PASSES = 164u,
    QW_JP2_PACKET_MAX_LENGTH_BITS = 32u,
    QW_JP2_PACKET_MAX_BIT_PLANES = 63u,
    QW_JP2_PACKET_MAX_TAG_TREE_NODES = 2000000u,
    QW_JP2_PACKET_MAX_TAG_TREE_LEVELS = 33u,
    QW_JP2_PACKET_MAX_TAG_TREE_THRESHOLD = 65535u,
    QW_JP2_PACKET_MAX_SUBBANDS = 3u
};

typedef struct QwJp2PacketBitReader {
    const uint8_t *data;
    size_t size;
    size_t byte_offset;
    uint8_t current_byte;
    uint8_t bits_remaining;
    uint8_t previous_was_ff;
} QwJp2PacketBitReader;

typedef struct QwJp2PacketTagTree QwJp2PacketTagTree;

typedef struct QwJp2PacketCodeBlockState {
    uint16_t coding_passes;
    uint8_t max_bitplanes;
    uint8_t zero_bitplanes;
    uint8_t lblock;
    uint8_t included;
    uint8_t initialized;
} QwJp2PacketCodeBlockState;

typedef struct QwJp2PacketCodeBlockContribution {
    uint16_t coding_passes;
    uint32_t codeword_segment_length;
    uint8_t included;
    uint8_t first_inclusion;
    uint8_t zero_bitplanes;
    uint16_t segment_count;
    size_t segment_offset;
} QwJp2PacketCodeBlockContribution;

/* Mutable per-precinct state for one resolution's sub-band. Code-block states
 * use raster order matching the two tag trees. */
typedef struct QwJp2PacketSubbandHeaderState {
    uint32_t codeblocks_wide;
    uint32_t codeblocks_high;
    size_t codeblock_state_capacity;
    QwJp2PacketTagTree *inclusion_tree;
    QwJp2PacketTagTree *zero_bitplane_tree;
    QwJp2PacketCodeBlockState *codeblocks;
} QwJp2PacketSubbandHeaderState;

int qw_jp2_packet_bit_reader_init(QwJp2PacketBitReader *reader,
                                  const uint8_t *data,
                                  size_t size);
int qw_jp2_packet_read_bit(QwJp2PacketBitReader *reader, uint8_t *out_bit);
int qw_jp2_packet_read_bits(QwJp2PacketBitReader *reader,
                            uint8_t bit_count,
                            uint32_t *out_value);
int qw_jp2_packet_read_coding_passes(QwJp2PacketBitReader *reader,
                                     uint16_t *out_passes);

/* Reads one Lblock increment prefix followed by one length per codeword
 * segment. pass_counts supplies the number of new coding passes in each
 * segment; lengths and reader state change only if the whole parse succeeds. */
int qw_jp2_packet_read_segment_lengths(
    QwJp2PacketBitReader *reader,
    uint8_t *lblock,
    const uint16_t *pass_counts,
    size_t segment_count,
    uint32_t *lengths,
    size_t length_capacity);

/* Aligns the header and consumes the required stuffed byte after 0xFF. */
int qw_jp2_packet_header_align(QwJp2PacketBitReader *reader);
size_t qw_jp2_packet_header_bytes_consumed(
    const QwJp2PacketBitReader *reader);

/* Creates a zero-initialized JPEG 2000 tag tree for one precinct subband,
 * rejecting dimensions whose hierarchy exceeds the node resource cap. */
int qw_jp2_packet_tag_tree_create(uint32_t width,
                                  uint32_t height,
                                  QwJp2PacketTagTree **out_tree);
void qw_jp2_packet_tag_tree_free(QwJp2PacketTagTree *tree);

/* Decodes whether the selected leaf value is below threshold. The reader and
 * tree are updated only if the full root-to-leaf query succeeds. */
int qw_jp2_packet_tag_tree_decode(QwJp2PacketBitReader *reader,
                                  QwJp2PacketTagTree *tree,
                                  uint32_t x,
                                  uint32_t y,
                                  uint32_t threshold,
                                  uint8_t *out_below_threshold);

int qw_jp2_packet_codeblock_state_init(
    QwJp2PacketCodeBlockState *state,
    uint8_t max_bitplanes);

/* Parses one code-block entry after the packet's non-empty flag. This first
 * integration supports regular code-block style only, which contributes one
 * codeword segment per packet entry. */
int qw_jp2_packet_read_codeblock_header(
    QwJp2PacketBitReader *reader,
    QwJp2PacketTagTree *inclusion_tree,
    QwJp2PacketTagTree *zero_bitplane_tree,
    uint32_t x,
    uint32_t y,
    uint32_t layer_index,
    uint8_t codeblock_style,
    QwJp2PacketCodeBlockState *state,
    QwJp2PacketCodeBlockContribution *out_contribution);

/* Variant that also copies the per-segment lengths into a caller-owned flat
 * array. out_segment_length_count is committed only when parsing succeeds. */
int qw_jp2_packet_read_codeblock_header_with_segments(
    QwJp2PacketBitReader *reader,
    QwJp2PacketTagTree *inclusion_tree,
    QwJp2PacketTagTree *zero_bitplane_tree,
    uint32_t x,
    uint32_t y,
    uint32_t layer_index,
    uint8_t codeblock_style,
    QwJp2PacketCodeBlockState *state,
    QwJp2PacketCodeBlockContribution *out_contribution,
    uint32_t *segment_lengths,
    size_t segment_length_capacity,
    size_t *inout_segment_length_count);

/* Reads one packet header across its LL or HL/LH/HH precinct sub-bands. For a
 * non-empty packet, one contribution record is returned per code-block in
 * sub-band then raster order; pass NULL/zero for contributions to obtain only
 * the summed body extent. Header bytes include byte alignment. Reader and
 * scalar outputs are committed only on success; contribution contents are
 * undefined after failure. If a multi-code-block header fails partway through,
 * discard the supplied sub-band state. */
int qw_jp2_packet_read_header(
    QwJp2PacketBitReader *reader,
    QwJp2PacketSubbandHeaderState *subbands,
    size_t subband_count,
    uint32_t layer_index,
    uint8_t codeblock_style,
    QwJp2PacketCodeBlockContribution *contributions,
    size_t contribution_capacity,
    size_t *out_contribution_count,
    uint8_t *out_is_empty,
    size_t *out_header_bytes,
    size_t *out_body_bytes);

int qw_jp2_packet_read_header_with_segments(
    QwJp2PacketBitReader *reader,
    QwJp2PacketSubbandHeaderState *subbands,
    size_t subband_count,
    uint32_t layer_index,
    uint8_t codeblock_style,
    QwJp2PacketCodeBlockContribution *contributions,
    size_t contribution_capacity,
    size_t *out_contribution_count,
    uint8_t *out_is_empty,
    size_t *out_header_bytes,
    size_t *out_body_bytes,
    uint32_t *segment_lengths,
    size_t segment_length_capacity,
    size_t *out_segment_length_count);

#endif
