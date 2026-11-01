#include "jp2_tier1_model.h"

static int qw_jp2_t1_neighbors_valid(const QwJp2Tier1Neighbors *neighbors)
{
    return neighbors &&
           neighbors->west >= -1 && neighbors->west <= 1 &&
           neighbors->east >= -1 && neighbors->east <= 1 &&
           neighbors->north >= -1 && neighbors->north <= 1 &&
           neighbors->south >= -1 && neighbors->south <= 1 &&
           neighbors->northwest >= -1 && neighbors->northwest <= 1 &&
           neighbors->northeast >= -1 && neighbors->northeast <= 1 &&
           neighbors->southwest >= -1 && neighbors->southwest <= 1 &&
           neighbors->southeast >= -1 && neighbors->southeast <= 1;
}

static uint8_t qw_jp2_t1_count_significant(int8_t first, int8_t second)
{
    return (uint8_t)((first != 0) + (second != 0));
}

/* Context thresholds and orientation grouping follow ITU-T T.800 Table D.1. */
int qw_jp2_t1_significance_context(const QwJp2Tier1Neighbors *neighbors,
                                  QwJp2Tier1Orientation orientation,
                                  uint8_t *context)
{
    uint8_t horizontal;
    uint8_t vertical;
    uint8_t diagonal;

    if (!qw_jp2_t1_neighbors_valid(neighbors) || !context ||
        orientation < QW_JP2_T1_ORIENTATION_LL_LH ||
        orientation > QW_JP2_T1_ORIENTATION_HH) {
        return QW_JP2_T1_ERR_ARGUMENT;
    }

    horizontal = qw_jp2_t1_count_significant(neighbors->west,
                                             neighbors->east);
    vertical = qw_jp2_t1_count_significant(neighbors->north,
                                           neighbors->south);
    diagonal = (uint8_t)((neighbors->northwest != 0) +
                         (neighbors->northeast != 0) +
                         (neighbors->southwest != 0) +
                         (neighbors->southeast != 0));

    if (orientation == QW_JP2_T1_ORIENTATION_LL_LH) {
        if (horizontal >= 2u) *context = 8u;
        else if (horizontal == 1u) {
            if (vertical != 0u) *context = 7u;
            else if (diagonal != 0u) *context = 6u;
            else *context = 5u;
        } else if (vertical >= 2u) *context = 4u;
        else if (vertical == 1u) *context = 3u;
        else if (diagonal >= 2u) *context = 2u;
        else if (diagonal == 1u) *context = 1u;
        else *context = 0u;
    } else if (orientation == QW_JP2_T1_ORIENTATION_HL) {
        if (vertical >= 2u) *context = 8u;
        else if (vertical == 1u) {
            if (horizontal != 0u) *context = 7u;
            else if (diagonal != 0u) *context = 6u;
            else *context = 5u;
        } else if (horizontal >= 2u) *context = 4u;
        else if (horizontal == 1u) *context = 3u;
        else if (diagonal >= 2u) *context = 2u;
        else if (diagonal == 1u) *context = 1u;
        else *context = 0u;
    } else {
        uint8_t axial = (uint8_t)(horizontal + vertical);
        if (diagonal >= 3u) *context = 8u;
        else if (diagonal == 2u) *context = axial ? 7u : 6u;
        else if (diagonal == 1u) {
            if (axial >= 2u) *context = 5u;
            else if (axial == 1u) *context = 4u;
            else *context = 3u;
        } else if (axial >= 2u) *context = 2u;
        else if (axial == 1u) *context = 1u;
        else *context = 0u;
    }
    return QW_JP2_T1_OK;
}

static int8_t qw_jp2_t1_pair_contribution(int8_t first, int8_t second)
{
    if ((first > 0 && second < 0) || (first < 0 && second > 0)) return 0;
    if (first > 0 || second > 0) return 1;
    if (first < 0 || second < 0) return -1;
    return 0;
}

/* Sign context labels and prediction XOR follow ITU-T T.800 Tables D.2-D.3. */
int qw_jp2_t1_sign_context(const QwJp2Tier1Neighbors *neighbors,
                           uint8_t *context,
                           uint8_t *xor_bit)
{
    int8_t horizontal;
    int8_t vertical;
    uint8_t result_context;
    uint8_t result_xor;

    if (!qw_jp2_t1_neighbors_valid(neighbors) || !context || !xor_bit) {
        return QW_JP2_T1_ERR_ARGUMENT;
    }

    horizontal = qw_jp2_t1_pair_contribution(neighbors->west,
                                             neighbors->east);
    vertical = qw_jp2_t1_pair_contribution(neighbors->north,
                                           neighbors->south);
    result_xor = (horizontal < 0 ||
                  (horizontal == 0 && vertical < 0)) ? 1u : 0u;

    if (horizontal == 0) {
        result_context = vertical == 0 ? 9u : 10u;
    } else if (horizontal > 0) {
        result_context = vertical > 0 ? 13u : vertical == 0 ? 12u : 11u;
    } else {
        result_context = vertical > 0 ? 11u : vertical == 0 ? 12u : 13u;
    }

    *context = result_context;
    *xor_bit = result_xor;
    return QW_JP2_T1_OK;
}

int qw_jp2_t1_magnitude_refinement_context(
    const QwJp2Tier1Neighbors *neighbors,
    uint8_t first_refinement,
    uint8_t *context)
{
    uint8_t neighbor_count;

    if (!qw_jp2_t1_neighbors_valid(neighbors) || first_refinement > 1u ||
        !context) {
        return QW_JP2_T1_ERR_ARGUMENT;
    }
    if (!first_refinement) {
        *context = 16u;
        return QW_JP2_T1_OK;
    }

    neighbor_count = (uint8_t)(
        (neighbors->west != 0) + (neighbors->east != 0) +
        (neighbors->north != 0) + (neighbors->south != 0) +
        (neighbors->northwest != 0) + (neighbors->northeast != 0) +
        (neighbors->southwest != 0) + (neighbors->southeast != 0));
    *context = neighbor_count ? 15u : 14u;
    return QW_JP2_T1_OK;
}
