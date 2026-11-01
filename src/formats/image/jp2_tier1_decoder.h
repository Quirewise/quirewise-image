#ifndef QUIREWISE_JP2_TIER1_DECODER_H
#define QUIREWISE_JP2_TIER1_DECODER_H

#include "jp2_mq.h"
#include "jp2_tier1_model.h"

#include <stddef.h>
#include <stdint.h>

typedef struct QwJp2Tier1Coefficient {
    uint64_t magnitude;
    uint8_t negative;
    uint8_t reconstruction_shift;
} QwJp2Tier1Coefficient;

enum {
    QW_JP2_T1_ERR_FORMAT = -2,
    QW_JP2_T1_ERR_MEMORY = -3,
    QW_JP2_T1_ERR_UNSUPPORTED = -4,
    QW_JP2_T1_ERR_DECODE = -5,
    QW_JP2_T1_STYLE_BYPASS = 0x01u,
    QW_JP2_T1_STYLE_RESET = 0x02u,
    QW_JP2_T1_STYLE_TERMALL = 0x04u,
    QW_JP2_T1_STYLE_VSC = 0x08u,
    QW_JP2_T1_STYLE_PTERM = 0x10u,
    QW_JP2_T1_STYLE_SEGSYM = 0x20u,
    QW_JP2_T1_NO_RECONSTRUCTION_MIDPOINT = 63u,
    QW_JP2_T1_MAX_CODEBLOCK_DIMENSION = 1024u
};

/* Decodes one regular (no optional code-block style flags) MQ segment.
 * The caller supplies the bit-plane count and number of passes from packet
 * headers and initializes the MQ decoder over the segment bytes. */
int qw_jp2_t1_decode_mq_codeblock(
    QwJp2MqDecoder *mq,
    uint32_t width,
    uint32_t height,
    uint8_t bit_planes,
    uint16_t pass_count,
    QwJp2Tier1Orientation orientation,
    uint8_t codeblock_style,
    QwJp2Tier1Coefficient *coefficients,
    size_t coefficient_count);

int qw_jp2_t1_decode_mq_codeblock_segments(
    const uint8_t *data,
    size_t data_size,
    const uint32_t *segment_lengths,
    size_t segment_count,
    uint32_t width,
    uint32_t height,
    uint8_t bit_planes,
    QwJp2Tier1Orientation orientation,
    uint8_t codeblock_style,
    QwJp2Tier1Coefficient *coefficients,
    size_t coefficient_count);

#endif
