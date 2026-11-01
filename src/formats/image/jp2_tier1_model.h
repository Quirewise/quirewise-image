#ifndef QUIREWISE_JP2_TIER1_MODEL_H
#define QUIREWISE_JP2_TIER1_MODEL_H

#include <stdint.h>

typedef enum QwJp2Tier1Orientation {
    QW_JP2_T1_ORIENTATION_LL_LH = 0,
    QW_JP2_T1_ORIENTATION_HL = 1,
    QW_JP2_T1_ORIENTATION_HH = 2
} QwJp2Tier1Orientation;

/* Each neighbor is -1 for significant-negative, 0 for insignificant, or
 * +1 for significant-positive. Neighbors outside a code-block are zero. */
typedef struct QwJp2Tier1Neighbors {
    int8_t west;
    int8_t east;
    int8_t north;
    int8_t south;
    int8_t northwest;
    int8_t northeast;
    int8_t southwest;
    int8_t southeast;
} QwJp2Tier1Neighbors;

enum {
    QW_JP2_T1_OK = 0,
    QW_JP2_T1_ERR_ARGUMENT = -1
};

int qw_jp2_t1_significance_context(const QwJp2Tier1Neighbors *neighbors,
                                  QwJp2Tier1Orientation orientation,
                                  uint8_t *context);
int qw_jp2_t1_sign_context(const QwJp2Tier1Neighbors *neighbors,
                           uint8_t *context,
                           uint8_t *xor_bit);
int qw_jp2_t1_magnitude_refinement_context(
    const QwJp2Tier1Neighbors *neighbors,
    uint8_t first_refinement,
    uint8_t *context);

#endif
