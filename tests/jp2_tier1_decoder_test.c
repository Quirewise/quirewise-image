#include "jp2_tier1_decoder.h"

#include <stdio.h>
#include <string.h>

static int test_empty_passes(void)
{
    static const uint8_t payload[] = {0u, 0u};
    QwJp2MqDecoder mq;
    QwJp2Tier1Coefficient coefficients[4];

    if (qw_jp2_mq_init(&mq, payload, sizeof(payload)) != QW_JP2_MQ_OK) {
        fputs("Tier-1 MQ init failed for empty-pass test\n", stderr);
        return 0;
    }
    memset(coefficients, 0xff, sizeof(coefficients));
    if (qw_jp2_t1_decode_mq_codeblock(
            &mq, 2u, 2u, 2u, 0u, QW_JP2_T1_ORIENTATION_LL_LH, 0u,
            coefficients, 4u) != QW_JP2_T1_OK) {
        fputs("Tier-1 zero-pass block decode failed\n", stderr);
        return 0;
    }
    for (size_t i = 0u; i < 4u; ++i) {
        if (coefficients[i].magnitude != 0u || coefficients[i].negative != 0u) {
            fputs("Tier-1 zero-pass block was not cleared\n", stderr);
            return 0;
        }
    }
    return 1;
}

static int test_single_coefficient_cleanup(void)
{
    /* OpenJPEG 2.5.4 body for a 1x1 8-bit gray sample of 96, encoded with
       -n 1 -b 4,4 -M 32. The decoded level-shifted coefficient is -32. */
    static const uint8_t payload[] = {0x08u, 0xa0u, 0x57u, 0xabu};
    QwJp2MqDecoder mq;
    QwJp2Tier1Coefficient coefficient = {0u, 0u};

    if (qw_jp2_mq_init(&mq, payload, sizeof(payload)) != QW_JP2_MQ_OK ||
        qw_jp2_t1_decode_mq_codeblock(
            &mq, 1u, 1u, 6u, 16u, QW_JP2_T1_ORIENTATION_LL_LH, 0x20u,
            &coefficient, 1u) != QW_JP2_T1_OK ||
        coefficient.magnitude != 32u || coefficient.negative != 1u) {
        fprintf(stderr,
                "Tier-1 OpenJPEG reference coefficient mismatch (magnitude=%llu negative=%u)\n",
                (unsigned long long)coefficient.magnitude,
                coefficient.negative);
        return 0;
    }
    return 1;
}

static int test_cleanup_run_and_pass_limits(void)
{
    static const uint8_t payload[] = {
        0x00u, 0x00u, 0x80u, 0x7fu, 0x55u, 0xaau, 0xffu, 0x7fu
    };
    QwJp2MqDecoder mq;
    QwJp2Tier1Coefficient coefficients[4];
    QwJp2Tier1Coefficient untouched = {9u, 1u};

    if (qw_jp2_mq_init(&mq, payload, sizeof(payload)) != QW_JP2_MQ_OK ||
        qw_jp2_t1_decode_mq_codeblock(
            &mq, 1u, 4u, 1u, 1u, QW_JP2_T1_ORIENTATION_LL_LH, 0u,
            coefficients, 4u) != QW_JP2_T1_OK) {
        fputs("Tier-1 cleanup run decode failed\n", stderr);
        return 0;
    }
    if (coefficients[0].magnitude != 0u || coefficients[0].negative != 0u ||
        coefficients[1].magnitude != 0u || coefficients[1].negative != 0u ||
        coefficients[2].magnitude != 0u || coefficients[2].negative != 0u ||
        coefficients[3].magnitude != 1u || coefficients[3].negative != 1u) {
        fputs("Tier-1 cleanup run vector did not select the fourth row\n",
              stderr);
        return 0;
    }
    if (qw_jp2_t1_decode_mq_codeblock(
            &mq, 1u, 1u, 1u, 2u, QW_JP2_T1_ORIENTATION_LL_LH, 0u,
            &untouched, 1u) != QW_JP2_T1_ERR_FORMAT ||
        untouched.magnitude != 9u || untouched.negative != 1u) {
        fputs("Tier-1 impossible pass count handling mismatch\n", stderr);
        return 0;
    }
    return 1;
}

static int test_openjpeg_zero_block_reference(void)
{
    /* OpenJPEG 2.5.4 body for a 4x4 all-zero gray image, encoded with
       -n 1 -b 4,4 -M 32. All level-shifted coefficients are -128. */
    static const uint8_t payload[] = {
        0x11u, 0x50u, 0x4du, 0x37u, 0x8bu, 0x23u, 0xe3u, 0x0fu
    };
    QwJp2MqDecoder mq;
    QwJp2Tier1Coefficient coefficients[16];
    QwJp2Tier1Coefficient truncated[16];

    if (qw_jp2_mq_init(&mq, payload, sizeof(payload)) != QW_JP2_MQ_OK ||
        qw_jp2_t1_decode_mq_codeblock(
            &mq, 4u, 4u, 8u, 22u, QW_JP2_T1_ORIENTATION_LL_LH, 0x20u,
            coefficients, 16u) != QW_JP2_T1_OK) {
        fputs("Tier-1 OpenJPEG zero-block reference decode failed\n", stderr);
        return 0;
    }
    for (size_t i = 0u; i < 16u; ++i) {
        if (coefficients[i].magnitude != 128u ||
            coefficients[i].negative != 1u) {
            fprintf(stderr,
                    "Tier-1 OpenJPEG zero-block coefficient mismatch at %zu\n",
                    i);
            return 0;
        }
    }
    return 1;
}

static int test_significance_propagation_vector(void)
{
    /* OpenJPEG 2.5.4 body for PGM samples
       0 128 192 255 / 64 160 224 240 / 127 129 191 193 / 32 96 224 255,
       encoded with -n 1 -b 4,4 -M 32. With one resolution, coefficients are
       samples-128. Mixed magnitudes exercise later-plane significance coding. */
    static const uint8_t payload[] = {
        0x12u, 0x11u, 0xeau, 0x95u, 0x0fu, 0x4fu, 0xd1u, 0xa6u,
        0xbdu, 0x36u, 0xc1u, 0xa6u, 0x3eu, 0xdbu, 0x5bu, 0x92u,
        0x5cu, 0x3bu, 0x50u, 0xbeu, 0xfdu, 0xacu, 0x7fu
    };
    static const QwJp2Tier1Coefficient expected[16] = {
        {128u, 1u}, {0u, 0u}, {64u, 0u}, {127u, 0u},
        {64u, 1u}, {32u, 0u}, {96u, 0u}, {112u, 0u},
        {1u, 1u}, {1u, 0u}, {63u, 0u}, {65u, 0u},
        {96u, 1u}, {32u, 1u}, {96u, 0u}, {127u, 0u}
    };
    QwJp2MqDecoder mq;
    QwJp2Tier1Coefficient coefficients[16];
    QwJp2Tier1Coefficient truncated[16];

    if (qw_jp2_mq_init(&mq, payload, sizeof(payload)) != QW_JP2_MQ_OK ||
        qw_jp2_t1_decode_mq_codeblock(
            &mq, 4u, 4u, 8u, 22u, QW_JP2_T1_ORIENTATION_LL_LH, 0x20u,
            coefficients, 16u) != QW_JP2_T1_OK) {
        fputs("Tier-1 OpenJPEG mixed-block reference decode failed\n", stderr);
        return 0;
    }
    for (size_t i = 0u; i < 16u; ++i) {
        if (coefficients[i].magnitude != expected[i].magnitude ||
            coefficients[i].negative != expected[i].negative ||
            coefficients[i].reconstruction_shift !=
                (expected[i].magnitude == 0u
                     ? 0u : QW_JP2_T1_NO_RECONSTRUCTION_MIDPOINT)) {
            fprintf(stderr,
                    "Tier-1 OpenJPEG mixed-block coefficient/midpoint mismatch at %zu\n",
                    i);
            return 0;
        }
    }
    if (qw_jp2_mq_init(&mq, payload, sizeof(payload)) != QW_JP2_MQ_OK ||
        qw_jp2_t1_decode_mq_codeblock(
            &mq, 4u, 4u, 8u, 2u, QW_JP2_T1_ORIENTATION_LL_LH, 0x20u,
            truncated, 16u) != QW_JP2_T1_OK ||
        truncated[0].magnitude != 128u ||
        truncated[0].reconstruction_shift != 7u ||
        truncated[4].magnitude != 64u ||
        truncated[4].reconstruction_shift != 6u) {
        fputs("Tier-1 truncated block lost per-coefficient reconstruction planes\n",
              stderr);
        return 0;
    }
    return 1;
}

static int test_unsupported_styles_and_limits(void)
{
    static const uint8_t payload[] = {0u, 0u};
    QwJp2MqDecoder mq;
    QwJp2Tier1Coefficient coefficient = {7u, 1u};

    if (qw_jp2_mq_init(&mq, payload, sizeof(payload)) != QW_JP2_MQ_OK ||
        qw_jp2_t1_decode_mq_codeblock(
            &mq, 1u, 1u, 1u, 1u, QW_JP2_T1_ORIENTATION_LL_LH,
            QW_JP2_T1_STYLE_BYPASS, &coefficient, 1u) !=
            QW_JP2_T1_ERR_UNSUPPORTED ||
        coefficient.magnitude != 7u || coefficient.negative != 1u ||
        qw_jp2_t1_decode_mq_codeblock(
            &mq, QW_JP2_T1_MAX_CODEBLOCK_DIMENSION + 1u, 1u, 1u, 1u,
            QW_JP2_T1_ORIENTATION_LL_LH, 0u, &coefficient, 1u) !=
            QW_JP2_T1_ERR_ARGUMENT) {
        fputs("Tier-1 unsupported style/dimension handling mismatch\n",
              stderr);
        return 0;
    }
    return 1;
}

int main(void)
{
    if (!test_empty_passes() || !test_single_coefficient_cleanup() ||
        !test_cleanup_run_and_pass_limits() ||
        !test_openjpeg_zero_block_reference() ||
        !test_significance_propagation_vector() ||
        !test_unsupported_styles_and_limits()) {
        return 1;
    }
    puts("Quirewise JPEG 2000 Tier-1 decoder tests passed");
    return 0;
}
