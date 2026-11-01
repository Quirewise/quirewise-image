#include "jp2_tier1_decoder.h"

#include <stdlib.h>
#include <string.h>

enum {
    QW_JP2_T1_FLAG_SIGNIFICANT = 1u << 0,
    QW_JP2_T1_FLAG_NEGATIVE = 1u << 1,
    QW_JP2_T1_FLAG_NEW = 1u << 2,
    QW_JP2_T1_FLAG_REFINED = 1u << 3,
    QW_JP2_T1_FLAG_VISITED = 1u << 4
};

typedef struct QwJp2Tier1Block {
    QwJp2MqDecoder *mq;
    QwJp2Tier1Coefficient *coefficients;
    uint8_t *flags;
    uint32_t width;
    uint32_t height;
    uint8_t bit_plane;
    uint8_t segmentation_symbol;
    QwJp2Tier1Orientation orientation;
} QwJp2Tier1Block;

static uint32_t qw_jp2_t1_index(const QwJp2Tier1Block *block,
                                uint32_t x,
                                uint32_t y)
{
    return y * block->width + x;
}

static int8_t qw_jp2_t1_neighbor_value(const QwJp2Tier1Block *block,
                                       int32_t x,
                                       int32_t y)
{
    uint32_t index;

    if (x < 0 || y < 0 || (uint32_t)x >= block->width ||
        (uint32_t)y >= block->height) {
        return 0;
    }
    index = qw_jp2_t1_index(block, (uint32_t)x, (uint32_t)y);
    if ((block->flags[index] & QW_JP2_T1_FLAG_SIGNIFICANT) == 0u) return 0;
    return (block->flags[index] & QW_JP2_T1_FLAG_NEGATIVE) ? -1 : 1;
}

static void qw_jp2_t1_get_neighbors(const QwJp2Tier1Block *block,
                                    uint32_t x,
                                    uint32_t y,
                                    QwJp2Tier1Neighbors *neighbors)
{
    neighbors->west = qw_jp2_t1_neighbor_value(block,
                                               (int32_t)x - 1,
                                               (int32_t)y);
    neighbors->east = qw_jp2_t1_neighbor_value(block,
                                               (int32_t)x + 1,
                                               (int32_t)y);
    neighbors->north = qw_jp2_t1_neighbor_value(block,
                                                (int32_t)x,
                                                (int32_t)y - 1);
    neighbors->south = qw_jp2_t1_neighbor_value(block,
                                                (int32_t)x,
                                                (int32_t)y + 1);
    neighbors->northwest = qw_jp2_t1_neighbor_value(block,
                                                    (int32_t)x - 1,
                                                    (int32_t)y - 1);
    neighbors->northeast = qw_jp2_t1_neighbor_value(block,
                                                    (int32_t)x + 1,
                                                    (int32_t)y - 1);
    neighbors->southwest = qw_jp2_t1_neighbor_value(block,
                                                    (int32_t)x - 1,
                                                    (int32_t)y + 1);
    neighbors->southeast = qw_jp2_t1_neighbor_value(block,
                                                    (int32_t)x + 1,
                                                    (int32_t)y + 1);
}

static int qw_jp2_t1_decode_symbol(QwJp2Tier1Block *block,
                                  uint8_t context,
                                  uint8_t *symbol)
{
    int rc = qw_jp2_mq_decode(block->mq, context, symbol);
    return rc == QW_JP2_MQ_OK ? QW_JP2_T1_OK : QW_JP2_T1_ERR_DECODE;
}

static int qw_jp2_t1_decode_sign(QwJp2Tier1Block *block,
                                 uint32_t x,
                                 uint32_t y)
{
    QwJp2Tier1Neighbors neighbors;
    uint8_t context;
    uint8_t xor_bit;
    uint8_t symbol;
    uint32_t index = qw_jp2_t1_index(block, x, y);

    qw_jp2_t1_get_neighbors(block, x, y, &neighbors);
    if (qw_jp2_t1_sign_context(&neighbors, &context, &xor_bit) !=
        QW_JP2_T1_OK) {
        return QW_JP2_T1_ERR_DECODE;
    }
    if (qw_jp2_t1_decode_symbol(block, context, &symbol) != QW_JP2_T1_OK) {
        return QW_JP2_T1_ERR_DECODE;
    }
    if ((symbol ^ xor_bit) != 0u) {
        block->flags[index] |= QW_JP2_T1_FLAG_NEGATIVE;
        block->coefficients[index].negative = 1u;
    }
    return QW_JP2_T1_OK;
}

static int qw_jp2_t1_mark_significant(QwJp2Tier1Block *block,
                                      uint32_t x,
                                      uint32_t y)
{
    uint32_t index = qw_jp2_t1_index(block, x, y);

    block->flags[index] |= QW_JP2_T1_FLAG_SIGNIFICANT |
                           QW_JP2_T1_FLAG_NEW;
    block->coefficients[index].magnitude |= UINT64_C(1) << block->bit_plane;
    block->coefficients[index].reconstruction_shift = block->bit_plane != 0u
        ? block->bit_plane : QW_JP2_T1_NO_RECONSTRUCTION_MIDPOINT;
    return qw_jp2_t1_decode_sign(block, x, y);
}

static int qw_jp2_t1_decode_significance_bit(QwJp2Tier1Block *block,
                                              uint32_t x,
                                              uint32_t y,
                                              int skip_zero_context)
{
    QwJp2Tier1Neighbors neighbors;
    uint8_t context;
    uint8_t symbol;
    uint32_t index = qw_jp2_t1_index(block, x, y);

    if (block->flags[index] & QW_JP2_T1_FLAG_SIGNIFICANT) {
        return QW_JP2_T1_OK;
    }
    qw_jp2_t1_get_neighbors(block, x, y, &neighbors);
    if (qw_jp2_t1_significance_context(&neighbors, block->orientation,
                                      &context) != QW_JP2_T1_OK) {
        return QW_JP2_T1_ERR_DECODE;
    }
    if (skip_zero_context && context == 0u) return QW_JP2_T1_OK;
    if (skip_zero_context) block->flags[index] |= QW_JP2_T1_FLAG_VISITED;
    if (qw_jp2_t1_decode_symbol(block, context, &symbol) != QW_JP2_T1_OK) {
        return QW_JP2_T1_ERR_DECODE;
    }
    if (symbol) return qw_jp2_t1_mark_significant(block, x, y);
    return QW_JP2_T1_OK;
}

static void qw_jp2_t1_scan_pass(QwJp2Tier1Block *block,
                                int (*visit)(QwJp2Tier1Block *, uint32_t,
                                             uint32_t),
                                int *status)
{
    for (uint32_t stripe_y = 0u; stripe_y < block->height &&
                                  *status == QW_JP2_T1_OK;
         stripe_y += 4u) {
        uint32_t rows = block->height - stripe_y;
        if (rows > 4u) rows = 4u;
        for (uint32_t x = 0u; x < block->width &&
                               *status == QW_JP2_T1_OK;
             ++x) {
            for (uint32_t dy = 0u; dy < rows; ++dy) {
                *status = visit(block, x, stripe_y + dy);
                if (*status != QW_JP2_T1_OK) break;
            }
        }
    }
}

static int qw_jp2_t1_visit_significance(QwJp2Tier1Block *block,
                                        uint32_t x,
                                        uint32_t y)
{
    return qw_jp2_t1_decode_significance_bit(block, x, y, 1);
}

static int qw_jp2_t1_visit_refinement(QwJp2Tier1Block *block,
                                      uint32_t x,
                                      uint32_t y)
{
    uint32_t index = qw_jp2_t1_index(block, x, y);
    QwJp2Tier1Neighbors neighbors;
    uint8_t context;
    uint8_t symbol;

    if ((block->flags[index] & QW_JP2_T1_FLAG_SIGNIFICANT) == 0u ||
        (block->flags[index] & QW_JP2_T1_FLAG_NEW) != 0u) {
        return QW_JP2_T1_OK;
    }
    qw_jp2_t1_get_neighbors(block, x, y, &neighbors);
    if (qw_jp2_t1_magnitude_refinement_context(
            &neighbors,
            (block->flags[index] & QW_JP2_T1_FLAG_REFINED) == 0u,
            &context) != QW_JP2_T1_OK ||
        qw_jp2_t1_decode_symbol(block, context, &symbol) != QW_JP2_T1_OK) {
        return QW_JP2_T1_ERR_DECODE;
    }
    if (symbol) {
        block->coefficients[index].magnitude |=
            UINT64_C(1) << block->bit_plane;
    }
    block->coefficients[index].reconstruction_shift = block->bit_plane != 0u
        ? block->bit_plane : QW_JP2_T1_NO_RECONSTRUCTION_MIDPOINT;
    block->flags[index] |= QW_JP2_T1_FLAG_REFINED;
    return QW_JP2_T1_OK;
}

static int qw_jp2_t1_cleanup_run_is_eligible(QwJp2Tier1Block *block,
                                              uint32_t x,
                                              uint32_t stripe_y)
{
    if (block->height - stripe_y < 4u) return 0;
    for (uint32_t dy = 0u; dy < 4u; ++dy) {
        uint32_t index = qw_jp2_t1_index(block, x, stripe_y + dy);
        QwJp2Tier1Neighbors neighbors;
        uint8_t context;
        if (block->flags[index] & (QW_JP2_T1_FLAG_SIGNIFICANT |
                                   QW_JP2_T1_FLAG_VISITED)) {
            return 0;
        }
        qw_jp2_t1_get_neighbors(block, x, stripe_y + dy, &neighbors);
        if (qw_jp2_t1_significance_context(&neighbors, block->orientation,
                                           &context) != QW_JP2_T1_OK ||
            context != 0u) {
            return 0;
        }
    }
    return 1;
}

static int qw_jp2_t1_decode_uniform_bit(QwJp2Tier1Block *block,
                                        uint8_t *symbol)
{
    return qw_jp2_t1_decode_symbol(block, 18u, symbol);
}

static int qw_jp2_t1_decode_segmentation_symbol(QwJp2Tier1Block *block)
{
    static const uint8_t expected_bits[4] = {1u, 0u, 1u, 0u};
    for (size_t i = 0u;
         i < sizeof(expected_bits) / sizeof(expected_bits[0]); ++i) {
        uint8_t symbol;
        if (qw_jp2_t1_decode_uniform_bit(block, &symbol) != QW_JP2_T1_OK) {
            return QW_JP2_T1_ERR_DECODE;
        }
        if (symbol != expected_bits[i]) {
            return QW_JP2_T1_ERR_DECODE;
        }
    }
    return QW_JP2_T1_OK;
}

static int qw_jp2_t1_decode_cleanup(QwJp2Tier1Block *block)
{
    for (uint32_t stripe_y = 0u; stripe_y < block->height; stripe_y += 4u) {
        uint32_t rows = block->height - stripe_y;
        if (rows > 4u) rows = 4u;
        for (uint32_t x = 0u; x < block->width; ++x) {
            if (qw_jp2_t1_cleanup_run_is_eligible(block, x, stripe_y)) {
                uint8_t run;
                if (qw_jp2_t1_decode_symbol(block, 17u, &run) !=
                    QW_JP2_T1_OK) {
                    return QW_JP2_T1_ERR_DECODE;
                }
                if (!run) continue;
                uint8_t first_msb;
                uint8_t first_lsb;
                if (qw_jp2_t1_decode_uniform_bit(block, &first_msb) !=
                        QW_JP2_T1_OK ||
                    qw_jp2_t1_decode_uniform_bit(block, &first_lsb) !=
                        QW_JP2_T1_OK) {
                    return QW_JP2_T1_ERR_DECODE;
                }
                uint32_t first = ((uint32_t)first_msb << 1) | first_lsb;
                if (qw_jp2_t1_mark_significant(block, x,
                                               stripe_y + first) !=
                    QW_JP2_T1_OK) {
                    return QW_JP2_T1_ERR_DECODE;
                }
                for (uint32_t dy = first + 1u; dy < 4u; ++dy) {
                    if (qw_jp2_t1_decode_significance_bit(
                            block, x, stripe_y + dy, 0) != QW_JP2_T1_OK) {
                        return QW_JP2_T1_ERR_DECODE;
                    }
                }
                continue;
            }
            for (uint32_t dy = 0u; dy < rows; ++dy) {
                uint32_t index = qw_jp2_t1_index(block, x, stripe_y + dy);
                if (block->flags[index] & QW_JP2_T1_FLAG_VISITED) continue;
                if (qw_jp2_t1_decode_significance_bit(
                        block, x, stripe_y + dy, 0) != QW_JP2_T1_OK) {
                    return QW_JP2_T1_ERR_DECODE;
                }
            }
        }
    }
    return QW_JP2_T1_OK;
}

static uint16_t qw_jp2_t1_max_passes(uint8_t bit_planes)
{
    return bit_planes ? (uint16_t)(3u * (uint16_t)bit_planes - 2u) : 0u;
}

static int qw_jp2_t1_decode_pass(QwJp2Tier1Block *block,
                                uint16_t pass_index,
                                uint8_t codeblock_style)
{
    int rc = QW_JP2_T1_OK;
    if (pass_index == 0u || pass_index % 3u == 0u) {
        rc = qw_jp2_t1_decode_cleanup(block);
        if (rc == QW_JP2_T1_OK && (codeblock_style & 0x20u) != 0u) {
            rc = qw_jp2_t1_decode_segmentation_symbol(block);
        }
        return rc;
    }
    if (pass_index % 3u == 1u) {
        if (block->bit_plane == 0u) return QW_JP2_T1_ERR_FORMAT;
        --block->bit_plane;
        for (size_t i = 0u; i < (size_t)block->width * block->height; ++i) {
            block->flags[i] &= (uint8_t)~(QW_JP2_T1_FLAG_NEW |
                                         QW_JP2_T1_FLAG_VISITED);
        }
        qw_jp2_t1_scan_pass(block, qw_jp2_t1_visit_significance, &rc);
        return rc;
    }
    if (pass_index % 3u == 2u) {
        qw_jp2_t1_scan_pass(block, qw_jp2_t1_visit_refinement, &rc);
        return rc;
    }
    return QW_JP2_T1_ERR_FORMAT;
}

static int qw_jp2_t1_decode_mq_codeblock_impl(
    QwJp2MqDecoder *mq,
    const uint8_t *data,
    size_t data_size,
    const uint32_t *segment_lengths,
    size_t segment_count,
    int segmented,
    uint32_t width,
    uint32_t height,
    uint8_t bit_planes,
    uint16_t pass_count,
    QwJp2Tier1Orientation orientation,
    uint8_t codeblock_style,
    QwJp2Tier1Coefficient *coefficients,
    size_t coefficient_count)
{
    QwJp2Tier1Coefficient *decoded = NULL;
    uint8_t *flags = NULL;
    size_t count;
    uint16_t max_passes;
    int rc = QW_JP2_T1_OK;
    QwJp2Tier1Block block;

    if (!mq || !coefficients || width == 0u || height == 0u ||
        width > QW_JP2_T1_MAX_CODEBLOCK_DIMENSION ||
        height > QW_JP2_T1_MAX_CODEBLOCK_DIMENSION ||
        orientation < QW_JP2_T1_ORIENTATION_LL_LH ||
        orientation > QW_JP2_T1_ORIENTATION_HH || bit_planes > 63u) {
        return QW_JP2_T1_ERR_ARGUMENT;
    }
    if ((codeblock_style & (uint8_t)~0x36u) != 0u) {
        return QW_JP2_T1_ERR_UNSUPPORTED;
    }
    if (((codeblock_style & 0x04u) != 0u) != (segmented != 0)) {
        return QW_JP2_T1_ERR_UNSUPPORTED;
    }
    count = (size_t)width * (size_t)height;
    if (coefficient_count != count) return QW_JP2_T1_ERR_ARGUMENT;
    max_passes = qw_jp2_t1_max_passes(bit_planes);
    if ((bit_planes == 0u && pass_count != 0u) ||
        (bit_planes != 0u && pass_count > max_passes)) {
        return QW_JP2_T1_ERR_FORMAT;
    }
    if (segmented &&
        (segment_count != (size_t)pass_count ||
         (data_size != 0u && !data) ||
         (segment_count != 0u && !segment_lengths) ||
         (segment_count == 0u && data_size != 0u))) {
        return QW_JP2_T1_ERR_FORMAT;
    }
    if (pass_count == 0u) {
        memset(coefficients, 0, count * sizeof(*coefficients));
        return QW_JP2_T1_OK;
    }

    decoded = (QwJp2Tier1Coefficient *)calloc(count, sizeof(*decoded));
    flags = (uint8_t *)calloc(count, sizeof(*flags));
    if (!decoded || !flags) {
        rc = QW_JP2_T1_ERR_MEMORY;
        goto cleanup;
    }
    block.mq = mq;
    block.coefficients = decoded;
    block.flags = flags;
    block.width = width;
    block.height = height;
    block.orientation = orientation;
    block.bit_plane = (uint8_t)(bit_planes - 1u);
    block.segmentation_symbol = (uint8_t)((codeblock_style & 0x20u) != 0u);

    if (segmented) {
        size_t data_offset = 0u;
        for (uint16_t pass = 0u; pass < pass_count; ++pass) {
            size_t pass_size = segment_lengths[pass];
            const uint8_t *pass_data =
                data ? data + data_offset : NULL;
            if (pass_size > data_size - data_offset) {
                rc = QW_JP2_T1_ERR_FORMAT;
                goto cleanup;
            }
            if (pass_size != 0u && (codeblock_style & 0x10u) != 0u &&
                pass_data[pass_size - 1u] == 0xffu) {
                rc = QW_JP2_T1_ERR_FORMAT;
                goto cleanup;
            }
            if (qw_jp2_mq_init_segment(
                    mq, pass_data, pass_size,
                    pass != 0u && (codeblock_style & 0x02u) == 0u) !=
                QW_JP2_MQ_OK) {
                rc = QW_JP2_T1_ERR_DECODE;
                goto cleanup;
            }
            block.mq = mq;
            rc = qw_jp2_t1_decode_pass(&block, pass, codeblock_style);
            if (rc != QW_JP2_T1_OK) {
                goto cleanup;
            }
            data_offset += pass_size;
        }
        if (data_offset != data_size) {
            rc = QW_JP2_T1_ERR_FORMAT;
            goto cleanup;
        }
    } else {
        if ((codeblock_style & 0x10u) != 0u && mq->size != 0u &&
            mq->data[mq->size - 1u] == 0xffu) {
            rc = QW_JP2_T1_ERR_FORMAT;
            goto cleanup;
        }
        for (uint16_t pass = 0u; pass < pass_count; ++pass) {
            rc = qw_jp2_t1_decode_pass(&block, pass, codeblock_style);
            if (rc != QW_JP2_T1_OK) goto cleanup;
            if ((codeblock_style & 0x02u) != 0u &&
                pass + 1u < pass_count) {
                qw_jp2_mq_reset_contexts(mq);
            }
        }
    }

    memcpy(coefficients, decoded, count * sizeof(*coefficients));

cleanup:
    free(flags);
    free(decoded);
    return rc;
}

int qw_jp2_t1_decode_mq_codeblock(
    QwJp2MqDecoder *mq,
    uint32_t width,
    uint32_t height,
    uint8_t bit_planes,
    uint16_t pass_count,
    QwJp2Tier1Orientation orientation,
    uint8_t codeblock_style,
    QwJp2Tier1Coefficient *coefficients,
    size_t coefficient_count)
{
    if ((codeblock_style & 0x04u) != 0u) return QW_JP2_T1_ERR_UNSUPPORTED;
    return qw_jp2_t1_decode_mq_codeblock_impl(
        mq, NULL, 0u, NULL, 0u, 0, width, height, bit_planes, pass_count,
        orientation, codeblock_style, coefficients, coefficient_count);
}

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
    size_t coefficient_count)
{
    QwJp2MqDecoder mq = {0};
    if ((codeblock_style & 0x04u) == 0u ||
        segment_count > UINT16_MAX) {
        return QW_JP2_T1_ERR_UNSUPPORTED;
    }
    return qw_jp2_t1_decode_mq_codeblock_impl(
        &mq, data, data_size, segment_lengths, segment_count, 1,
        width, height, bit_planes, (uint16_t)segment_count, orientation,
        codeblock_style, coefficients, coefficient_count);
}

