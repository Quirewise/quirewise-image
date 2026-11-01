#include "jp2_dwt.h"

#include <float.h>
#include <stdio.h>
#include <string.h>

static int expect_line(const char *name,
                       const int64_t *actual,
                       const int64_t *expected,
                       size_t count)
{
    for (size_t i = 0u; i < count; ++i) {
        if (actual[i] != expected[i]) {
            fprintf(stderr, "%s mismatch at %zu: got %lld expected %lld\n",
                    name, i, (long long)actual[i],
                    (long long)expected[i]);
            return 0;
        }
    }
    return 1;
}

static int test_inverse_1d_even_phase(void)
{
    static const int64_t low[] = {2, 8};
    static const int64_t high[] = {0, -5};
    static const int64_t expected[] = {2, 5, 9, 4};
    int64_t output[4] = {0};

    return qw_jp2_dwt53_inverse_1d(
               low, 2u, 1u, high, 2u, 1u, 0u, output, 4u) ==
               QW_JP2_DWT_OK &&
           expect_line("5/3 even-phase inverse", output, expected, 4u);
}

static int test_inverse_1d_odd_phase(void)
{
    static const int64_t low[] = {8, 2};
    static const int64_t high[] = {-4, -1};
    static const int64_t expected[] = {5, 9, 4, 2};
    int64_t output[4] = {0};

    return qw_jp2_dwt53_inverse_1d(
               low, 2u, 1u, high, 2u, 1u, 1u, output, 4u) ==
               QW_JP2_DWT_OK &&
           expect_line("5/3 odd-phase inverse", output, expected, 4u);
}

static int test_single_sample_phases(void)
{
    static const int64_t low[] = {7};
    static const int64_t high[] = {-3};
    int64_t output = 0;

    if (qw_jp2_dwt53_inverse_1d(
            low, 1u, 1u, NULL, 0u, 0u, 0u, &output, 1u) !=
            QW_JP2_DWT_OK || output != 7) {
        fputs("5/3 even-phase singleton mismatch\n", stderr);
        return 0;
    }
    if (qw_jp2_dwt53_inverse_1d(
            NULL, 0u, 0u, high, 1u, 1u, 1u, &output, 1u) !=
            QW_JP2_DWT_OK || output != -1) {
        fputs("5/3 odd-phase singleton mismatch\n", stderr);
        return 0;
    }
    return 1;
}

static int test_rejects_bad_ranges_transactionally(void)
{
    static const int64_t too_large[] = {INT64_MAX};
    int64_t output[2] = {41, 42};

    if (qw_jp2_dwt53_inverse_1d(
            too_large, 1u, 1u, NULL, 0u, 0u, 0u, output, 1u) !=
            QW_JP2_DWT_ERR_RANGE || output[0] != 41 || output[1] != 42) {
        fputs("5/3 range failure changed output\n", stderr);
        return 0;
    }
    if (qw_jp2_dwt53_inverse_1d(
            too_large, 1u, 1u, NULL, 0u, 0u, 2u, output, 1u) !=
            QW_JP2_DWT_ERR_ARGUMENT || output[0] != 41 || output[1] != 42) {
        fputs("5/3 argument failure changed output\n", stderr);
        return 0;
    }
    return 1;
}

static int test_rejects_unrepresentable_spans_transactionally(void)
{
    static const int64_t values[] = {1, 2};
    int64_t output[2] = {41, 42};
    int64_t plane[2] = {11, 12};
    int64_t scratch[2] = {0, 0};
    size_t oversized_stride = SIZE_MAX / sizeof(int64_t);

    if (qw_jp2_dwt53_inverse_1d(
            values, 2u, oversized_stride, NULL, 0u, 0u, 0u,
            output, 2u) != QW_JP2_DWT_ERR_ARGUMENT ||
        output[0] != 41 || output[1] != 42) {
        fputs("5/3 rejected 1-D span changed output or returned wrong status\n",
              stderr);
        return 0;
    }
    if (qw_jp2_dwt53_inverse_level(
            plane, oversized_stride, 1u, 2u, 0u, 0u,
            scratch, 2u) != QW_JP2_DWT_ERR_ARGUMENT ||
        plane[0] != 11 || plane[1] != 12) {
        fputs("5/3 rejected 2-D span changed coefficients or returned wrong status\n",
              stderr);
        return 0;
    }
    return 1;
}

static int test_inverse_level_keeps_odd_phase_bands_in_fixed_quadrants(void)
{
    static const int64_t expected_horizontal[2][4] = {
        {5, 9, 4, 2},
        {5, 9, 4, 2}
    };
    static const int64_t expected_vertical[4] = {5, 9, 4, 2};
    int64_t horizontal_coefficients[2][4] = {
        {8, 2, -4, -1},
        {0, 0, 0, 0}
    };
    int64_t vertical_coefficients[4][1] = {
        {8}, {2}, {-4}, {-1}
    };
    int64_t scratch[4] = {0};

    if (qw_jp2_dwt53_inverse_level(
            &horizontal_coefficients[0][0], 4u, 4u, 2u, 1u, 0u,
            scratch, 4u) != QW_JP2_DWT_OK ||
        !expect_line("5/3 odd horizontal-phase row 0",
                     horizontal_coefficients[0], expected_horizontal[0], 4u) ||
        !expect_line("5/3 odd horizontal-phase row 1",
                     horizontal_coefficients[1], expected_horizontal[1], 4u)) {
        fputs("5/3 odd horizontal phase changed fixed quadrant placement\n",
              stderr);
        return 0;
    }
    if (qw_jp2_dwt53_inverse_level(
            &vertical_coefficients[0][0], 1u, 1u, 4u, 0u, 1u,
            scratch, 4u) != QW_JP2_DWT_OK ||
        !expect_line("5/3 odd vertical-phase column",
                     &vertical_coefficients[0][0], expected_vertical, 4u)) {
        fputs("5/3 odd vertical phase changed fixed quadrant placement\n",
              stderr);
        return 0;
    }
    return 1;
}

static int test_t800_annex_j_53_reference(void)
{
    /*
     * Tables J.11-J.17 in ITU-T T.800, Annex J.4.2, packed as the
     * two-level 13x17 coefficient plane shown in Figure F.18.
     */
    static const int64_t reference_coefficients[17][13] = {
        {0, 4, 8, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {4, 5, 8, 12, 0, 1, 0, 0, 0, 0, 0, 0, 0},
        {8, 8, 11, 15, 0, 1, 0, 0, 1, 0, 1, 0, 0},
        {12, 12, 14, 18, 0, 0, 1, 0, 0, 0, 0, -1, 1},
        {16, 16, 18, 20, 0, 0, 0, 0, 0, 0, 0, 1, 1},
        {0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 1, 0, -1},
        {0, 1, 1, 1, 0, -1, 0, 0, 0, 1, 0, 1, 1},
        {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0},
        {0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1},
        {0, 0, 0, 0, 1, 0, 2, 0, 0, 0, 0, 0, 1},
        {0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0},
        {0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1},
        {0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, -1, 0}
    };
    /* Table J.3 source tile component samples, 13 samples by 17 samples. */
    static const int64_t expected[17][13] = {
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
        {1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
        {2, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
        {3, 3, 3, 4, 5, 5, 6, 7, 8, 9, 10, 11, 12},
        {4, 4, 4, 5, 5, 6, 7, 8, 8, 9, 10, 11, 12},
        {5, 5, 5, 5, 6, 7, 7, 8, 9, 10, 11, 12, 13},
        {6, 6, 6, 6, 7, 7, 8, 9, 10, 10, 11, 12, 13},
        {7, 7, 7, 7, 8, 8, 9, 9, 10, 11, 12, 13, 13},
        {8, 8, 8, 8, 8, 9, 10, 10, 11, 12, 12, 13, 14},
        {9, 9, 9, 9, 9, 10, 10, 11, 12, 12, 13, 14, 15},
        {10, 10, 10, 10, 10, 11, 11, 12, 12, 13, 14, 14, 15},
        {11, 11, 11, 11, 11, 12, 12, 13, 13, 14, 14, 15, 16},
        {12, 12, 12, 12, 12, 13, 13, 13, 14, 15, 15, 16, 16},
        {13, 13, 13, 13, 13, 13, 14, 14, 15, 15, 16, 17, 17},
        {14, 14, 14, 14, 14, 14, 15, 15, 16, 16, 17, 17, 18},
        {15, 15, 15, 15, 15, 15, 16, 16, 17, 17, 18, 18, 19},
        {16, 16, 16, 16, 16, 16, 17, 17, 17, 18, 18, 19, 20}
    };
    int64_t coefficients[17][13];
    int64_t scratch[17];

    memcpy(coefficients, reference_coefficients, sizeof(coefficients));
    if (qw_jp2_dwt53_inverse_level(
            &coefficients[0][0], 13u, 7u, 9u, 0u, 0u,
            scratch, 17u) != QW_JP2_DWT_OK ||
        qw_jp2_dwt53_inverse_level(
            &coefficients[0][0], 13u, 13u, 17u, 0u, 0u,
            scratch, 17u) != QW_JP2_DWT_OK) {
        fputs("T.800 5/3 inverse transform rejected its reference plane\n",
              stderr);
        return 0;
    }
    for (size_t y = 0u; y < 17u; ++y) {
        for (size_t x = 0u; x < 13u; ++x) {
            if (coefficients[y][x] != expected[y][x]) {
                fprintf(stderr,
                        "T.800 5/3 sample mismatch at (%zu,%zu): got %lld expected %lld\n",
                        x, y, (long long)coefficients[y][x],
                        (long long)expected[y][x]);
                return 0;
            }
        }
    }
    return 1;
}

static int test_97_single_sample_phases_and_range(void)
{
    static const double low[] = {7.0};
    static const double high[] = {-3.0};
    static const double too_large[] = {DBL_MAX};
    double output[2] = {41.0, 42.0};

    if (qw_jp2_dwt97_inverse_1d(
            low, 1u, 1u, NULL, 0u, 0u, 0u, output, 1u) !=
            QW_JP2_DWT_OK || output[0] != 7.0) {
        fputs("9/7 even-phase singleton mismatch\n", stderr);
        return 0;
    }
    if (qw_jp2_dwt97_inverse_1d(
            NULL, 0u, 0u, high, 1u, 1u, 1u, output, 1u) !=
            QW_JP2_DWT_OK || output[0] != -1.5) {
        fputs("9/7 odd-phase singleton mismatch\n", stderr);
        return 0;
    }
    output[0] = 41.0;
    output[1] = 42.0;
    if (qw_jp2_dwt97_inverse_1d(
            too_large, 1u, 1u, NULL, 0u, 0u, 0u, output, 1u) !=
            QW_JP2_DWT_ERR_RANGE || output[0] != 41.0 ||
        output[1] != 42.0) {
        fputs("9/7 range failure changed output\n", stderr);
        return 0;
    }
    return 1;
}

static int test_97_inverse_odd_phase(void)
{
    static const double low[] = {
        18.633227448778815, 37.73354510244212
    };
    static const double high[] = {
        -8.650870524570085, -0.6745647377150658
    };
    static const double expected[] = {10.0, 20.0, 30.0, 40.0};
    double output[4] = {0.0};

    if (qw_jp2_dwt97_inverse_1d(
            low, 2u, 1u, high, 2u, 1u, 1u, output, 4u) !=
            QW_JP2_DWT_OK) {
        fputs("9/7 odd-phase inverse rejected its coefficients\n", stderr);
        return 0;
    }
    for (size_t i = 0u; i < 4u; ++i) {
        double error = output[i] - expected[i];
        if (error < 0.0) error = -error;
        if (error > 1e-11) {
            fprintf(stderr,
                    "9/7 odd-phase inverse mismatch at %zu: got %.12f expected %.12f\n",
                    i, output[i], expected[i]);
            return 0;
        }
    }
    return 1;
}

static int test_t800_annex_j_97_reference(void)
{
    /*
     * Tables J.4-J.10 in ITU-T T.800, Annex J.4.1. Those tables round the
     * transform coefficients to the nearest integer before publication.
     */
    static const double level2_ll[5][4] = {
        {1, 4, 8, 11}, {4, 5, 8, 11}, {8, 9, 11, 13},
        {12, 12, 14, 16}, {15, 15, 17, 18}
    };
    static const double level2_hh[4][3] = {
        {-1, 0, 0}, {0, 1, 0}, {0, 0, 0}, {0, 0, 0}
    };
    static const double level1_hl[9][6] = {
        {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0}, {0, -1, 0, 0, 0, 0},
        {0, 0, 0, -1, 0, 0}, {0, 0, 1, 1, 0, -1},
        {0, 0, 0, 0, 0, 0}, {0, 0, -1, -1, -1, 0},
        {0, 0, 0, 0, 0, 0}
    };
    static const double level1_lh[8][7] = {
        {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, -1, 0, 0, 0},
        {0, 0, 0, 0, 0, 1, 1}, {0, 0, 0, 0, -1, 1, 0},
        {0, 0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0}
    };
    static const double level1_hh[8][6] = {
        {-1, 0, 0, 0, 0, 0}, {0, 0, -1, 0, 0, 0},
        {0, -1, 1, 0, 0, 0}, {0, 0, 0, 0, 0, 1},
        {0, 0, 0, 0, -1, 0}, {0, 0, 0, 0, 0, 0},
        {0, 0, -1, 0, -1, 1}, {0, 0, 0, 0, -1, 0}
    };
    double coefficients[17][13] = {{0}};
    double scratch[17];
    double maximum_error = 0.0;
    double total_error = 0.0;

    for (size_t y = 0u; y < 5u; ++y) {
        for (size_t x = 0u; x < 4u; ++x) {
            coefficients[y][x] = level2_ll[y][x];
        }
    }
    for (size_t y = 0u; y < 4u; ++y) {
        for (size_t x = 0u; x < 3u; ++x) {
            coefficients[y + 5u][x + 4u] = level2_hh[y][x];
        }
    }
    for (size_t y = 0u; y < 9u; ++y) {
        for (size_t x = 0u; x < 6u; ++x) {
            coefficients[y][x + 7u] = level1_hl[y][x];
        }
    }
    for (size_t y = 0u; y < 8u; ++y) {
        for (size_t x = 0u; x < 7u; ++x) {
            coefficients[y + 9u][x] = level1_lh[y][x];
        }
        for (size_t x = 0u; x < 6u; ++x) {
            coefficients[y + 9u][x + 7u] = level1_hh[y][x];
        }
    }

    if (qw_jp2_dwt97_inverse_level(
            &coefficients[0][0], 13u, 7u, 9u, 0u, 0u,
            scratch, 17u) != QW_JP2_DWT_OK ||
        qw_jp2_dwt97_inverse_level(
            &coefficients[0][0], 13u, 13u, 17u, 0u, 0u,
            scratch, 17u) != QW_JP2_DWT_OK) {
        fputs("T.800 9/7 inverse transform rejected its reference plane\n",
              stderr);
        return 0;
    }

    for (size_t y = 0u; y < 17u; ++y) {
        for (size_t x = 0u; x < 13u; ++x) {
            double expected = (double)(x > y ? x : y);
            double error = coefficients[y][x] - expected;
            if (error < 0.0) error = -error;
            if (error > maximum_error) maximum_error = error;
            total_error += error;
        }
    }
    if (maximum_error >= 5.0 || total_error / (17.0 * 13.0) >= 1.1) {
        fprintf(stderr,
                "T.800 9/7 rounded reference error too large: max %.6f, mean %.6f\n",
                maximum_error, total_error / (17.0 * 13.0));
        return 0;
    }
    printf("T.800 Annex J 9/7 rounded reference: max error %.6f, mean %.6f\n",
           maximum_error, total_error / (17.0 * 13.0));
    return 1;
}

int main(void)
{
    if (!test_inverse_1d_even_phase() ||
        !test_inverse_1d_odd_phase() ||
        !test_single_sample_phases() ||
        !test_rejects_bad_ranges_transactionally() ||
        !test_rejects_unrepresentable_spans_transactionally() ||
        !test_inverse_level_keeps_odd_phase_bands_in_fixed_quadrants() ||
        !test_t800_annex_j_53_reference() ||
        !test_97_single_sample_phases_and_range() ||
        !test_97_inverse_odd_phase() ||
        !test_t800_annex_j_97_reference()) {
        return 1;
    }
    return 0;
}
