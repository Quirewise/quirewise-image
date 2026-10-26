#include "scan_geometry.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct QwScanPoint {
    uint16_t x;
    uint16_t y;
} QwScanPoint;

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}


static unsigned otsu_threshold(const uint8_t *pixels, size_t count)
{
    uint64_t histogram[256] = {0};
    uint64_t total_sum = 0;
    uint64_t background_sum = 0;
    uint64_t background_count = 0;
    double best = -1.0;
    unsigned threshold = 1;
    size_t i;
    unsigned value;

    for (i = 0; i < count; ++i) {
        ++histogram[pixels[i]];
        total_sum += pixels[i];
    }
    for (value = 0; value < 255; ++value) {
        uint64_t foreground_count;
        double difference;
        double score;
        background_count += histogram[value];
        background_sum += (uint64_t)value * histogram[value];
        foreground_count = (uint64_t)count - background_count;
        if (!background_count || !foreground_count) continue;
        difference = (double)background_sum / background_count -
                     (double)(total_sum - background_sum) / foreground_count;
        score = (double)background_count * foreground_count *
                difference * difference;
        if (score > best) {
            best = score;
            threshold = value + 1u;
        }
    }
    return threshold;
}

static double projection_score(const QwScanPoint *points,
                               size_t point_count,
                               uint32_t width,
                               uint32_t height,
                               double angle,
                               uint32_t *histogram,
                               size_t histogram_size)
{
    const double radians = angle * (3.14159265358979323846 / 180.0);
    const double sine = sin(radians);
    const double cosine = cos(radians);
    const double center_x = ((double)width - 1.0) * 0.5;
    const double center_y = ((double)height - 1.0) * 0.5;
    uint64_t score = 0;
    size_t i;

    memset(histogram, 0, histogram_size * sizeof(*histogram));
    for (i = 0; i < point_count; ++i) {
        double y = ((double)points[i].y - center_y) * cosine -
                   ((double)points[i].x - center_x) * sine + center_y;
        long bin = lround(y);
        if (bin < 0) bin = 0;
        if ((size_t)bin >= histogram_size) bin = (long)histogram_size - 1;
        ++histogram[bin];
    }
    for (i = 0; i < histogram_size; ++i) {
        score += (uint64_t)histogram[i] * histogram[i];
    }
    return (double)score;
}

static int rotate_gray(uint8_t **pixels,
                       uint32_t *width,
                       uint32_t *height,
                       double angle)
{
    const double radians = angle * (3.14159265358979323846 / 180.0);
    const double sine = sin(radians);
    const double cosine = cos(radians);
    const uint32_t source_width = *width;
    const uint32_t source_height = *height;
    const uint8_t *source = *pixels;
    uint32_t out_width;
    uint32_t out_height;
    size_t out_pixels;
    uint8_t *out;
    double source_cx = ((double)source_width - 1.0) * 0.5;
    double source_cy = ((double)source_height - 1.0) * 0.5;
    double output_cx;
    double output_cy;
    uint32_t y;

    {
        double calculated_width = ceil(source_width * cosine + source_height * fabs(sine));
        double calculated_height = ceil(source_height * cosine + source_width * fabs(sine));
        if (calculated_width > UINT32_MAX || calculated_height > UINT32_MAX) return 0;
        out_width = (uint32_t)calculated_width;
        out_height = (uint32_t)calculated_height;
    }
    if (out_width < source_width) out_width = source_width;
    if (out_height < source_height) out_height = source_height;
    if (!qw_checked_size_mul(out_width, out_height, &out_pixels)) return 0;
    out = (uint8_t *)malloc(out_pixels);
    if (!out) return 0;
    memset(out, 255, out_pixels);
    output_cx = ((double)out_width - 1.0) * 0.5;
    output_cy = ((double)out_height - 1.0) * 0.5;

    for (y = 0; y < out_height; ++y) {
        uint32_t x;
        uint8_t *row = out + (size_t)y * out_width;
        for (x = 0; x < out_width; ++x) {
            double dx = (double)x - output_cx;
            double dy = (double)y - output_cy;
            double sx = cosine * dx - sine * dy + source_cx;
            double sy = sine * dx + cosine * dy + source_cy;
            int x0 = (int)floor(sx);
            int y0 = (int)floor(sy);
            double fx = sx - x0;
            double fy = sy - y0;
            double value = 0.0;
            int ix;
            int iy;
            if (sx < 0.0 || sy < 0.0 ||
                sx > (double)source_width - 1.0 ||
                sy > (double)source_height - 1.0) {
                continue;
            }
            for (iy = 0; iy <= 1; ++iy) {
                int yy = y0 + iy;
                double wy = iy ? fy : 1.0 - fy;
                if (yy < 0) yy = 0;
                if ((uint32_t)yy >= source_height) yy = (int)source_height - 1;
                for (ix = 0; ix <= 1; ++ix) {
                    int xx = x0 + ix;
                    double wx = ix ? fx : 1.0 - fx;
                    if (xx < 0) xx = 0;
                    if ((uint32_t)xx >= source_width) xx = (int)source_width - 1;
                    value += source[(size_t)yy * source_width + (uint32_t)xx] * wx * wy;
                }
            }
            row[x] = (uint8_t)(value + 0.5);
        }
    }
    free(*pixels);
    *pixels = out;
    *width = out_width;
    *height = out_height;
    return 1;
}

int qw_scan_deskew_gray(uint8_t **pixels, uint32_t *width, uint32_t *height)
{
    const uint32_t max_dimension = 1200u;
    QwScanPoint *points = NULL;
    uint32_t *histogram = NULL;
    uint8_t *small = NULL;
    uint32_t small_width;
    uint32_t small_height;
    uint32_t step;
    size_t small_count;
    size_t point_capacity;
    size_t point_count = 0;
    size_t histogram_size;
    unsigned threshold;
    uint32_t y;
    double best_angle = 0.0;
    double best_score;
    double zero_score;
    double median_score = 0.0;
    double coarse_scores[41];
    int i;
    int applied;

    if (!pixels || !*pixels || !width || !height || !*width || !*height) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    step = (*width > max_dimension || *height > max_dimension)
               ? ((*width > *height ? *width : *height) + max_dimension - 1u) /
                     max_dimension
               : 1u;
    small_width = (*width - 1u) / step + 1u;
    small_height = (*height - 1u) / step + 1u;
    if (!qw_checked_size_mul(small_width, small_height, &small_count)) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    small = (uint8_t *)malloc(small_count);
    if (!small) return QW_STATUS_ERR_OUT_OF_MEMORY;
    for (y = 0; y < small_height; ++y) {
        uint32_t x;
        uint32_t sy = y * step;
        if (sy >= *height) sy = *height - 1u;
        for (x = 0; x < small_width; ++x) {
            uint32_t sx = x * step;
            if (sx >= *width) sx = *width - 1u;
            small[(size_t)y * small_width + x] = (*pixels)[(size_t)sy * *width + sx];
        }
    }
    threshold = otsu_threshold(small, small_count);
    point_capacity = small_count;
    if (point_capacity > SIZE_MAX / sizeof(*points)) {
        free(small);
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    points = (QwScanPoint *)malloc(point_capacity * sizeof(*points));
    if (!points) {
        free(small);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    for (y = 0; y < small_height; ++y) {
        uint32_t x;
        for (x = 0; x < small_width; ++x) {
            if (small[(size_t)y * small_width + x] < threshold) {
                points[point_count].x = (uint16_t)x;
                points[point_count].y = (uint16_t)y;
                ++point_count;
            }
        }
    }
    free(small);
    if (point_count < small_count / 1000u || point_count < 32u) {
        free(points);
        return QW_STATUS_OK;
    }

    histogram_size = (size_t)small_height + (size_t)small_width / 8u + 8u;
    if (histogram_size > SIZE_MAX / sizeof(*histogram)) {
        free(points);
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    histogram = (uint32_t *)malloc(histogram_size * sizeof(*histogram));
    if (!histogram) {
        free(points);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    best_score = projection_score(points, point_count, small_width, small_height,
                                  0.0, histogram, histogram_size);
    zero_score = best_score;
    for (i = -20; i <= 20; ++i) {
        double angle = (double)i * 0.25;
        double score = projection_score(points, point_count, small_width,
                                        small_height, angle, histogram,
                                        histogram_size);
        coarse_scores[i + 20] = score;
        if (score > best_score) {
            best_score = score;
            best_angle = angle;
        }
    }
    qsort(coarse_scores, 41u, sizeof(coarse_scores[0]), compare_double);
    median_score = coarse_scores[20];
    {
        double center = best_angle;
        int sample;
        for (sample = -5; sample <= 5; ++sample) {
            double angle = center + (double)sample * 0.05;
            double score = projection_score(points, point_count,
                                            small_width, small_height,
                                            angle, histogram,
                                            histogram_size);
            if (score > best_score) {
                best_score = score;
                best_angle = angle;
            }
        }
    }
    free(points);
    free(histogram);
    if (fabs(best_angle) < 0.08 || best_score < zero_score * 1.005 ||
        best_score < median_score * 1.02 || fabs(best_angle) > 5.0) {
        return QW_STATUS_OK;
    }
    applied = rotate_gray(pixels, width, height, best_angle);
    return applied ? QW_STATUS_OK : QW_STATUS_ERR_OUT_OF_MEMORY;
}

int qw_scan_crop_gray(uint8_t **pixels,
                      uint32_t *width,
                      uint32_t *height,
                      unsigned padding)
{
    size_t count;
    unsigned threshold;
    uint32_t min_x;
    uint32_t min_y;
    uint32_t max_x;
    uint32_t max_y;
    uint32_t roi_x0 = 0;
    uint32_t roi_y0 = 0;
    uint32_t roi_x1;
    uint32_t roi_y1;
    uint32_t y;
    uint8_t *cropped;
    uint32_t out_width;
    uint32_t out_height;
    size_t out_count;

    if (!pixels || !*pixels || !width || !height ||
        !qw_checked_size_mul(*width, *height, &count)) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    threshold = otsu_threshold(*pixels, count);
    roi_x1 = *width;
    roi_y1 = *height;
    {
        uint32_t limit = *height / 10u;
        uint32_t peeled = 0;
        while (roi_y0 + 1u < roi_y1 && peeled < limit) {
            uint64_t dark = 0;
            uint32_t x;
            for (x = roi_x0; x < roi_x1; ++x) {
                if ((*pixels)[(size_t)roi_y0 * *width + x] < threshold) ++dark;
            }
            if (dark * 10u < (uint64_t)(roi_x1 - roi_x0) * 9u) break;
            ++roi_y0;
            ++peeled;
        }
        peeled = 0;
        while (roi_y0 + 1u < roi_y1 && peeled < limit) {
            uint64_t dark = 0;
            uint32_t x;
            for (x = roi_x0; x < roi_x1; ++x) {
                if ((*pixels)[(size_t)(roi_y1 - 1u) * *width + x] < threshold) ++dark;
            }
            if (dark * 10u < (uint64_t)(roi_x1 - roi_x0) * 9u) break;
            --roi_y1;
            ++peeled;
        }
        limit = *width / 10u;
        peeled = 0;
        while (roi_x0 + 1u < roi_x1 && peeled < limit) {
            uint64_t dark = 0;
            uint32_t y;
            for (y = roi_y0; y < roi_y1; ++y) {
                if ((*pixels)[(size_t)y * *width + roi_x0] < threshold) ++dark;
            }
            if (dark * 10u < (uint64_t)(roi_y1 - roi_y0) * 9u) break;
            ++roi_x0;
            ++peeled;
        }
        peeled = 0;
        while (roi_x0 + 1u < roi_x1 && peeled < limit) {
            uint64_t dark = 0;
            uint32_t y;
            for (y = roi_y0; y < roi_y1; ++y) {
                if ((*pixels)[(size_t)y * *width + roi_x1 - 1u] < threshold) ++dark;
            }
            if (dark * 10u < (uint64_t)(roi_y1 - roi_y0) * 9u) break;
            --roi_x1;
            ++peeled;
        }
    }
    min_x = *width;
    min_y = *height;
    max_x = max_y = 0;
    for (y = roi_y0; y < roi_y1; ++y) {
        uint32_t x;
        const uint8_t *row = *pixels + (size_t)y * *width;
        for (x = roi_x0; x < roi_x1; ++x) {
            if (row[x] < threshold) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }
    if (min_x == *width || min_y == *height) return QW_STATUS_OK;
    if (padding > min_x) min_x = 0;
    else min_x -= padding;
    if (padding > min_y) min_y = 0;
    else min_y -= padding;
    if (padding >= *width - 1u - max_x) max_x = *width - 1u;
    else max_x += padding;
    if (padding >= *height - 1u - max_y) max_y = *height - 1u;
    else max_y += padding;
    out_width = max_x - min_x + 1u;
    out_height = max_y - min_y + 1u;
    if (out_width < 16u || out_height < 16u ||
        (uint64_t)out_width * 20u < *width ||
        (uint64_t)out_height * 20u < *height) return QW_STATUS_OK;
    if (!qw_checked_size_mul(out_width, out_height, &out_count)) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    cropped = (uint8_t *)malloc(out_count);
    if (!cropped) return QW_STATUS_ERR_OUT_OF_MEMORY;
    for (y = 0; y < out_height; ++y) {
        memcpy(cropped + (size_t)y * out_width,
               *pixels + (size_t)(min_y + y) * *width + min_x,
               out_width);
    }
    free(*pixels);
    *pixels = cropped;
    *width = out_width;
    *height = out_height;
    return QW_STATUS_OK;
}

int qw_scan_split_gray_copy(const uint8_t *pixels,
                            uint32_t width,
                            uint32_t height,
                            unsigned side,
                            uint8_t **out_pixels,
                            uint32_t *out_width)
{
    uint32_t center;
    uint32_t radius;
    uint32_t split;
    uint64_t best = UINT64_MAX;
    uint32_t x;
    uint32_t start;
    uint32_t end;
    uint32_t part_width;
    size_t out_count;
    uint8_t *out;
    uint32_t y;

    if (!pixels || !width || !height || !out_pixels || !out_width ||
        side > 1u || width < 4u) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_pixels = NULL;
    center = width / 2u;
    radius = width / 8u;
    start = center > radius ? center - radius : 1u;
    end = center + radius;
    if (end >= width) end = width - 1u;
    split = center;
    for (x = start; x <= end; ++x) {
        uint64_t darkness = 0;
        for (y = 0; y < height; ++y) {
            darkness += 255u - pixels[(size_t)y * width + x];
        }
        if (darkness < best) {
            best = darkness;
            split = x;
        }
    }
    if (split < width / 4u || split > width - width / 4u) split = center;
    if (side == 0u) {
        part_width = split;
        start = 0;
    } else {
        part_width = width - split;
        start = split;
    }
    if (!qw_checked_size_mul(part_width, height, &out_count)) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    out = (uint8_t *)malloc(out_count);
    if (!out) return QW_STATUS_ERR_OUT_OF_MEMORY;
    for (y = 0; y < height; ++y) {
        memcpy(out + (size_t)y * part_width,
               pixels + (size_t)y * width + start, part_width);
    }
    *out_pixels = out;
    *out_width = part_width;
    return QW_STATUS_OK;
}

int qw_scan_split_gray(uint8_t **pixels,
                       uint32_t *width,
                       uint32_t *height,
                       unsigned side)
{
    uint8_t *out = NULL;
    uint32_t out_width;
    int rc;
    if (!pixels || !*pixels || !width || !height) return QW_STATUS_ERR_INVALID_ARGUMENT;
    rc = qw_scan_split_gray_copy(*pixels, *width, *height, side,
                                 &out, &out_width);
    if (rc != QW_STATUS_OK) return rc;
    free(*pixels);
    *pixels = out;
    *width = out_width;
    return QW_STATUS_OK;
}
