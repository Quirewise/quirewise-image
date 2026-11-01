#include "jp2_tier1_model.h"

#include <stdio.h>
#include <string.h>

typedef struct QwJp2T1SignificanceVector {
    uint8_t horizontal;
    uint8_t vertical;
    uint8_t diagonal;
    uint8_t expected_context;
} QwJp2T1SignificanceVector;

static void set_neighbors(QwJp2Tier1Neighbors *neighbors,
                          uint8_t horizontal,
                          uint8_t vertical,
                          uint8_t diagonal)
{
    memset(neighbors, 0, sizeof(*neighbors));
    if (horizontal > 0u) neighbors->west = 1;
    if (horizontal > 1u) neighbors->east = 1;
    if (vertical > 0u) neighbors->north = 1;
    if (vertical > 1u) neighbors->south = 1;
    if (diagonal > 0u) neighbors->northwest = 1;
    if (diagonal > 1u) neighbors->northeast = 1;
    if (diagonal > 2u) neighbors->southwest = 1;
    if (diagonal > 3u) neighbors->southeast = 1;
}

static int test_significance_contexts(void)
{
    static const QwJp2T1SignificanceVector ll_lh[] = {
        {0u, 0u, 0u, 0u}, {0u, 0u, 1u, 1u}, {0u, 0u, 2u, 2u},
        {0u, 1u, 0u, 3u}, {0u, 2u, 0u, 4u}, {1u, 0u, 0u, 5u},
        {1u, 0u, 1u, 6u}, {1u, 1u, 0u, 7u}, {2u, 0u, 0u, 8u}
    };
    static const QwJp2T1SignificanceVector hl[] = {
        {0u, 0u, 0u, 0u}, {0u, 0u, 1u, 1u}, {0u, 0u, 2u, 2u},
        {1u, 0u, 0u, 3u}, {2u, 0u, 0u, 4u}, {0u, 1u, 0u, 5u},
        {0u, 1u, 1u, 6u}, {1u, 1u, 0u, 7u}, {0u, 2u, 0u, 8u}
    };
    static const QwJp2T1SignificanceVector hh[] = {
        {0u, 0u, 0u, 0u}, {1u, 0u, 0u, 1u}, {2u, 0u, 0u, 2u},
        {0u, 0u, 1u, 3u}, {1u, 0u, 1u, 4u}, {2u, 0u, 1u, 5u},
        {0u, 0u, 2u, 6u}, {1u, 0u, 2u, 7u}, {0u, 0u, 3u, 8u}
    };
    const QwJp2T1SignificanceVector *vectors[3] = {ll_lh, hl, hh};
    const size_t counts[3] = {
        sizeof(ll_lh) / sizeof(ll_lh[0]),
        sizeof(hl) / sizeof(hl[0]),
        sizeof(hh) / sizeof(hh[0])
    };
    QwJp2Tier1Neighbors neighbors;

    for (size_t orientation = 0u; orientation < 3u; ++orientation) {
        for (size_t i = 0u; i < counts[orientation]; ++i) {
            uint8_t context = 0xffu;
            set_neighbors(&neighbors, vectors[orientation][i].horizontal,
                          vectors[orientation][i].vertical,
                          vectors[orientation][i].diagonal);
            if (qw_jp2_t1_significance_context(
                    &neighbors, (QwJp2Tier1Orientation)orientation,
                    &context) != QW_JP2_T1_OK ||
                context != vectors[orientation][i].expected_context) {
                fprintf(stderr,
                        "Tier-1 significance context mismatch: orientation=%zu vector=%zu got=%u expected=%u\n",
                        orientation, i, context,
                        vectors[orientation][i].expected_context);
                return 0;
            }
        }
    }
    return 1;
}

static void set_pair_contribution(int8_t contribution,
                                  int8_t *first,
                                  int8_t *second)
{
    if (contribution > 0) {
        *first = 1;
        *second = 0;
    } else if (contribution < 0) {
        *first = -1;
        *second = 0;
    } else {
        *first = 1;
        *second = -1;
    }
}

static int test_sign_contexts(void)
{
    static const struct {
        int8_t horizontal;
        int8_t vertical;
        uint8_t context;
        uint8_t xor_bit;
    } vectors[] = {
        { 1,  1, 13u, 0u}, { 1,  0, 12u, 0u}, { 1, -1, 11u, 0u},
        { 0,  1, 10u, 0u}, { 0,  0,  9u, 0u}, { 0, -1, 10u, 1u},
        {-1,  1, 11u, 1u}, {-1,  0, 12u, 1u}, {-1, -1, 13u, 1u}
    };
    QwJp2Tier1Neighbors neighbors = {0};

    for (size_t i = 0u; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        uint8_t context = 0xffu;
        uint8_t xor_bit = 0xffu;
        set_pair_contribution(vectors[i].horizontal, &neighbors.west,
                              &neighbors.east);
        set_pair_contribution(vectors[i].vertical, &neighbors.north,
                              &neighbors.south);
        if (qw_jp2_t1_sign_context(&neighbors, &context, &xor_bit) !=
                QW_JP2_T1_OK ||
            context != vectors[i].context || xor_bit != vectors[i].xor_bit) {
            fprintf(stderr,
                    "Tier-1 sign context mismatch: vector=%zu got=%u/%u expected=%u/%u\n",
                    i, context, xor_bit, vectors[i].context,
                    vectors[i].xor_bit);
            return 0;
        }
    }
    return 1;
}

static int test_magnitude_refinement_contexts(void)
{
    QwJp2Tier1Neighbors neighbors = {0};
    uint8_t context = 0xffu;

    if (qw_jp2_t1_magnitude_refinement_context(&neighbors, 0u, &context) !=
            QW_JP2_T1_OK || context != 16u ||
        qw_jp2_t1_magnitude_refinement_context(&neighbors, 1u, &context) !=
            QW_JP2_T1_OK || context != 14u) {
        fputs("Tier-1 first magnitude refinement contexts mismatch\n", stderr);
        return 0;
    }
    neighbors.southeast = -1;
    if (qw_jp2_t1_magnitude_refinement_context(&neighbors, 1u, &context) !=
            QW_JP2_T1_OK || context != 15u) {
        fputs("Tier-1 subsequent magnitude refinement context mismatch\n",
              stderr);
        return 0;
    }
    return 1;
}

static int test_invalid_inputs(void)
{
    QwJp2Tier1Neighbors neighbors = {0};
    uint8_t context = 0u;
    uint8_t xor_bit = 0u;

    if (qw_jp2_t1_significance_context(
            &neighbors, (QwJp2Tier1Orientation)3, &context) !=
            QW_JP2_T1_ERR_ARGUMENT ||
        qw_jp2_t1_significance_context(&neighbors,
                                       QW_JP2_T1_ORIENTATION_HH, NULL) !=
            QW_JP2_T1_ERR_ARGUMENT ||
        qw_jp2_t1_sign_context(&neighbors, &context, NULL) !=
            QW_JP2_T1_ERR_ARGUMENT ||
        qw_jp2_t1_magnitude_refinement_context(&neighbors, 2u, &context) !=
            QW_JP2_T1_ERR_ARGUMENT) {
        fputs("Tier-1 invalid argument handling mismatch\n", stderr);
        return 0;
    }
    neighbors.west = 2;
    if (qw_jp2_t1_sign_context(&neighbors, &context, &xor_bit) !=
        QW_JP2_T1_ERR_ARGUMENT) {
        fputs("Tier-1 invalid neighbor state was accepted\n", stderr);
        return 0;
    }
    return 1;
}

int main(void)
{
    if (!test_significance_contexts() || !test_sign_contexts() ||
        !test_magnitude_refinement_contexts() || !test_invalid_inputs()) {
        return 1;
    }
    puts("Quirewise JPEG 2000 Tier-1 context model tests passed");
    return 0;
}
