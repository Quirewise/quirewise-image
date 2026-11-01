#ifndef QUIREWISE_JP2_DWT_H
#define QUIREWISE_JP2_DWT_H

#include <stddef.h>
#include <stdint.h>

typedef enum QwJp2DwtStatus {
    QW_JP2_DWT_OK = 0,
    QW_JP2_DWT_ERR_ARGUMENT = -1,
    QW_JP2_DWT_ERR_RANGE = -2
} QwJp2DwtStatus;

/*
 * Inverts one 5/3 lifting level. Low and high coefficients are separate,
 * equally spaced sequences; first_is_high selects which sequence occupies
 * output sample zero. The output buffer must not overlap either input.
 */
int qw_jp2_dwt53_inverse_1d(const int64_t *low,
                            size_t low_count,
                            size_t low_stride,
                            const int64_t *high,
                            size_t high_count,
                            size_t high_stride,
                            uint8_t first_is_high,
                            int64_t *output,
                            size_t output_capacity);

/*
 * Inverts one packed 2-D level in place. The plane stores its four subbands
 * in the fixed LL/HL/LH/HH quadrants; first_is_high flags describe the
 * reconstructed sample phase and do not change subband placement.
 * scratch must hold max(width, height) int64_t values and must not overlap
 * the coefficient plane.
 */
int qw_jp2_dwt53_inverse_level(int64_t *coefficients,
                               size_t stride,
                               size_t width,
                               size_t height,
                               uint8_t horizontal_first_is_high,
                               uint8_t vertical_first_is_high,
                               int64_t *scratch,
                               size_t scratch_capacity);

/*
 * Inverts one irreversible 9/7 lifting level. The coefficient sequences are
 * separate and equally spaced; first_is_high selects the phase of output
 * sample zero. The output buffer must not overlap either input.
 */
int qw_jp2_dwt97_inverse_1d(const double *low,
                            size_t low_count,
                            size_t low_stride,
                            const double *high,
                            size_t high_count,
                            size_t high_stride,
                            uint8_t first_is_high,
                            double *output,
                            size_t output_capacity);

/*
 * Inverts one packed 2-D 9/7 level in place. The plane stores the four
 * subbands in fixed LL/HL/LH/HH quadrants. scratch must hold max(width,
 * height) double values and must not overlap the coefficient plane.
 */
int qw_jp2_dwt97_inverse_level(double *coefficients,
                               size_t stride,
                               size_t width,
                               size_t height,
                               uint8_t horizontal_first_is_high,
                               uint8_t vertical_first_is_high,
                               double *scratch,
                               size_t scratch_capacity);

#endif
