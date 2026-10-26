#include "scan_advanced_geometry.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct QwScanPoint {
    double x;
    double y;
} QwScanPoint;

typedef struct QwScanLine {
    double slope;
    double offset;
    double rms;
    size_t points;
    int horizontal;
    int valid;
} QwScanLine;

typedef struct QwScanCorner {
    double x;
    double y;
} QwScanCorner;

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}



static unsigned histogram_threshold(const uint8_t *pixels, size_t count)
{
    uint64_t histogram[256] = {0};
    uint64_t total = 0;
    uint64_t sum = 0;
    uint64_t left_count = 0;
    uint64_t left_sum = 0;
    double best = -1.0;
    unsigned threshold = 1;
    unsigned i;
    size_t p;
    for (p = 0; p < count; ++p) {
        ++histogram[pixels[p]];
        sum += pixels[p];
    }
    total = count;
    for (i = 0; i < 255u; ++i) {
        uint64_t right_count;
        double delta;
        double score;
        left_count += histogram[i];
        left_sum += (uint64_t)i * histogram[i];
        right_count = total - left_count;
        if (!left_count || !right_count) continue;
        delta = (double)left_sum / left_count -
                (double)(sum - left_sum) / right_count;
        score = (double)left_count * right_count * delta * delta;
        if (score > best) {
            best = score;
            threshold = i + 1u;
        }
    }
    if (threshold < 32u) threshold = 32u;
    if (threshold > 240u) threshold = 240u;
    return threshold;
}

static void fit_line_once(const QwScanPoint *points,
                          const uint8_t *active,
                          size_t count,
                          int horizontal,
                          QwScanLine *line)
{
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    double denominator;
    size_t used = 0;
    size_t i;
    for (i = 0; i < count; ++i) {
        double x;
        double y;
        if (!active[i]) continue;
        x = horizontal ? points[i].x : points[i].y;
        y = horizontal ? points[i].y : points[i].x;
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
        ++used;
    }
    line->valid = 0;
    line->points = used;
    line->horizontal = horizontal;
    if (used < 4u) return;
    denominator = (double)used * sum_xx - sum_x * sum_x;
    if (fabs(denominator) < 1e-9) return;
    line->slope = ((double)used * sum_xy - sum_x * sum_y) / denominator;
    line->offset = (sum_y - line->slope * sum_x) / (double)used;
    line->rms = 0.0;
    for (i = 0; i < count; ++i) {
        double x;
        double y;
        double error;
        if (!active[i]) continue;
        x = horizontal ? points[i].x : points[i].y;
        y = horizontal ? points[i].y : points[i].x;
        error = y - (line->slope * x + line->offset);
        line->rms += error * error;
    }
    line->rms = sqrt(line->rms / (double)used);
    line->valid = 1;
}

static QwScanLine fit_line_segment(const QwScanPoint *points,
                                   size_t count,
                                   int horizontal)
{
    QwScanLine line;
    uint8_t *active;
    double *residuals;
    size_t i;
    int iteration;
    memset(&line, 0, sizeof(line));
    if (count < 6u || count > SIZE_MAX / sizeof(*residuals)) return line;
    active = (uint8_t *)malloc(count);
    residuals = (double *)malloc(count * sizeof(*residuals));
    if (!active || !residuals) {
        free(active);
        free(residuals);
        return line;
    }
    memset(active, 1, count);
    for (iteration = 0; iteration < 4; ++iteration) {
        double threshold;
        fit_line_once(points, active, count, horizontal, &line);
        if (!line.valid) break;
        {
            size_t used = 0;
            for (i = 0; i < count; ++i) {
                if (active[i]) {
                    double x = horizontal ? points[i].x : points[i].y;
                    double y = horizontal ? points[i].y : points[i].x;
                    residuals[used++] = fabs(y -
                        (line.slope * x + line.offset));
                }
            }
            if (used < 6u) break;
            /* A bounded trim is deliberately conservative: ignore the largest
             * 15 percent of edge residuals, then refit the straight segment. */
            qsort(residuals, used, sizeof(*residuals), compare_double);
            threshold = residuals[(used * 85u) / 100u];
            if (threshold < 1.0) threshold = 1.0;
            for (i = 0; i < count; ++i) {
                if (!active[i]) continue;
                {
                    double x = horizontal ? points[i].x : points[i].y;
                    double y = horizontal ? points[i].y : points[i].x;
                    if (fabs(y - (line.slope * x + line.offset)) > threshold) {
                        active[i] = 0;
                    }
                }
            }
        }
    }
    fit_line_once(points, active, count, horizontal, &line);
    free(active);
    free(residuals);
    return line;
}

static QwScanLine pick_contour_line(const QwScanPoint *points,
                                    size_t count,
                                    int horizontal)
{
    size_t half = count / 2u;
    QwScanLine first = fit_line_segment(points, half, horizontal);
    QwScanLine second = fit_line_segment(points + half, count - half, horizontal);
    if (!first.valid) return second;
    if (!second.valid) return first;
    return first.rms <= second.rms ? first : second;
}

static int intersect_contours(const QwScanLine *horizontal,
                              const QwScanLine *vertical,
                              QwScanCorner *out)
{
    double denominator = 1.0 - horizontal->slope * vertical->slope;
    if (fabs(denominator) < 1e-6) return 0;
    out->x = (vertical->slope * horizontal->offset + vertical->offset) /
             denominator;
    out->y = horizontal->slope * out->x + horizontal->offset;
    return isfinite(out->x) && isfinite(out->y);
}

static int detect_page_quad(const uint8_t *gray,
                            uint32_t width,
                            uint32_t height,
                            QwScanCorner quad[4])
{
    size_t pixel_count;
    QwScanPoint *top = NULL;
    QwScanPoint *bottom = NULL;
    QwScanPoint *left = NULL;
    QwScanPoint *right = NULL;
    size_t n_top = 0;
    size_t n_bottom = 0;
    size_t n_left = 0;
    size_t n_right = 0;
    unsigned threshold;
    uint32_t x;
    double area = 0.0;
    QwScanLine top_line;
    QwScanLine bottom_line;
    QwScanLine left_line;
    QwScanLine right_line;
    int result = 0;

    if (width < 32u || height < 32u || !qw_checked_size_mul(width, height, &pixel_count)) {
        return 0;
    }
    threshold = histogram_threshold(gray, pixel_count);
    {
        size_t point_bytes;
        if (!qw_checked_size_mul(width, sizeof(*top), &point_bytes)) return 0;
        top = (QwScanPoint *)malloc(point_bytes);
        bottom = (QwScanPoint *)malloc(point_bytes);
        if (!qw_checked_size_mul(height, sizeof(*left), &point_bytes)) goto cleanup;
        left = (QwScanPoint *)malloc(point_bytes);
        right = (QwScanPoint *)malloc(point_bytes);
    }
    if (!top || !bottom || !left || !right) goto cleanup;

    for (x = 0; x < width; ++x) {
        uint32_t y;
        uint32_t first = height;
        uint32_t last = 0;
        for (y = 0; y < height; ++y) {
            if (gray[(size_t)y * width + x] >= threshold) {
                if (first == height) first = y;
                last = y;
            }
        }
        if (first != height) {
            top[n_top++] = (QwScanPoint){(double)x, (double)first};
            bottom[n_bottom++] = (QwScanPoint){(double)x, (double)last};
        }
    }
    for (x = 0; x < height; ++x) {
        uint32_t xx;
        uint32_t first = width;
        uint32_t last = 0;
        for (xx = 0; xx < width; ++xx) {
            if (gray[(size_t)x * width + xx] >= threshold) {
                if (first == width) first = xx;
                last = xx;
            }
        }
        if (first != width) {
            left[n_left++] = (QwScanPoint){(double)first, (double)x};
            right[n_right++] = (QwScanPoint){(double)last, (double)x};
        }
    }
    if (n_top < 16u || n_bottom < 16u || n_left < 16u || n_right < 16u) {
        goto cleanup;
    }
    top_line = pick_contour_line(top, n_top, 1);
    bottom_line = pick_contour_line(bottom, n_bottom, 1);
    left_line = pick_contour_line(left, n_left, 0);
    right_line = pick_contour_line(right, n_right, 0);
    if (!top_line.valid || !bottom_line.valid ||
        !left_line.valid || !right_line.valid) goto cleanup;
    if (top_line.rms > 0.02 * fmax(width, height) ||
        bottom_line.rms > 0.02 * fmax(width, height) ||
        left_line.rms > 0.02 * fmax(width, height) ||
        right_line.rms > 0.02 * fmax(width, height)) goto cleanup;
    if (!intersect_contours(&top_line, &left_line, &quad[0]) ||
        !intersect_contours(&top_line, &right_line, &quad[1]) ||
        !intersect_contours(&bottom_line, &right_line, &quad[2]) ||
        !intersect_contours(&bottom_line, &left_line, &quad[3])) goto cleanup;
    {
        int i;
        const double max_x = (double)width - 1.0;
        const double max_y = (double)height - 1.0;
        for (i = 0; i < 4; ++i) {
            if (quad[i].x < -2.0 || quad[i].y < -2.0 ||
                quad[i].x > max_x + 2.0 || quad[i].y > max_y + 2.0) {
                goto cleanup;
            }
            if (quad[i].x < 0.0) quad[i].x = 0.0;
            if (quad[i].y < 0.0) quad[i].y = 0.0;
            if (quad[i].x > max_x) quad[i].x = max_x;
            if (quad[i].y > max_y) quad[i].y = max_y;
        }
        for (i = 0; i < 4; ++i) {
            int next = (i + 1) & 3;
            area += quad[i].x * quad[next].y - quad[next].x * quad[i].y;
        }
        area = fabs(area) * 0.5;
        if (area < 0.30 * (double)width * height) goto cleanup;
        if (fabs(quad[0].x) < 3.0 && fabs(quad[0].y) < 3.0 &&
            fabs(quad[1].x - max_x) < 3.0 && fabs(quad[1].y) < 3.0 &&
            fabs(quad[2].x - max_x) < 3.0 && fabs(quad[2].y - max_y) < 3.0 &&
            fabs(quad[3].x) < 3.0 && fabs(quad[3].y - max_y) < 3.0) {
            goto cleanup;
        }
        /* Avoid resampling a merely axis-aligned crop box. */
        if (fabs(top_line.slope) < 0.003 && fabs(bottom_line.slope) < 0.003 &&
            fabs(left_line.slope) < 0.003 && fabs(right_line.slope) < 0.003) {
            double top_width = hypot(quad[1].x - quad[0].x,
                                     quad[1].y - quad[0].y);
            double bottom_width = hypot(quad[2].x - quad[3].x,
                                        quad[2].y - quad[3].y);
            double denominator = fmax(top_width, bottom_width);
            if (denominator <= 0.0 ||
                fabs(top_width - bottom_width) / denominator < 0.02) {
                goto cleanup;
            }
        }
    }
    result = 1;

cleanup:
    free(top);
    free(bottom);
    free(left);
    free(right);
    return result;
}

static int solve_homography(const QwScanCorner source[4],
                            uint32_t out_width,
                            uint32_t out_height,
                            double h[8])
{
    double matrix[8][9] = {{0}};
    double out_x[4] = {0.0, (double)out_width - 1.0,
                       (double)out_width - 1.0, 0.0};
    double out_y[4] = {0.0, 0.0,
                       (double)out_height - 1.0, (double)out_height - 1.0};
    int corner;
    int column;
    for (corner = 0; corner < 4; ++corner) {
        double x = out_x[corner];
        double y = out_y[corner];
        double u = source[corner].x;
        double v = source[corner].y;
        int row = corner * 2;
        matrix[row][0] = x;
        matrix[row][1] = y;
        matrix[row][2] = 1.0;
        matrix[row][6] = -u * x;
        matrix[row][7] = -u * y;
        matrix[row][8] = u;
        matrix[row + 1][3] = x;
        matrix[row + 1][4] = y;
        matrix[row + 1][5] = 1.0;
        matrix[row + 1][6] = -v * x;
        matrix[row + 1][7] = -v * y;
        matrix[row + 1][8] = v;
    }
    for (column = 0; column < 8; ++column) {
        int pivot = column;
        int row;
        double magnitude = fabs(matrix[pivot][column]);
        for (row = column + 1; row < 8; ++row) {
            double candidate = fabs(matrix[row][column]);
            if (candidate > magnitude) {
                magnitude = candidate;
                pivot = row;
            }
        }
        if (magnitude < 1e-10) return 0;
        if (pivot != column) {
            int j;
            for (j = column; j < 9; ++j) {
                double swap = matrix[column][j];
                matrix[column][j] = matrix[pivot][j];
                matrix[pivot][j] = swap;
            }
        }
        {
            double divisor = matrix[column][column];
            int j;
            for (j = column; j < 9; ++j) matrix[column][j] /= divisor;
        }
        for (row = 0; row < 8; ++row) {
            double factor;
            int j;
            if (row == column) continue;
            factor = matrix[row][column];
            for (j = column; j < 9; ++j) {
                matrix[row][j] -= factor * matrix[column][j];
            }
        }
    }
    for (column = 0; column < 8; ++column) h[column] = matrix[column][8];
    return 1;
}

static uint8_t sample_bilinear(const uint8_t *source,
                               uint32_t width,
                               uint32_t height,
                               double x,
                               double y)
{
    int x0;
    int y0;
    double fx;
    double fy;
    double value = 0.0;
    int ix;
    int iy;
    if (x < 0.0 || y < 0.0 || x > (double)width - 1.0 ||
        y > (double)height - 1.0) return 255u;
    x0 = (int)floor(x);
    y0 = (int)floor(y);
    fx = x - x0;
    fy = y - y0;
    for (iy = 0; iy <= 1; ++iy) {
        int sy = y0 + iy;
        double wy = iy ? fy : 1.0 - fy;
        if ((uint32_t)sy >= height) sy = (int)height - 1;
        for (ix = 0; ix <= 1; ++ix) {
            int sx = x0 + ix;
            double wx = ix ? fx : 1.0 - fx;
            if ((uint32_t)sx >= width) sx = (int)width - 1;
            value += source[(size_t)sy * width + (uint32_t)sx] * wx * wy;
        }
    }
    return (uint8_t)(value + 0.5);
}

int qw_scan_perspective_gray(uint8_t **pixels,
                             uint32_t *width,
                             uint32_t *height)
{
    QwScanCorner quad[4];
    double h[8];
    double top_width;
    double bottom_width;
    double left_height;
    double right_height;
    uint32_t out_width;
    uint32_t out_height;
    size_t out_count;
    uint8_t *out;
    const uint8_t *source;
    uint32_t y;
    if (!pixels || !*pixels || !width || !height) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (!detect_page_quad(*pixels, *width, *height, quad)) return QW_STATUS_OK;
    top_width = hypot(quad[1].x - quad[0].x, quad[1].y - quad[0].y);
    bottom_width = hypot(quad[2].x - quad[3].x, quad[2].y - quad[3].y);
    left_height = hypot(quad[3].x - quad[0].x, quad[3].y - quad[0].y);
    right_height = hypot(quad[2].x - quad[1].x, quad[2].y - quad[1].y);
    if (top_width < 8.0 || bottom_width < 8.0 ||
        left_height < 8.0 || right_height < 8.0) return QW_STATUS_OK;
    if ((top_width + bottom_width) * 0.5 > UINT32_MAX ||
        (left_height + right_height) * 0.5 > UINT32_MAX) return QW_STATUS_OK;
    out_width = (uint32_t)floor((top_width + bottom_width) * 0.5 + 0.5);
    out_height = (uint32_t)floor((left_height + right_height) * 0.5 + 0.5);
    if (!out_width || !out_height || (uint64_t)out_width > (uint64_t)*width * 2u ||
        (uint64_t)out_height > (uint64_t)*height * 2u ||
        !qw_checked_size_mul(out_width, out_height, &out_count)) return QW_STATUS_OK;
    if (!solve_homography(quad, out_width, out_height, h)) return QW_STATUS_OK;
    out = (uint8_t *)malloc(out_count);
    if (!out) return QW_STATUS_ERR_OUT_OF_MEMORY;
    source = *pixels;
    for (y = 0; y < out_height; ++y) {
        uint32_t x;
        for (x = 0; x < out_width; ++x) {
            double denominator = h[6] * x + h[7] * y + 1.0;
            if (fabs(denominator) < 1e-10) {
                out[(size_t)y * out_width + x] = 255u;
            } else {
                double sx = (h[0] * x + h[1] * y + h[2]) / denominator;
                double sy = (h[3] * x + h[4] * y + h[5]) / denominator;
                out[(size_t)y * out_width + x] =
                    sample_bilinear(source, *width, *height, sx, sy);
            }
        }
    }
    free(*pixels);
    *pixels = out;
    *width = out_width;
    *height = out_height;
    return QW_STATUS_OK;
}

static double profile_correlation(const uint16_t *reference,
                                  const uint16_t *candidate,
                                  uint32_t length,
                                  int shift)
{
    uint64_t sum_a = 0;
    uint64_t sum_b = 0;
    long double sum_aa = 0.0;
    long double sum_bb = 0.0;
    long double sum_ab = 0.0;
    uint32_t start_a = shift < 0 ? (uint32_t)(-shift) : 0u;
    uint32_t start_b = shift > 0 ? (uint32_t)shift : 0u;
    uint32_t count = length - (uint32_t)abs(shift);
    uint32_t i;
    if (count < length / 2u) return -1.0;
    for (i = 0; i < count; ++i) {
        sum_a += reference[start_a + i];
        sum_b += candidate[start_b + i];
        sum_aa += (long double)reference[start_a + i] * reference[start_a + i];
        sum_bb += (long double)candidate[start_b + i] * candidate[start_b + i];
        sum_ab += (long double)reference[start_a + i] * candidate[start_b + i];
    }
    {
        long double n = count;
        long double covariance = sum_ab - (long double)sum_a * sum_b / n;
        long double variance_a = sum_aa - (long double)sum_a * sum_a / n;
        long double variance_b = sum_bb - (long double)sum_b * sum_b / n;
        if (variance_a <= 1e-9 || variance_b <= 1e-9) return -1.0;
        return (double)(covariance / sqrtl(variance_a * variance_b));
    }
}

static int compare_int(const void *left, const void *right)
{
    int a = *(const int *)left;
    int b = *(const int *)right;
    return (a > b) - (a < b);
}

int qw_scan_dewarp_gray(uint8_t **pixels,
                        uint32_t width,
                        uint32_t height)
{
    const uint32_t band_width = 48u;
    uint32_t profile_step;
    uint32_t profile_height;
    uint32_t bands;
    uint32_t max_shift;
    size_t profile_count;
    size_t mass_bytes;
    size_t shift_bytes;
    uint16_t *profiles = NULL;
    uint64_t *masses = NULL;
    int *shifts = NULL;
    uint8_t *valid = NULL;
    uint8_t *out = NULL;
    size_t pixel_count;
    unsigned threshold;
    uint32_t band;
    uint32_t reference_band = 0;
    uint64_t max_mass = 0;
    double total_quality = 0.0;
    size_t valid_count = 0;
    int min_shift = 0;
    int max_shift_found = 0;
    int rc = QW_STATUS_OK;

    if (!pixels || !*pixels || !qw_checked_size_mul(width, height, &pixel_count)) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    (void)pixel_count;
    bands = (width - 1u) / band_width + 1u;
    if (bands < 5u) return QW_STATUS_OK;
    profile_step = height > 1400u ? (height + 1399u) / 1400u : 1u;
    profile_height = (height - 1u) / profile_step + 1u;
    max_shift = profile_height / 10u;
    if (max_shift > 48u) max_shift = 48u;
    if (max_shift < 2u) return QW_STATUS_OK;
    if ((size_t)bands > SIZE_MAX / profile_height ||
        !qw_checked_size_mul(bands, sizeof(*masses), &mass_bytes) ||
        !qw_checked_size_mul(bands, sizeof(*shifts), &shift_bytes)) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    profile_count = (size_t)bands * profile_height;
    if (profile_count > SIZE_MAX / sizeof(*profiles)) return QW_STATUS_ERR_UNSUPPORTED;
    profiles = (uint16_t *)calloc(profile_count, sizeof(*profiles));
    masses = (uint64_t *)calloc(1u, mass_bytes);
    shifts = (int *)calloc(1u, shift_bytes);
    valid = (uint8_t *)calloc(bands, 1u);
    if (!profiles || !masses || !shifts || !valid) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    threshold = histogram_threshold(*pixels, pixel_count);
    for (band = 0; band < bands; ++band) {
        uint32_t x0 = band * band_width;
        uint32_t x1 = x0 + band_width;
        uint32_t py;
        if (x1 > width) x1 = width;
        for (py = 0; py < profile_height; ++py) {
            uint32_t y = py * profile_step;
            uint32_t x;
            uint16_t ink = 0;
            if (y >= height) y = height - 1u;
            for (x = x0; x < x1; ++x) {
                if ((*pixels)[(size_t)y * width + x] < threshold) ++ink;
            }
            profiles[(size_t)band * profile_height + py] = ink;
            masses[band] += ink;
        }
        if (masses[band] > max_mass) {
            max_mass = masses[band];
            reference_band = band;
        }
    }
    if (max_mass < profile_height) goto cleanup;
    {
        const uint16_t *reference = profiles + (size_t)reference_band * profile_height;
        for (band = 0; band < bands; ++band) {
            const uint16_t *candidate = profiles + (size_t)band * profile_height;
            double best = -1.0;
            double zero = profile_correlation(reference, candidate,
                                              profile_height, 0);
            int best_shift = 0;
            int shift;
            if (masses[band] < max_mass / 10u) continue;
            for (shift = -(int)max_shift; shift <= (int)max_shift; ++shift) {
                double score = profile_correlation(reference, candidate,
                                                   profile_height, shift);
                if (score > best) {
                    best = score;
                    best_shift = shift;
                }
            }
            if (best < 0.18 || (best_shift != 0 && best < zero + 0.015)) continue;
            shifts[band] = best_shift * (int)profile_step;
            valid[band] = 1;
            total_quality += best;
            ++valid_count;
        }
    }
    if (valid_count < (size_t)bands * 3u / 5u ||
        total_quality / (double)valid_count < 0.30) goto cleanup;

    /* Smooth noisy stripe estimates, then remove the linear component so this
     * stage corrects curvature without undoing deskew. */
    {
        size_t band_int_bytes;
        int *smooth;
        double sum_x = 0.0;
        double sum_y = 0.0;
        double sum_xx = 0.0;
        double sum_xy = 0.0;
        double slope;
        double offset;
        size_t used = 0;
        int *ordered;
        if (!qw_checked_size_mul(bands, sizeof(*smooth), &band_int_bytes)) {
            rc = QW_STATUS_ERR_UNSUPPORTED;
            goto cleanup;
        }
        smooth = (int *)calloc(1u, band_int_bytes);
        if (!smooth) {
            rc = QW_STATUS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        ordered = (int *)malloc(band_int_bytes);
        if (!ordered) {
            free(smooth);
            rc = QW_STATUS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        for (band = 0; band < bands; ++band) {
            uint32_t j;
            size_t n = 0;
            for (j = band > 2u ? band - 2u : 0u;
                 j <= band + 2u && j < bands; ++j) {
                if (valid[j]) ordered[n++] = shifts[j];
            }
            if (n) {
                qsort(ordered, n, sizeof(*ordered), compare_int);
                smooth[band] = ordered[n / 2u];
            }
            if (valid[band]) {
                sum_x += band;
                sum_y += smooth[band];
                sum_xx += (double)band * band;
                sum_xy += (double)band * smooth[band];
                ++used;
            }
        }
        free(ordered);
        if (used < 3u) {
            free(smooth);
            goto cleanup;
        }
        {
            double denominator = (double)used * sum_xx - sum_x * sum_x;
            slope = fabs(denominator) > 1e-9
                        ? ((double)used * sum_xy - sum_x * sum_y) / denominator
                        : 0.0;
            offset = (sum_y - slope * sum_x) / (double)used;
        }
        for (band = 0; band < bands; ++band) {
            double curve = smooth[band] - (slope * band + offset);
            int correction = curve >= 0.0
                                 ? (int)floor(curve + 0.5)
                                 : (int)ceil(curve - 0.5);
            shifts[band] = correction;
            if (valid[band]) {
                if (correction < min_shift) min_shift = correction;
                if (correction > max_shift_found) max_shift_found = correction;
            }
        }
        free(smooth);
    }
    if (max_shift_found - min_shift < (int)(profile_step * 2u)) goto cleanup;
    out = (uint8_t *)malloc(pixel_count);
    if (!out) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (band = 0; band < width; ++band) {
        double band_position = ((double)band + 0.5) / band_width - 0.5;
        int left = (int)floor(band_position);
        double fraction = band_position - left;
        int right = left + 1;
        int left_shift;
        int right_shift;
        uint32_t y;
        if (left < 0) left = 0;
        if (right < 0) right = 0;
        if ((uint32_t)left >= bands) left = (int)bands - 1;
        if ((uint32_t)right >= bands) right = (int)bands - 1;
        left_shift = shifts[left];
        right_shift = shifts[right];
        for (y = 0; y < height; ++y) {
            double correction = left_shift * (1.0 - fraction) +
                                right_shift * fraction;
            double sy = (double)y + correction;
            uint8_t value = 255u;
            if (sy >= 0.0 && sy <= (double)height - 1.0) {
                uint32_t y0 = (uint32_t)floor(sy);
                uint32_t y1 = y0 + 1u < height ? y0 + 1u : y0;
                double fy = sy - y0;
                value = (uint8_t)((*pixels)[(size_t)y0 * width + band] *
                                  (1.0 - fy) +
                                  (*pixels)[(size_t)y1 * width + band] * fy + 0.5);
            }
            out[(size_t)y * width + band] = value;
        }
    }
    free(*pixels);
    *pixels = out;
    out = NULL;

cleanup:
    free(out);
    free(profiles);
    free(masses);
    free(shifts);
    free(valid);
    return rc;
}
