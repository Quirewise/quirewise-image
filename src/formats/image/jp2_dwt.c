#include "jp2_dwt.h"

#include <float.h>
#include <math.h>
#include <string.h>

#define QW_JP2_DWT53_MAX_1D_INPUT (INT64_MAX / INT64_C(16))
#define QW_JP2_DWT53_MAX_LEVEL_INPUT (INT64_MAX / INT64_C(128))

static int qw_jp2_dwt53_values_span_valid(const int64_t *values,
                                          size_t count,
                                          size_t stride)
{
    if (count == 0u) return 1;
    if (!values || stride == 0u) {
        return 0;
    }
    return count - 1u <= (SIZE_MAX / sizeof(*values) - 1u) / stride;
}

static int qw_jp2_dwt53_values_in_range(const int64_t *values,
                                        size_t count,
                                        size_t stride,
                                        int64_t maximum_absolute_value)
{
    if (count == 0u) return 1;
    for (size_t i = 0u; i < count; ++i) {
        int64_t value = values[i * stride];
        if (value < -maximum_absolute_value ||
            value > maximum_absolute_value) {
            return 0;
        }
    }
    return 1;
}

static int64_t qw_jp2_dwt53_floor_shift(int64_t value, unsigned shift)
{
    int64_t divisor = INT64_C(1) << shift;
    if (value >= 0) return value / divisor;
    return -((-value + divisor - 1) / divisor);
}

static size_t qw_jp2_dwt53_low_count(size_t count, uint8_t first_is_high)
{
    return count / 2u + (!first_is_high && count % 2u != 0u ? 1u : 0u);
}

static size_t qw_jp2_dwt53_high_count(size_t count, uint8_t first_is_high)
{
    return count - qw_jp2_dwt53_low_count(count, first_is_high);
}

int qw_jp2_dwt53_inverse_1d(const int64_t *low,
                            size_t low_count,
                            size_t low_stride,
                            const int64_t *high,
                            size_t high_count,
                            size_t high_stride,
                            uint8_t first_is_high,
                            int64_t *output,
                            size_t output_capacity)
{
    size_t output_count;

    if (!output || first_is_high > 1u ||
        low_count > SIZE_MAX - high_count) {
        return QW_JP2_DWT_ERR_ARGUMENT;
    }
    output_count = low_count + high_count;
    if (output_count == 0u || output_capacity < output_count ||
        output_count > SIZE_MAX / sizeof(*output) ||
        low_count != qw_jp2_dwt53_low_count(output_count, first_is_high) ||
        high_count != qw_jp2_dwt53_high_count(output_count, first_is_high)) {
        return QW_JP2_DWT_ERR_ARGUMENT;
    }
    if (!qw_jp2_dwt53_values_span_valid(low, low_count, low_stride) ||
        !qw_jp2_dwt53_values_span_valid(high, high_count, high_stride)) {
        return QW_JP2_DWT_ERR_ARGUMENT;
    }
    if (!qw_jp2_dwt53_values_in_range(low, low_count, low_stride,
                                      QW_JP2_DWT53_MAX_1D_INPUT) ||
        !qw_jp2_dwt53_values_in_range(high, high_count, high_stride,
                                      QW_JP2_DWT53_MAX_1D_INPUT)) {
        return QW_JP2_DWT_ERR_RANGE;
    }

    if (first_is_high && low_count == 0u) {
        output[0] = high[0] / 2;
        return QW_JP2_DWT_OK;
    }
    if (!first_is_high && high_count == 0u) {
        output[0] = low[0];
        return QW_JP2_DWT_OK;
    }

    if (!first_is_high) {
        for (size_t i = 0u; i < low_count; ++i) {
            size_t left_index = i == 0u ? 0u : i - 1u;
            size_t right_index = i < high_count ? i : high_count - 1u;
            int64_t left = high[left_index * high_stride];
            int64_t right = high[right_index * high_stride];
            int64_t correction =
                qw_jp2_dwt53_floor_shift(left + right + 2, 2u);
            output[2u * i] = low[i * low_stride] - correction;
        }
        for (size_t i = 0u; i < high_count; ++i) {
            size_t right_index = i + 1u < low_count ? i + 1u : i;
            int64_t left = output[2u * i];
            int64_t right = output[2u * right_index];
            output[2u * i + 1u] = high[i * high_stride] +
                qw_jp2_dwt53_floor_shift(left + right, 1u);
        }
    } else {
        for (size_t i = 0u; i < low_count; ++i) {
            size_t right_index = i + 1u < high_count ? i + 1u : i;
            int64_t left = high[i * high_stride];
            int64_t right = high[right_index * high_stride];
            int64_t correction =
                qw_jp2_dwt53_floor_shift(left + right + 2, 2u);
            output[2u * i + 1u] = low[i * low_stride] - correction;
        }
        for (size_t i = 0u; i < high_count; ++i) {
            size_t left_index = i == 0u ? 0u : i - 1u;
            size_t right_index = i < low_count ? i : low_count - 1u;
            int64_t left = output[2u * left_index + 1u];
            int64_t right = output[2u * right_index + 1u];
            output[2u * i] = high[i * high_stride] +
                qw_jp2_dwt53_floor_shift(left + right, 1u);
        }
    }
    return QW_JP2_DWT_OK;
}

static int qw_jp2_dwt53_plane_span_valid(size_t stride,
                                         size_t width,
                                         size_t height)
{
    size_t max_elements = SIZE_MAX / sizeof(int64_t);

    if (width == 0u || height == 0u || stride < width ||
        width > max_elements ||
        height - 1u > (max_elements - width) / stride) {
        return 0;
    }
    return 1;
}

static int qw_jp2_dwt53_plane_in_range(const int64_t *coefficients,
                                       size_t stride,
                                       size_t width,
                                       size_t height)
{
    for (size_t y = 0u; y < height; ++y) {
        for (size_t x = 0u; x < width; ++x) {
            int64_t value = coefficients[y * stride + x];
            if (value < -QW_JP2_DWT53_MAX_LEVEL_INPUT ||
                value > QW_JP2_DWT53_MAX_LEVEL_INPUT) {
                return 0;
            }
        }
    }
    return 1;
}

int qw_jp2_dwt53_inverse_level(int64_t *coefficients,
                               size_t stride,
                               size_t width,
                               size_t height,
                               uint8_t horizontal_first_is_high,
                               uint8_t vertical_first_is_high,
                               int64_t *scratch,
                               size_t scratch_capacity)
{
    size_t low_width;
    size_t high_width;
    size_t low_height;
    size_t high_height;
    size_t required_scratch;
    size_t low_x_offset;
    size_t high_x_offset;
    size_t low_y_offset;
    size_t high_y_offset;

    if (!coefficients || !scratch || width == 0u || height == 0u ||
        stride < width || horizontal_first_is_high > 1u ||
        vertical_first_is_high > 1u ||
        !qw_jp2_dwt53_plane_span_valid(stride, width, height)) {
        return QW_JP2_DWT_ERR_ARGUMENT;
    }
    required_scratch = width > height ? width : height;
    if (scratch_capacity < required_scratch ||
        required_scratch > SIZE_MAX / sizeof(*scratch)) {
        return QW_JP2_DWT_ERR_ARGUMENT;
    }
    if (!qw_jp2_dwt53_plane_in_range(coefficients, stride, width, height)) {
        return QW_JP2_DWT_ERR_RANGE;
    }

    low_width = qw_jp2_dwt53_low_count(width, horizontal_first_is_high);
    high_width = width - low_width;
    low_height = qw_jp2_dwt53_low_count(height, vertical_first_is_high);
    high_height = height - low_height;
    /* Packed JPEG 2000 planes keep low and high bands in fixed quadrants.
     * The phase flags affect output sample interleaving, not band placement. */
    low_x_offset = 0u;
    high_x_offset = low_width;
    low_y_offset = 0u;
    high_y_offset = low_height;

    for (size_t y = 0u; y < height; ++y) {
        int64_t *row = coefficients + y * stride;
        const int64_t *low = low_width ? row + low_x_offset : NULL;
        const int64_t *high = high_width ? row + high_x_offset : NULL;
        int rc = qw_jp2_dwt53_inverse_1d(
            low, low_width, 1u, high, high_width, 1u,
            horizontal_first_is_high, scratch, scratch_capacity);
        if (rc != QW_JP2_DWT_OK) return rc;
        memcpy(row, scratch, width * sizeof(*scratch));
    }

    for (size_t x = 0u; x < width; ++x) {
        const int64_t *low = low_height
            ? coefficients + low_y_offset * stride + x : NULL;
        const int64_t *high = high_height
            ? coefficients + high_y_offset * stride + x : NULL;
        int rc = qw_jp2_dwt53_inverse_1d(
            low, low_height, stride, high, high_height, stride,
            vertical_first_is_high, scratch, scratch_capacity);
        if (rc != QW_JP2_DWT_OK) return rc;
        for (size_t y = 0u; y < height; ++y) {
            coefficients[y * stride + x] = scratch[y];
        }
    }
    return QW_JP2_DWT_OK;
}

#define QW_JP2_DWT97_MAX_1D_INPUT (DBL_MAX / 64.0)
#define QW_JP2_DWT97_MAX_LEVEL_INPUT (DBL_MAX / 4096.0)

static int qw_jp2_dwt97_values_span_valid(const double *values,
                                          size_t count,
                                          size_t stride)
{
    if (count == 0u) return 1;
    if (!values || stride == 0u) return 0;
    return count - 1u <= (SIZE_MAX / sizeof(*values) - 1u) / stride;
}

static int qw_jp2_dwt97_values_in_range(const double *values,
                                        size_t count,
                                        size_t stride,
                                        double maximum_absolute_value)
{
    if (count == 0u) return 1;
    for (size_t i = 0u; i < count; ++i) {
        double value = values[i * stride];
        if (!isfinite(value) || value < -maximum_absolute_value ||
            value > maximum_absolute_value) {
            return 0;
        }
    }
    return 1;
}

static size_t qw_jp2_dwt97_low_count(size_t count, uint8_t first_is_high)
{
    return count / 2u +
           (!first_is_high && count % 2u != 0u ? 1u : 0u);
}

static size_t qw_jp2_dwt97_high_count(size_t count, uint8_t first_is_high)
{
    return count - qw_jp2_dwt97_low_count(count, first_is_high);
}

int qw_jp2_dwt97_inverse_1d(const double *low,
                            size_t low_count,
                            size_t low_stride,
                            const double *high,
                            size_t high_count,
                            size_t high_stride,
                            uint8_t first_is_high,
                            double *output,
                            size_t output_capacity)
{
    static const double alpha = -1.586134342059924;
    static const double beta = -0.052980118572961;
    static const double gamma = 0.882911075530934;
    static const double delta = 0.443506852043971;
    static const double scale = 1.230174104914001;
    size_t output_count;

    if (!output || first_is_high > 1u || low_count > SIZE_MAX - high_count) {
        return QW_JP2_DWT_ERR_ARGUMENT;
    }
    output_count = low_count + high_count;
    if (output_count == 0u || output_capacity < output_count ||
        output_count > SIZE_MAX / sizeof(*output) ||
        low_count != qw_jp2_dwt97_low_count(output_count, first_is_high) ||
        high_count != qw_jp2_dwt97_high_count(output_count, first_is_high)) {
        return QW_JP2_DWT_ERR_ARGUMENT;
    }
    if (!qw_jp2_dwt97_values_span_valid(low, low_count, low_stride) ||
        !qw_jp2_dwt97_values_span_valid(high, high_count, high_stride)) {
        return QW_JP2_DWT_ERR_ARGUMENT;
    }
    if (!qw_jp2_dwt97_values_in_range(
            low, low_count, low_stride, QW_JP2_DWT97_MAX_1D_INPUT) ||
        !qw_jp2_dwt97_values_in_range(
            high, high_count, high_stride, QW_JP2_DWT97_MAX_1D_INPUT)) {
        return QW_JP2_DWT_ERR_RANGE;
    }

    if (first_is_high && low_count == 0u) {
        output[0] = high[0] / 2.0;
        return QW_JP2_DWT_OK;
    }
    if (!first_is_high && high_count == 0u) {
        output[0] = low[0];
        return QW_JP2_DWT_OK;
    }

    if (!first_is_high) {
        for (size_t i = 0u; i < low_count; ++i) {
            output[2u * i] = low[i * low_stride] * scale;
        }
        for (size_t i = 0u; i < high_count; ++i) {
            output[2u * i + 1u] = high[i * high_stride] / scale;
        }

        for (size_t i = 0u; i < low_count; ++i) {
            size_t left = i == 0u ? 0u : i - 1u;
            size_t right = i < high_count ? i : high_count - 1u;
            output[2u * i] -= delta *
                (output[2u * left + 1u] + output[2u * right + 1u]);
        }
        for (size_t i = 0u; i < high_count; ++i) {
            size_t right = i + 1u < low_count ? i + 1u : i;
            output[2u * i + 1u] -= gamma *
                (output[2u * i] + output[2u * right]);
        }
        for (size_t i = 0u; i < low_count; ++i) {
            size_t left = i == 0u ? 0u : i - 1u;
            size_t right = i < high_count ? i : high_count - 1u;
            output[2u * i] -= beta *
                (output[2u * left + 1u] + output[2u * right + 1u]);
        }
        for (size_t i = 0u; i < high_count; ++i) {
            size_t right = i + 1u < low_count ? i + 1u : i;
            output[2u * i + 1u] -= alpha *
                (output[2u * i] + output[2u * right]);
        }
    } else {
        for (size_t i = 0u; i < high_count; ++i) {
            output[2u * i] = high[i * high_stride] / scale;
        }
        for (size_t i = 0u; i < low_count; ++i) {
            output[2u * i + 1u] = low[i * low_stride] * scale;
        }

        for (size_t i = 0u; i < low_count; ++i) {
            size_t right = i + 1u < high_count ? i + 1u : i;
            output[2u * i + 1u] -= delta *
                (output[2u * i] + output[2u * right]);
        }
        for (size_t i = 0u; i < high_count; ++i) {
            size_t left = i == 0u ? 0u : i - 1u;
            size_t right = i < low_count ? i : low_count - 1u;
            output[2u * i] -= gamma *
                (output[2u * left + 1u] + output[2u * right + 1u]);
        }
        for (size_t i = 0u; i < low_count; ++i) {
            size_t right = i + 1u < high_count ? i + 1u : i;
            output[2u * i + 1u] -= beta *
                (output[2u * i] + output[2u * right]);
        }
        for (size_t i = 0u; i < high_count; ++i) {
            size_t left = i == 0u ? 0u : i - 1u;
            size_t right = i < low_count ? i : low_count - 1u;
            output[2u * i] -= alpha *
                (output[2u * left + 1u] + output[2u * right + 1u]);
        }
    }

    for (size_t i = 0u; i < output_count; ++i) {
        if (!isfinite(output[i])) return QW_JP2_DWT_ERR_RANGE;
    }
    return QW_JP2_DWT_OK;
}

static int qw_jp2_dwt97_plane_span_valid(size_t stride,
                                         size_t width,
                                         size_t height)
{
    size_t max_elements = SIZE_MAX / sizeof(double);

    if (width == 0u || height == 0u || stride < width ||
        width > max_elements ||
        height - 1u > (max_elements - width) / stride) {
        return 0;
    }
    return 1;
}

static int qw_jp2_dwt97_plane_in_range(const double *coefficients,
                                       size_t stride,
                                       size_t width,
                                       size_t height)
{
    for (size_t y = 0u; y < height; ++y) {
        if (!qw_jp2_dwt97_values_in_range(
                coefficients + y * stride, width, 1u,
                QW_JP2_DWT97_MAX_LEVEL_INPUT)) {
            return 0;
        }
    }
    return 1;
}

int qw_jp2_dwt97_inverse_level(double *coefficients,
                               size_t stride,
                               size_t width,
                               size_t height,
                               uint8_t horizontal_first_is_high,
                               uint8_t vertical_first_is_high,
                               double *scratch,
                               size_t scratch_capacity)
{
    size_t low_width;
    size_t high_width;
    size_t low_height;
    size_t high_height;
    size_t required_scratch;

    if (!coefficients || !scratch || width == 0u || height == 0u ||
        stride < width || horizontal_first_is_high > 1u ||
        vertical_first_is_high > 1u ||
        !qw_jp2_dwt97_plane_span_valid(stride, width, height)) {
        return QW_JP2_DWT_ERR_ARGUMENT;
    }
    required_scratch = width > height ? width : height;
    if (scratch_capacity < required_scratch ||
        required_scratch > SIZE_MAX / sizeof(*scratch)) {
        return QW_JP2_DWT_ERR_ARGUMENT;
    }
    if (!qw_jp2_dwt97_plane_in_range(coefficients, stride, width, height)) {
        return QW_JP2_DWT_ERR_RANGE;
    }

    low_width = qw_jp2_dwt97_low_count(width, horizontal_first_is_high);
    high_width = width - low_width;
    low_height = qw_jp2_dwt97_low_count(height, vertical_first_is_high);
    high_height = height - low_height;

    for (size_t y = 0u; y < height; ++y) {
        double *row = coefficients + y * stride;
        int rc = qw_jp2_dwt97_inverse_1d(
            low_width ? row : NULL, low_width, 1u,
            high_width ? row + low_width : NULL, high_width, 1u,
            horizontal_first_is_high, scratch, scratch_capacity);
        if (rc != QW_JP2_DWT_OK) return rc;
        memcpy(row, scratch, width * sizeof(*scratch));
    }

    for (size_t x = 0u; x < width; ++x) {
        const double *low = low_height ? coefficients + x : NULL;
        const double *high = high_height
            ? coefficients + low_height * stride + x : NULL;
        int rc = qw_jp2_dwt97_inverse_1d(
            low, low_height, stride, high, high_height, stride,
            vertical_first_is_high, scratch, scratch_capacity);
        if (rc != QW_JP2_DWT_OK) return rc;
        for (size_t y = 0u; y < height; ++y) {
            coefficients[y * stride + x] = scratch[y];
        }
    }
    return QW_JP2_DWT_OK;
}
