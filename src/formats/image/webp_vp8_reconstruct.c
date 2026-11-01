#include "image_internal.h"
#include "webp_vp8_quant.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct QwWebPVP8Reconstruction {
    const QwWebPVP8Header *header;
    uint8_t *y_plane;
    uint8_t *u_plane;
    uint8_t *v_plane;
    size_t macroblock_columns;
    size_t macroblock_rows;
    size_t y_stride;
    size_t uv_stride;
    uint8_t *macroblock_has_coefficients;
} QwWebPVP8Reconstruction;

static uint8_t webp_vp8_clip_byte(int value)
{
    if (value < 0) return 0u;
    if (value > 255) return 255u;
    return (uint8_t)value;
}

static int webp_vp8_clip_quantizer(int value)
{
    if (value < 0) return 0;
    if (value > 127) return 127;
    return value;
}

static int webp_vp8_store_short(int value, int16_t *out_value)
{
    if (!out_value || value < INT16_MIN || value > INT16_MAX) return 0;
    *out_value = (int16_t)value;
    return 1;
}

static int webp_vp8_inverse_wht(const int16_t input[16], int16_t output[16])
{
    int16_t intermediate[16];

    for (size_t column = 0u; column < 4u; ++column) {
        int a = input[column] + input[12u + column];
        int b = input[4u + column] + input[8u + column];
        int c = input[4u + column] - input[8u + column];
        int d = input[column] - input[12u + column];
        if (!webp_vp8_store_short(a + b, &intermediate[column]) ||
            !webp_vp8_store_short(c + d, &intermediate[4u + column]) ||
            !webp_vp8_store_short(a - b, &intermediate[8u + column]) ||
            !webp_vp8_store_short(d - c, &intermediate[12u + column])) {
            return 0;
        }
    }

    for (size_t row = 0u; row < 4u; ++row) {
        const int16_t *src = intermediate + row * 4u;
        int a = src[0] + src[3];
        int b = src[1] + src[2];
        int c = src[1] - src[2];
        int d = src[0] - src[3];
        int values[4] = {(a + b + 3) >> 3, (c + d + 3) >> 3,
                         (a - b + 3) >> 3, (d - c + 3) >> 3};
        for (size_t column = 0u; column < 4u; ++column) {
            if (!webp_vp8_store_short(values[column],
                                      &output[row * 4u + column])) {
                return 0;
            }
        }
    }
    return 1;
}

static int webp_vp8_inverse_dct4x4(const int16_t input[16],
                                  int16_t output[16])
{
    static const int cospi8sqrt2minus1 = 20091;
    static const int sinpi8sqrt2 = 35468;
    int16_t intermediate[16];

    for (size_t column = 0u; column < 4u; ++column) {
        int a = input[column] + input[8u + column];
        int b = input[column] - input[8u + column];
        int temp1 = (input[4u + column] * sinpi8sqrt2) >> 16;
        int temp2 = input[12u + column] +
            ((input[12u + column] * cospi8sqrt2minus1) >> 16);
        int c = temp1 - temp2;
        temp1 = input[4u + column] +
            ((input[4u + column] * cospi8sqrt2minus1) >> 16);
        temp2 = (input[12u + column] * sinpi8sqrt2) >> 16;
        int d = temp1 + temp2;
        if (!webp_vp8_store_short(a + d, &intermediate[column]) ||
            !webp_vp8_store_short(a - d, &intermediate[12u + column]) ||
            !webp_vp8_store_short(b + c, &intermediate[4u + column]) ||
            !webp_vp8_store_short(b - c, &intermediate[8u + column])) {
            return 0;
        }
    }

    for (size_t row = 0u; row < 4u; ++row) {
        const int16_t *src = intermediate + row * 4u;
        int a = src[0] + src[2];
        int b = src[0] - src[2];
        int temp1 = (src[1] * sinpi8sqrt2) >> 16;
        int temp2 = src[3] + ((src[3] * cospi8sqrt2minus1) >> 16);
        int c = temp1 - temp2;
        temp1 = src[1] + ((src[1] * cospi8sqrt2minus1) >> 16);
        temp2 = (src[3] * sinpi8sqrt2) >> 16;
        int d = temp1 + temp2;
        int values[4] = {(a + d + 4) >> 3, (b + c + 4) >> 3,
                         (b - c + 4) >> 3, (a - d + 4) >> 3};
        for (size_t column = 0u; column < 4u; ++column) {
            if (!webp_vp8_store_short(values[column],
                                      &output[row * 4u + column])) {
                return 0;
            }
        }
    }
    return 1;
}

static int webp_vp8_dequantize_coefficient(int16_t coefficient,
                                           int factor,
                                           int16_t *out_value)
{
    int value = (int)coefficient * factor;
    return webp_vp8_store_short(value, out_value);
}

static int webp_vp8_decode_residuals(
    const QwWebPVP8Header *header,
    const QwWebPVP8Macroblock *macroblock,
    const int16_t coefficients[25][16],
    int16_t y_residual[16][16],
    int16_t u_residual[8][8],
    int16_t v_residual[8][8])
{
    int quantizer;
    int y_dc_factor;
    int y_ac_factor;
    int uv_dc_factor;
    int uv_ac_factor;
    int y2_dc_factor;
    int y2_ac_factor;
    int16_t y2_coefficients[16] = {0};
    int16_t y2_dc[16] = {0};

    if (!header || !macroblock || !coefficients || !y_residual ||
        !u_residual || !v_residual || macroblock->segment_id > 3u) {
        return 0;
    }
    quantizer = header->base_quantizer;
    if (header->segmentation_enabled) {
        if (header->segmentation_feature_mode) {
            quantizer = header->segment_quantizer_delta[macroblock->segment_id];
        } else {
            quantizer += header->segment_quantizer_delta[macroblock->segment_id];
        }
    }
    quantizer = webp_vp8_clip_quantizer(quantizer);
    y_dc_factor = webp_vp8_dc_qlookup[
        webp_vp8_clip_quantizer(quantizer + header->quantizer_delta[0])];
    y_ac_factor = webp_vp8_ac_qlookup[quantizer];
    uv_dc_factor = webp_vp8_dc_qlookup[
        webp_vp8_clip_quantizer(quantizer + header->quantizer_delta[3])];
    if (uv_dc_factor > 132) uv_dc_factor = 132;
    uv_ac_factor = webp_vp8_ac_qlookup[
        webp_vp8_clip_quantizer(quantizer + header->quantizer_delta[4])];
    y2_dc_factor = 2 * webp_vp8_dc_qlookup[
        webp_vp8_clip_quantizer(quantizer + header->quantizer_delta[1])];
    y2_ac_factor = webp_vp8_ac_qlookup[
        webp_vp8_clip_quantizer(quantizer + header->quantizer_delta[2])] * 155 / 100;
    if (y2_ac_factor < 8) y2_ac_factor = 8;

    if (macroblock->y_mode != 4u) {
        for (size_t coefficient = 0u; coefficient < 16u; ++coefficient) {
            int factor = coefficient == 0u ? y2_dc_factor : y2_ac_factor;
            if (!webp_vp8_dequantize_coefficient(
                    coefficients[24][coefficient], factor,
                    &y2_coefficients[coefficient])) {
                return 0;
            }
        }
        if (!webp_vp8_inverse_wht(y2_coefficients, y2_dc)) return 0;
    }

    memset(y_residual, 0, 16u * 16u * sizeof(y_residual[0][0]));
    memset(u_residual, 0, 8u * 8u * sizeof(u_residual[0][0]));
    memset(v_residual, 0, 8u * 8u * sizeof(v_residual[0][0]));
    for (size_t block = 0u; block < 16u; ++block) {
        int16_t dequantized[16] = {0};
        int16_t transformed[16];
        size_t block_x = block % 4u;
        size_t block_y = block / 4u;
        for (size_t coefficient = 0u; coefficient < 16u; ++coefficient) {
            if (coefficient == 0u && macroblock->y_mode != 4u) {
                dequantized[0] = y2_dc[block];
            } else {
                int factor = coefficient == 0u ? y_dc_factor : y_ac_factor;
                if (!webp_vp8_dequantize_coefficient(
                        coefficients[block][coefficient], factor,
                        &dequantized[coefficient])) {
                    return 0;
                }
            }
        }
        if (!webp_vp8_inverse_dct4x4(dequantized, transformed)) return 0;
        for (size_t y = 0u; y < 4u; ++y) {
            memcpy(&y_residual[block_y * 4u + y][block_x * 4u],
                   transformed + y * 4u, 4u * sizeof(transformed[0]));
        }
    }

    for (size_t block = 16u; block < 24u; ++block) {
        int16_t dequantized[16];
        int16_t transformed[16];
        size_t chroma_block = block < 20u ? block - 16u : block - 20u;
        size_t block_x = chroma_block % 2u;
        size_t block_y = chroma_block / 2u;
        int16_t (*residual)[8] = block < 20u ? u_residual : v_residual;
        for (size_t coefficient = 0u; coefficient < 16u; ++coefficient) {
            int factor = coefficient == 0u ? uv_dc_factor : uv_ac_factor;
            if (!webp_vp8_dequantize_coefficient(
                    coefficients[block][coefficient], factor,
                    &dequantized[coefficient])) {
                return 0;
            }
        }
        if (!webp_vp8_inverse_dct4x4(dequantized, transformed)) return 0;
        for (size_t y = 0u; y < 4u; ++y) {
            memcpy(&residual[block_y * 4u + y][block_x * 4u],
                   transformed + y * 4u, 4u * sizeof(transformed[0]));
        }
    }
    return 1;
}

static uint8_t webp_vp8_average2(uint8_t a, uint8_t b)
{
    return (uint8_t)(((unsigned)a + b + 1u) >> 1u);
}

static uint8_t webp_vp8_average3(uint8_t a, uint8_t b, uint8_t c)
{
    return (uint8_t)(((unsigned)a + 2u * b + c + 2u) >> 2u);
}

static void webp_vp8_predict_b4(uint8_t mode,
                               const uint8_t above[8],
                               const uint8_t left[4],
                               uint8_t top_left,
                               uint8_t output[16])
{
    uint8_t edge[9] = {left[3], left[2], left[1], left[0], top_left,
                       above[0], above[1], above[2], above[3]};
    uint8_t value;

    switch (mode) {
    case 0u: {
        unsigned sum = 4u;
        for (size_t i = 0u; i < 4u; ++i) sum += above[i] + left[i];
        value = (uint8_t)(sum >> 3u);
        for (size_t i = 0u; i < 16u; ++i) output[i] = value;
        break;
    }
    case 1u:
        for (size_t y = 0u; y < 4u; ++y) {
            for (size_t x = 0u; x < 4u; ++x) {
                output[y * 4u + x] = webp_vp8_clip_byte(
                    (int)left[y] + above[x] - top_left);
            }
        }
        break;
    case 2u:
        for (size_t x = 0u; x < 4u; ++x) {
            uint8_t a = x == 0u ? top_left : above[x - 1u];
            value = webp_vp8_average3(a, above[x], above[x + 1u]);
            for (size_t y = 0u; y < 4u; ++y) output[y * 4u + x] = value;
        }
        break;
    case 3u:
        for (size_t y = 0u; y < 4u; ++y) {
            uint8_t a = y == 0u ? top_left : left[y - 1u];
            uint8_t b = left[y];
            uint8_t c = y == 3u ? left[3] : left[y + 1u];
            value = webp_vp8_average3(a, b, c);
            for (size_t x = 0u; x < 4u; ++x) output[y * 4u + x] = value;
        }
        break;
    case 5u: /* B_RD_PRED */
        output[12] = webp_vp8_average3(edge[0], edge[1], edge[2]);
        output[13] = output[8] = webp_vp8_average3(edge[1], edge[2], edge[3]);
        output[14] = output[9] = output[4] =
            webp_vp8_average3(edge[2], edge[3], edge[4]);
        output[15] = output[10] = output[5] = output[0] =
            webp_vp8_average3(edge[3], edge[4], edge[5]);
        output[11] = output[6] = output[1] =
            webp_vp8_average3(edge[4], edge[5], edge[6]);
        output[7] = output[2] = webp_vp8_average3(edge[5], edge[6], edge[7]);
        output[3] = webp_vp8_average3(edge[6], edge[7], edge[8]);
        break;
    case 6u: /* B_VR_PRED */
        output[12] = webp_vp8_average3(edge[1], edge[2], edge[3]);
        output[8] = webp_vp8_average3(edge[2], edge[3], edge[4]);
        output[13] = output[4] = webp_vp8_average3(edge[3], edge[4], edge[5]);
        output[9] = output[0] = webp_vp8_average2(edge[4], edge[5]);
        output[14] = output[5] = webp_vp8_average3(edge[4], edge[5], edge[6]);
        output[10] = output[1] = webp_vp8_average2(edge[5], edge[6]);
        output[15] = output[6] = webp_vp8_average3(edge[5], edge[6], edge[7]);
        output[11] = output[2] = webp_vp8_average2(edge[6], edge[7]);
        output[7] = webp_vp8_average3(edge[6], edge[7], edge[8]);
        output[3] = webp_vp8_average2(edge[7], edge[8]);
        break;
    case 4u: /* B_LD_PRED */
        output[0] = webp_vp8_average3(above[0], above[1], above[2]);
        output[1] = output[4] = webp_vp8_average3(above[1], above[2], above[3]);
        output[2] = output[5] = output[8] =
            webp_vp8_average3(above[2], above[3], above[4]);
        output[3] = output[6] = output[9] = output[12] =
            webp_vp8_average3(above[3], above[4], above[5]);
        output[7] = output[10] = output[13] =
            webp_vp8_average3(above[4], above[5], above[6]);
        output[11] = output[14] = webp_vp8_average3(above[5], above[6], above[7]);
        output[15] = webp_vp8_average3(above[6], above[7], above[7]);
        break;
    case 7u:
        output[0] = webp_vp8_average2(above[0], above[1]);
        output[4] = webp_vp8_average3(above[0], above[1], above[2]);
        output[8] = output[1] = webp_vp8_average2(above[1], above[2]);
        output[5] = output[12] =
            webp_vp8_average3(above[1], above[2], above[3]);
        output[9] = output[2] = webp_vp8_average2(above[2], above[3]);
        output[13] = output[6] =
            webp_vp8_average3(above[2], above[3], above[4]);
        output[10] = output[3] = webp_vp8_average2(above[3], above[4]);
        output[14] = output[7] =
            webp_vp8_average3(above[3], above[4], above[5]);
        output[11] = webp_vp8_average3(above[4], above[5], above[6]);
        output[15] = webp_vp8_average3(above[5], above[6], above[7]);
        break;
    case 8u:
        output[12] = webp_vp8_average2(edge[0], edge[1]);
        output[13] = webp_vp8_average3(edge[0], edge[1], edge[2]);
        output[8] = output[14] = webp_vp8_average2(edge[1], edge[2]);
        output[9] = output[15] = webp_vp8_average3(edge[1], edge[2], edge[3]);
        output[10] = output[4] = webp_vp8_average2(edge[2], edge[3]);
        output[11] = output[5] = webp_vp8_average3(edge[2], edge[3], edge[4]);
        output[6] = output[0] = webp_vp8_average2(edge[3], edge[4]);
        output[7] = output[1] = webp_vp8_average3(edge[3], edge[4], edge[5]);
        output[2] = webp_vp8_average3(edge[4], edge[5], edge[6]);
        output[3] = webp_vp8_average3(edge[5], edge[6], edge[7]);
        break;
    case 9u:
        output[0] = webp_vp8_average2(left[0], left[1]);
        output[1] = webp_vp8_average3(left[0], left[1], left[2]);
        output[2] = output[4] = webp_vp8_average2(left[1], left[2]);
        output[3] = output[5] = webp_vp8_average3(left[1], left[2], left[3]);
        output[6] = output[8] = webp_vp8_average2(left[2], left[3]);
        output[7] = output[9] = webp_vp8_average3(left[2], left[3], left[3]);
        value = left[3];
        for (size_t i = 10u; i < 16u; ++i) output[i] = value;
        break;
    default:
        memset(output, 0, 16u);
        break;
    }
}

static uint8_t webp_vp8_top_left(const uint8_t *plane,
                                 size_t stride,
                                 size_t x,
                                 size_t y)
{
    if (y == 0u) return 127u;
    if (x == 0u) return 129u;
    return plane[(y - 1u) * stride + x - 1u];
}

static void webp_vp8_predict_macroblock_plane(
    uint8_t *plane,
    size_t stride,
    size_t x,
    size_t y,
    size_t block_size,
    uint8_t mode,
    const int16_t *residual)
{
    uint8_t above[16];
    uint8_t left[16];
    uint8_t top_left = webp_vp8_top_left(plane, stride, x, y);
    unsigned sum = 0u;
    uint8_t dc = 128u;

    for (size_t i = 0u; i < block_size; ++i) {
        above[i] = y == 0u ? 127u : plane[(y - 1u) * stride + x + i];
        left[i] = x == 0u ? 129u : plane[(y + i) * stride + x - 1u];
    }
    if (mode == 0u) {
        if (y == 0u && x == 0u) {
            dc = 128u;
        } else if (y == 0u) {
            for (size_t i = 0u; i < block_size; ++i) sum += left[i];
            dc = (uint8_t)((sum + (unsigned)(block_size / 2u)) / (unsigned)block_size);
        } else if (x == 0u) {
            for (size_t i = 0u; i < block_size; ++i) sum += above[i];
            dc = (uint8_t)((sum + (unsigned)(block_size / 2u)) / (unsigned)block_size);
        } else {
            for (size_t i = 0u; i < block_size; ++i) sum += above[i] + left[i];
            dc = (uint8_t)((sum + (unsigned)block_size) /
                           (unsigned)(2u * block_size));
        }
    }
    for (size_t row = 0u; row < block_size; ++row) {
        for (size_t column = 0u; column < block_size; ++column) {
            int prediction;
            if (mode == 0u) {
                prediction = dc;
            } else if (mode == 1u) {
                prediction = above[column];
            } else if (mode == 2u) {
                prediction = left[row];
            } else {
                prediction = (int)left[row] + above[column] - top_left;
            }
            plane[(y + row) * stride + x + column] = webp_vp8_clip_byte(
                prediction + residual[row * block_size + column]);
        }
    }
}

int qw_webp_vp8_bpred_above_right(const uint8_t *plane,
                                  size_t stride,
                                  size_t macroblock_columns,
                                  size_t macroblock_x,
                                  size_t macroblock_y,
                                  uint8_t out_samples[4])
{
    size_t macroblock_width;
    size_t macroblock_origin_x;
    size_t source_row;
    size_t source_x;
    size_t source_end_x;
    size_t row_offset;
    int at_right_edge;

    if (!plane || !out_samples || !stride || !macroblock_columns ||
        macroblock_columns > SIZE_MAX / 16u ||
        macroblock_x >= macroblock_columns ||
        stride < macroblock_columns * 16u) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (macroblock_y > SIZE_MAX / 16u ||
        macroblock_x > SIZE_MAX / 16u) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (macroblock_y == 0u) {
        memset(out_samples, 127, 4u);
        return QW_STATUS_OK;
    }

    macroblock_width = macroblock_columns * 16u;
    macroblock_origin_x = macroblock_x * 16u;
    at_right_edge = macroblock_x == macroblock_columns - 1u;
    source_row = macroblock_y * 16u - 1u;
    if (source_row > SIZE_MAX / stride) return QW_STATUS_ERR_RESOURCE_LIMIT;
    row_offset = source_row * stride;
    source_x = macroblock_origin_x + (at_right_edge ? 15u : 16u);
    source_end_x = source_x + (at_right_edge ? 0u : 3u);
    if (source_end_x < source_x || row_offset > SIZE_MAX - source_end_x ||
        source_end_x >= macroblock_width) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    for (size_t i = 0u; i < 4u; ++i) {
        size_t x = at_right_edge ? source_x : source_x + i;
        out_samples[i] = plane[row_offset + x];
    }
    return QW_STATUS_OK;
}

static int webp_vp8_reconstruct_luma_bpred(
    QwWebPVP8Reconstruction *frame,
    size_t macroblock_x,
    size_t macroblock_y,
    const QwWebPVP8Macroblock *macroblock,
    const int16_t residual[16][16])
{
    size_t x0 = macroblock_x * 16u;
    size_t y0 = macroblock_y * 16u;
    uint8_t above_right[4];
    int rc = qw_webp_vp8_bpred_above_right(
        frame->y_plane, frame->y_stride, frame->macroblock_columns,
        macroblock_x, macroblock_y, above_right);
    if (rc != QW_STATUS_OK) return rc;

    for (size_t block_y = 0u; block_y < 4u; ++block_y) {
        for (size_t block_x = 0u; block_x < 4u; ++block_x) {
            size_t block = block_y * 4u + block_x;
            size_t x = x0 + block_x * 4u;
            size_t y = y0 + block_y * 4u;
            uint8_t above[8];
            uint8_t left[4];
            uint8_t prediction[16];
            uint8_t top_left = webp_vp8_top_left(frame->y_plane,
                                                  frame->y_stride, x, y);
            for (size_t i = 0u; i < 8u; ++i) {
                size_t above_x = x + i;
                if (block_x == 3u && i >= 4u) {
                    above[i] = above_right[i - 4u];
                } else if (y == 0u) {
                    above[i] = 127u;
                } else {
                    above[i] = frame->y_plane[(y - 1u) * frame->y_stride + above_x];
                }
            }
            for (size_t i = 0u; i < 4u; ++i) {
                left[i] = x == 0u
                    ? 129u
                    : frame->y_plane[(y + i) * frame->y_stride + x - 1u];
            }
            webp_vp8_predict_b4(macroblock->b_mode[block], above, left,
                                top_left, prediction);
            for (size_t row = 0u; row < 4u; ++row) {
                for (size_t column = 0u; column < 4u; ++column) {
                    frame->y_plane[(y + row) * frame->y_stride + x + column] =
                        webp_vp8_clip_byte((int)prediction[row * 4u + column] +
                            residual[block_y * 4u + row][block_x * 4u + column]);
                }
            }
        }
    }
    return QW_STATUS_OK;
}

static int webp_vp8_reconstruct_macroblock(
    void *context,
    size_t macroblock_x,
    size_t macroblock_y,
    const QwWebPVP8Macroblock *macroblock,
    const int16_t coefficients[25][16])
{
    QwWebPVP8Reconstruction *frame = (QwWebPVP8Reconstruction *)context;
    int16_t y_residual[16][16];
    int16_t u_residual[8][8];
    int16_t v_residual[8][8];
    size_t y_x = macroblock_x * 16u;
    size_t y_y = macroblock_y * 16u;
    size_t uv_x = macroblock_x * 8u;
    size_t uv_y = macroblock_y * 8u;

    if (!frame || !macroblock || !coefficients ||
        macroblock_x >= frame->macroblock_columns ||
        macroblock_y >= frame->macroblock_rows ||
        !frame->macroblock_has_coefficients ||
        !webp_vp8_decode_residuals(frame->header, macroblock, coefficients,
                                  y_residual, u_residual, v_residual)) {
        return QW_STATUS_ERR_FORMAT;
    }

    {
        size_t macroblock_index =
            macroblock_y * frame->macroblock_columns + macroblock_x;
        int has_coefficients = 0;
        for (size_t block = 0u; block < 25u && !has_coefficients; ++block) {
            for (size_t coefficient = 0u; coefficient < 16u; ++coefficient) {
                if (coefficients[block][coefficient] != 0) {
                    has_coefficients = 1;
                    break;
                }
            }
        }
        frame->macroblock_has_coefficients[macroblock_index] =
            (uint8_t)has_coefficients;
    }

    if (macroblock->y_mode == 4u) {
        int rc = webp_vp8_reconstruct_luma_bpred(
            frame, macroblock_x, macroblock_y, macroblock, y_residual);
        if (rc != QW_STATUS_OK) return rc;
    } else {
        webp_vp8_predict_macroblock_plane(frame->y_plane, frame->y_stride,
            y_x, y_y, 16u, macroblock->y_mode, &y_residual[0][0]);
    }
    webp_vp8_predict_macroblock_plane(frame->u_plane, frame->uv_stride,
        uv_x, uv_y, 8u, macroblock->uv_mode, &u_residual[0][0]);
    webp_vp8_predict_macroblock_plane(frame->v_plane, frame->uv_stride,
        uv_x, uv_y, 8u, macroblock->uv_mode, &v_residual[0][0]);
    return QW_STATUS_OK;
}

static int webp_vp8_abs_int(int value)
{
    return value < 0 ? -value : value;
}

static int webp_vp8_clip_signed_byte(int value)
{
    if (value < -128) return -128;
    if (value > 127) return 127;
    return value;
}

static int webp_vp8_clip_filter_delta(int value)
{
    if (value < -16) return -16;
    if (value > 15) return 15;
    return value;
}

static int webp_vp8_arithmetic_shift(int value, unsigned shift)
{
    if (value >= 0) return value >> shift;
    return -(((-value) + (1 << shift) - 1) >> shift);
}

static int webp_vp8_needs_simple_filter(const uint8_t *edge,
                                       size_t step,
                                       int threshold)
{
    ptrdiff_t signed_step = (ptrdiff_t)step;
    int p1 = edge[-2 * signed_step];
    int p0 = edge[-signed_step];
    int q0 = edge[0];
    int q1 = edge[signed_step];
    return 4 * webp_vp8_abs_int(p0 - q0) +
           webp_vp8_abs_int(p1 - q1) <= 2 * threshold + 1;
}

static int webp_vp8_needs_normal_filter(const uint8_t *edge,
                                       size_t step,
                                       int threshold,
                                       int interior_limit)
{
    ptrdiff_t signed_step = (ptrdiff_t)step;
    int p3 = edge[-4 * signed_step];
    int p2 = edge[-3 * signed_step];
    int p1 = edge[-2 * signed_step];
    int p0 = edge[-signed_step];
    int q0 = edge[0];
    int q1 = edge[signed_step];
    int q2 = edge[2 * signed_step];
    int q3 = edge[3 * signed_step];
    if (4 * webp_vp8_abs_int(p0 - q0) +
        webp_vp8_abs_int(p1 - q1) > 2 * threshold + 1) {
        return 0;
    }
    return webp_vp8_abs_int(p3 - p2) <= interior_limit &&
           webp_vp8_abs_int(p2 - p1) <= interior_limit &&
           webp_vp8_abs_int(p1 - p0) <= interior_limit &&
           webp_vp8_abs_int(q3 - q2) <= interior_limit &&
           webp_vp8_abs_int(q2 - q1) <= interior_limit &&
           webp_vp8_abs_int(q1 - q0) <= interior_limit;
}

static int webp_vp8_high_edge_variance(const uint8_t *edge,
                                      size_t step,
                                      int threshold)
{
    ptrdiff_t signed_step = (ptrdiff_t)step;
    int p1 = edge[-2 * signed_step];
    int p0 = edge[-signed_step];
    int q0 = edge[0];
    int q1 = edge[signed_step];
    return webp_vp8_abs_int(p1 - p0) > threshold ||
           webp_vp8_abs_int(q1 - q0) > threshold;
}

static void webp_vp8_filter_two(uint8_t *edge, size_t step, int use_outer_taps)
{
    ptrdiff_t signed_step = (ptrdiff_t)step;
    int p1 = edge[-2 * signed_step];
    int p0 = edge[-signed_step];
    int q0 = edge[0];
    int q1 = edge[signed_step];
    int adjustment = 3 * (q0 - p0);
    int adjustment_p;
    int adjustment_q;

    if (use_outer_taps) {
        adjustment += webp_vp8_clip_signed_byte(p1 - q1);
    }
    adjustment = webp_vp8_clip_signed_byte(adjustment);
    adjustment_q = webp_vp8_clip_filter_delta(
        webp_vp8_arithmetic_shift(adjustment + 4, 3u));
    adjustment_p = webp_vp8_clip_filter_delta(
        webp_vp8_arithmetic_shift(adjustment + 3, 3u));
    edge[-signed_step] = webp_vp8_clip_byte(p0 + adjustment_p);
    edge[0] = webp_vp8_clip_byte(q0 - adjustment_q);
}

static void webp_vp8_filter_four(uint8_t *edge, size_t step)
{
    ptrdiff_t signed_step = (ptrdiff_t)step;
    int p1 = edge[-2 * signed_step];
    int p0 = edge[-signed_step];
    int q0 = edge[0];
    int q1 = edge[signed_step];
    int adjustment = 3 * (q0 - p0);
    int adjustment_q = webp_vp8_clip_filter_delta(
        webp_vp8_arithmetic_shift(adjustment + 4, 3u));
    int adjustment_p = webp_vp8_clip_filter_delta(
        webp_vp8_arithmetic_shift(adjustment + 3, 3u));
    int outer_adjustment = webp_vp8_arithmetic_shift(adjustment_q + 1, 1u);

    edge[-2 * signed_step] = webp_vp8_clip_byte(p1 + outer_adjustment);
    edge[-signed_step] = webp_vp8_clip_byte(p0 + adjustment_p);
    edge[0] = webp_vp8_clip_byte(q0 - adjustment_q);
    edge[signed_step] = webp_vp8_clip_byte(q1 - outer_adjustment);
}

static void webp_vp8_filter_six(uint8_t *edge, size_t step)
{
    ptrdiff_t signed_step = (ptrdiff_t)step;
    int p2 = edge[-3 * signed_step];
    int p1 = edge[-2 * signed_step];
    int p0 = edge[-signed_step];
    int q0 = edge[0];
    int q1 = edge[signed_step];
    int q2 = edge[2 * signed_step];
    int adjustment = webp_vp8_clip_signed_byte(
        3 * (q0 - p0) + webp_vp8_clip_signed_byte(p1 - q1));
    int adjustment0 = webp_vp8_arithmetic_shift(27 * adjustment + 63, 7u);
    int adjustment1 = webp_vp8_arithmetic_shift(18 * adjustment + 63, 7u);
    int adjustment2 = webp_vp8_arithmetic_shift(9 * adjustment + 63, 7u);

    edge[-3 * signed_step] = webp_vp8_clip_byte(p2 + adjustment2);
    edge[-2 * signed_step] = webp_vp8_clip_byte(p1 + adjustment1);
    edge[-signed_step] = webp_vp8_clip_byte(p0 + adjustment0);
    edge[0] = webp_vp8_clip_byte(q0 - adjustment0);
    edge[signed_step] = webp_vp8_clip_byte(q1 - adjustment1);
    edge[2 * signed_step] = webp_vp8_clip_byte(q2 - adjustment2);
}

static void webp_vp8_filter_edge(uint8_t *plane,
                                 size_t stride,
                                 size_t x,
                                 size_t y,
                                 size_t pixel_count,
                                 int horizontal,
                                 int threshold,
                                 int interior_limit,
                                 int hev_threshold,
                                 int macroblock_edge,
                                 int simple_filter)
{
    uint8_t *edge = plane + y * stride + x;
    size_t step = horizontal ? stride : 1u;
    size_t along = horizontal ? 1u : stride;

    for (size_t i = 0u; i < pixel_count; ++i) {
        uint8_t *segment = edge + i * along;
        if (simple_filter) {
            if (webp_vp8_needs_simple_filter(segment, step,
                                             threshold)) {
                webp_vp8_filter_two(segment, step, 1);
            }
        } else if (webp_vp8_needs_normal_filter(
                       segment, step, threshold, interior_limit)) {
            if (webp_vp8_high_edge_variance(segment, step, hev_threshold)) {
                webp_vp8_filter_two(segment, step, 1);
            } else if (macroblock_edge) {
                webp_vp8_filter_six(segment, step);
            } else {
                webp_vp8_filter_four(segment, step);
            }
        }
    }
}

static void webp_vp8_filter_plane_macroblock(
    uint8_t *plane,
    size_t stride,
    size_t macroblock_x,
    size_t macroblock_y,
    size_t block_size,
    int threshold,
    int interior_limit,
    int hev_threshold,
    int filter_inner,
    int simple_filter)
{
    size_t x = macroblock_x * block_size;
    size_t y = macroblock_y * block_size;

    if (macroblock_x > 0u) {
        webp_vp8_filter_edge(plane, stride, x, y, block_size, 0,
                             threshold + 4, interior_limit, hev_threshold,
                             1, simple_filter);
    }
    if (filter_inner) {
        for (size_t edge = 4u; edge < block_size; edge += 4u) {
            webp_vp8_filter_edge(plane, stride, x + edge, y, block_size,
                                 0, threshold, interior_limit, hev_threshold,
                                 0, simple_filter);
        }
    }
    if (macroblock_y > 0u) {
        webp_vp8_filter_edge(plane, stride, x, y, block_size, 1,
                             threshold + 4, interior_limit, hev_threshold,
                             1, simple_filter);
    }
    if (filter_inner) {
        for (size_t edge = 4u; edge < block_size; edge += 4u) {
            webp_vp8_filter_edge(plane, stride, x, y + edge, block_size,
                                 1, threshold, interior_limit, hev_threshold,
                                 0, simple_filter);
        }
    }
}

static void webp_vp8_apply_loop_filter(
    QwWebPVP8Reconstruction *frame,
    const QwWebPVP8Macroblock *macroblocks,
    size_t macroblock_count)
{
    const QwWebPVP8Header *header = frame->header;

    for (size_t macroblock_y = 0u; macroblock_y < frame->macroblock_rows;
         ++macroblock_y) {
        for (size_t macroblock_x = 0u;
             macroblock_x < frame->macroblock_columns; ++macroblock_x) {
            size_t index = macroblock_y * frame->macroblock_columns +
                           macroblock_x;
            const QwWebPVP8Macroblock *macroblock;
            int level = header->loop_filter_level;
            int interior_limit;
            int filter_limit;
            int hev_threshold;
            int filter_inner;
            int simple_filter = header->loop_filter_type != 0u;

            if (index >= macroblock_count) return;
            macroblock = &macroblocks[index];
            if (macroblock->segment_id >= 4u) return;
            if (header->segmentation_enabled) {
                int segment_level =
                    header->segment_filter_delta[macroblock->segment_id];
                level = header->segmentation_feature_mode
                    ? segment_level
                    : level + segment_level;
            }
            if (header->loop_filter_delta_enabled) {
                level += header->reference_filter_delta[0];
                if (macroblock->y_mode == 4u)
                    level += header->mode_filter_delta[0];
            }
            if (level < 0) level = 0;
            if (level > 63) level = 63;
            if (level == 0) continue;

            interior_limit = level;
            if (header->loop_filter_sharpness != 0u) {
                interior_limit >>= header->loop_filter_sharpness > 4u ? 2 : 1;
                if (interior_limit > 9 - header->loop_filter_sharpness)
                    interior_limit = 9 - header->loop_filter_sharpness;
            }
            if (interior_limit < 1) interior_limit = 1;
            filter_limit = 2 * level + interior_limit;
            hev_threshold = level >= 40 ? 2 : (level >= 15 ? 1 : 0);
            filter_inner = macroblock->y_mode == 4u ||
                frame->macroblock_has_coefficients[index] != 0u;

            webp_vp8_filter_plane_macroblock(
                frame->y_plane, frame->y_stride, macroblock_x, macroblock_y,
                16u, filter_limit, interior_limit, hev_threshold,
                filter_inner, simple_filter);
            if (!simple_filter) {
                webp_vp8_filter_plane_macroblock(
                    frame->u_plane, frame->uv_stride,
                    macroblock_x, macroblock_y, 8u, filter_limit,
                    interior_limit, hev_threshold, filter_inner, 0);
                webp_vp8_filter_plane_macroblock(
                    frame->v_plane, frame->uv_stride,
                    macroblock_x, macroblock_y, 8u, filter_limit,
                    interior_limit, hev_threshold, filter_inner, 0);
            }
        }
    }
}

static void webp_vp8_yuv_to_rgba(uint8_t y, uint8_t u, uint8_t v,
                                uint8_t *rgba)
{
    int luma = ((int)y * 19077) >> 8;
    int u_to_g = ((int)u * 6419) >> 8;
    int v_to_r = ((int)v * 26149) >> 8;
    int v_to_g = ((int)v * 13320) >> 8;
    int u_to_b = ((int)u * 33050) >> 8;
    rgba[0] = webp_vp8_clip_byte((luma + v_to_r - 14234) >> 6);
    rgba[1] = webp_vp8_clip_byte((luma - u_to_g - v_to_g + 8708) >> 6);
    rgba[2] = webp_vp8_clip_byte((luma + u_to_b - 17685) >> 6);
    rgba[3] = 255u;
}

static void webp_vp8_fancy_chroma_quad(uint8_t top_left,
                                       uint8_t top,
                                       uint8_t left,
                                       uint8_t current,
                                       uint8_t output[4])
{
    uint32_t average = (uint32_t)top_left + top + left + current + 8u;
    uint32_t diagonal_12 = (average + 2u * ((uint32_t)top + left)) >> 3u;
    uint32_t diagonal_03 =
        (average + 2u * ((uint32_t)top_left + current)) >> 3u;
    output[0] = (uint8_t)((diagonal_12 + top_left) >> 1u);
    output[1] = (uint8_t)((diagonal_03 + top) >> 1u);
    output[2] = (uint8_t)((diagonal_03 + left) >> 1u);
    output[3] = (uint8_t)((diagonal_12 + current) >> 1u);
}

static void webp_vp8_fancy_upsample_line_pair(
    const uint8_t *top_y,
    const uint8_t *bottom_y,
    const uint8_t *top_u,
    const uint8_t *top_v,
    const uint8_t *current_u,
    const uint8_t *current_v,
    uint8_t *top_rgba,
    uint8_t *bottom_rgba,
    size_t width)
{
    size_t last_pixel_pair = (width - 1u) >> 1u;
    size_t last_chroma = (width - 1u) >> 1u;
    uint8_t top_u_left = top_u[0];
    uint8_t top_v_left = top_v[0];
    uint8_t current_u_left = current_u[0];
    uint8_t current_v_left = current_v[0];
    uint8_t top_u_sample = (uint8_t)(
        (3u * (uint32_t)top_u_left + current_u_left + 2u) >> 2u);
    uint8_t top_v_sample = (uint8_t)(
        (3u * (uint32_t)top_v_left + current_v_left + 2u) >> 2u);

    webp_vp8_yuv_to_rgba(top_y[0], top_u_sample, top_v_sample, top_rgba);
    if (bottom_y) {
        uint8_t bottom_u_sample = (uint8_t)(
            (3u * (uint32_t)current_u_left + top_u_left + 2u) >> 2u);
        uint8_t bottom_v_sample = (uint8_t)(
            (3u * (uint32_t)current_v_left + top_v_left + 2u) >> 2u);
        webp_vp8_yuv_to_rgba(bottom_y[0], bottom_u_sample, bottom_v_sample,
                             bottom_rgba);
    }

    for (size_t x = 1u; x <= last_pixel_pair; ++x) {
        uint8_t top_u_right = top_u[x];
        uint8_t top_v_right = top_v[x];
        uint8_t current_u_right = current_u[x];
        uint8_t current_v_right = current_v[x];
        uint8_t top_u_quad[4];
        uint8_t top_v_quad[4];
        webp_vp8_fancy_chroma_quad(top_u_left, top_u_right,
                                   current_u_left, current_u_right,
                                   top_u_quad);
        webp_vp8_fancy_chroma_quad(top_v_left, top_v_right,
                                   current_v_left, current_v_right,
                                   top_v_quad);
        webp_vp8_yuv_to_rgba(top_y[2u * x - 1u], top_u_quad[0],
                             top_v_quad[0], top_rgba + (2u * x - 1u) * 4u);
        webp_vp8_yuv_to_rgba(top_y[2u * x], top_u_quad[1],
                             top_v_quad[1], top_rgba + 2u * x * 4u);
        if (bottom_y) {
            webp_vp8_yuv_to_rgba(bottom_y[2u * x - 1u], top_u_quad[2],
                                 top_v_quad[2],
                                 bottom_rgba + (2u * x - 1u) * 4u);
            webp_vp8_yuv_to_rgba(bottom_y[2u * x], top_u_quad[3],
                                 top_v_quad[3],
                                 bottom_rgba + 2u * x * 4u);
        }
        top_u_left = top_u_right;
        top_v_left = top_v_right;
        current_u_left = current_u_right;
        current_v_left = current_v_right;
    }

    if ((width & 1u) == 0u) {
        uint8_t edge_top_u = (uint8_t)(
            (3u * (uint32_t)top_u[last_chroma] + current_u[last_chroma] + 2u) >> 2u);
        uint8_t edge_top_v = (uint8_t)(
            (3u * (uint32_t)top_v[last_chroma] + current_v[last_chroma] + 2u) >> 2u);
        webp_vp8_yuv_to_rgba(top_y[width - 1u], edge_top_u, edge_top_v,
                             top_rgba + (width - 1u) * 4u);
        if (bottom_y) {
            uint8_t edge_bottom_u = (uint8_t)(
                (3u * (uint32_t)current_u[last_chroma] + top_u[last_chroma] + 2u) >> 2u);
            uint8_t edge_bottom_v = (uint8_t)(
                (3u * (uint32_t)current_v[last_chroma] + top_v[last_chroma] + 2u) >> 2u);
            webp_vp8_yuv_to_rgba(bottom_y[width - 1u], edge_bottom_u,
                                 edge_bottom_v,
                                 bottom_rgba + (width - 1u) * 4u);
        }
    }
}

int qw_webp_vp8_decode_rgba(const uint8_t *data,
                            size_t size,
                            uint32_t expected_width,
                            uint32_t expected_height,
                            uint8_t **out_rgba)
{
    QwWebPVP8Header header;
    QwWebPVP8Macroblock *macroblocks = NULL;
    QwWebPVP8Reconstruction frame = {0};
    uint8_t *rgba = NULL;
    size_t macroblock_count = 0u;
    size_t y_height;
    size_t uv_height;
    size_t y_bytes;
    size_t uv_bytes;
    size_t pixel_count;
    size_t rgba_size;
    size_t expected_macroblock_count;
    int rc;

    if (!out_rgba) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_rgba = NULL;
    if (!data || !expected_width || !expected_height)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (!qw_image_dimensions_within_limits(expected_width, expected_height))
        return QW_STATUS_ERR_RESOURCE_LIMIT;

    rc = qw_webp_vp8_parse_keyframe_header(data, size, expected_width,
                                            expected_height, &header);
    if (rc != QW_STATUS_OK) return rc;
    frame.header = &header;
    frame.macroblock_columns = ((size_t)expected_width + 15u) / 16u;
    frame.macroblock_rows = ((size_t)expected_height + 15u) / 16u;
    if (!qw_checked_size_mul(frame.macroblock_columns, 16u, &frame.y_stride) ||
        !qw_checked_size_mul(frame.macroblock_rows, 16u, &y_height) ||
        !qw_checked_size_mul(frame.macroblock_columns, 8u, &frame.uv_stride) ||
        !qw_checked_size_mul(frame.macroblock_rows, 8u, &uv_height) ||
        !qw_checked_size_mul(frame.macroblock_columns,
                             frame.macroblock_rows,
                             &expected_macroblock_count) ||
        !qw_checked_size_mul(frame.y_stride, y_height, &y_bytes) ||
        !qw_checked_size_mul(frame.uv_stride, uv_height, &uv_bytes) ||
        !qw_checked_size_mul((size_t)expected_width, (size_t)expected_height,
                             &pixel_count) ||
        !qw_checked_size_mul(pixel_count, 4u, &rgba_size)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    frame.y_plane = (uint8_t *)calloc(y_bytes ? y_bytes : 1u, 1u);
    frame.u_plane = (uint8_t *)calloc(uv_bytes ? uv_bytes : 1u, 1u);
    frame.v_plane = (uint8_t *)calloc(uv_bytes ? uv_bytes : 1u, 1u);
    if (!frame.y_plane || !frame.u_plane || !frame.v_plane) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto done;
    }
    rc = qw_webp_vp8_decode_macroblock_modes(
        data, size, &header, &macroblocks, &macroblock_count);
    if (rc != QW_STATUS_OK) goto done;
    if (macroblock_count != expected_macroblock_count) {
        rc = QW_STATUS_ERR_FORMAT;
        goto done;
    }
    frame.macroblock_has_coefficients =
        (uint8_t *)calloc(macroblock_count, 1u);
    if (!frame.macroblock_has_coefficients) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto done;
    }
    rc = qw_webp_vp8_decode_coefficients(
        data, size, &header, macroblocks, macroblock_count,
        webp_vp8_reconstruct_macroblock, &frame);
    if (rc != QW_STATUS_OK) goto done;
    webp_vp8_apply_loop_filter(&frame, macroblocks, macroblock_count);

    rgba = (uint8_t *)malloc(rgba_size ? rgba_size : 1u);
    if (!rgba) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto done;
    }
    webp_vp8_fancy_upsample_line_pair(
        frame.y_plane, NULL, frame.u_plane, frame.v_plane,
        frame.u_plane, frame.v_plane, rgba, NULL, expected_width);
    for (size_t y = 0u; y + 2u < expected_height; y += 2u) {
        size_t uv_y = (y / 2u) * frame.uv_stride;
        webp_vp8_fancy_upsample_line_pair(
            frame.y_plane + (y + 1u) * frame.y_stride,
            frame.y_plane + (y + 2u) * frame.y_stride,
            frame.u_plane + uv_y, frame.v_plane + uv_y,
            frame.u_plane + uv_y + frame.uv_stride,
            frame.v_plane + uv_y + frame.uv_stride,
            rgba + (y + 1u) * expected_width * 4u,
            rgba + (y + 2u) * expected_width * 4u,
            expected_width);
    }
    if ((expected_height & 1u) == 0u) {
        size_t last_y = (size_t)expected_height - 1u;
        size_t uv_y = (last_y / 2u) * frame.uv_stride;
        webp_vp8_fancy_upsample_line_pair(
            frame.y_plane + last_y * frame.y_stride, NULL,
            frame.u_plane + uv_y, frame.v_plane + uv_y,
            frame.u_plane + uv_y, frame.v_plane + uv_y,
            rgba + last_y * expected_width * 4u, NULL, expected_width);
    }
    *out_rgba = rgba;
    rgba = NULL;
    rc = QW_STATUS_OK;

done:
    free(rgba);
    free(macroblocks);
    free(frame.y_plane);
    free(frame.u_plane);
    free(frame.v_plane);
    free(frame.macroblock_has_coefficients);
    return rc;
}
