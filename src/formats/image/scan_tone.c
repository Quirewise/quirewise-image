#include "scan_tone.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define QW_SCAN_TONE_TILE 64u


static int compare_u8(const void *left, const void *right)
{
    unsigned a = *(const uint8_t *)left;
    unsigned b = *(const uint8_t *)right;
    return (a > b) - (a < b);
}

int qw_scan_normalize_illumination(uint8_t *pixels,
                                   uint32_t width,
                                   uint32_t height)
{
    size_t pixel_count;
    uint32_t grid_width;
    uint32_t grid_height;
    size_t grid_count;
    uint8_t *background = NULL;
    uint8_t *sorted = NULL;
    uint8_t *smoothed = NULL;
    uint32_t gy;
    unsigned target;

    if (!pixels || !qw_checked_size_mul(width, height, &pixel_count)) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    (void)pixel_count;
    grid_width = (width - 1u) / QW_SCAN_TONE_TILE + 1u;
    grid_height = (height - 1u) / QW_SCAN_TONE_TILE + 1u;
    if ((size_t)grid_width > SIZE_MAX / grid_height) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    grid_count = (size_t)grid_width * grid_height;
    background = (uint8_t *)malloc(grid_count);
    sorted = (uint8_t *)malloc(grid_count);
    smoothed = (uint8_t *)malloc(grid_count);
    if (!background || !sorted || !smoothed) {
        free(background);
        free(sorted);
        free(smoothed);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }

    for (gy = 0; gy < grid_height; ++gy) {
        uint32_t gx;
        uint32_t y0 = gy * QW_SCAN_TONE_TILE;
        uint32_t y1 = y0 + QW_SCAN_TONE_TILE;
        if (y1 > height) y1 = height;
        for (gx = 0; gx < grid_width; ++gx) {
            uint32_t x0 = gx * QW_SCAN_TONE_TILE;
            uint32_t x1 = x0 + QW_SCAN_TONE_TILE;
            uint64_t histogram[256] = {0};
            size_t samples = 0;
            size_t target_rank;
            unsigned value;
            uint32_t y;
            if (x1 > width) x1 = width;
            for (y = y0; y < y1; ++y) {
                uint32_t x;
                const uint8_t *row = pixels + (size_t)y * width;
                for (x = x0; x < x1; ++x) {
                    ++histogram[row[x]];
                    ++samples;
                }
            }
            target_rank = (samples * 9u + 9u) / 10u;
            for (value = 0; value < 255u; ++value) {
                if (target_rank <= histogram[value]) break;
                target_rank -= (size_t)histogram[value];
            }
            background[(size_t)gy * grid_width + gx] = (uint8_t)value;
        }
    }
    memcpy(sorted, background, grid_count);
    qsort(sorted, grid_count, sizeof(*sorted), compare_u8);
    target = sorted[grid_count / 2u];
    if (target < 128u) target = 128u;

    /* Smooth tile estimates before interpolation so printed text does not
     * become a block-shaped illumination map. */
    for (gy = 0; gy < grid_height; ++gy) {
        uint32_t gx;
        for (gx = 0; gx < grid_width; ++gx) {
            unsigned sum = 0;
            unsigned n = 0;
            int dy;
            for (dy = -1; dy <= 1; ++dy) {
                int yy = (int)gy + dy;
                int dx;
                if (yy < 0 || (uint32_t)yy >= grid_height) continue;
                for (dx = -1; dx <= 1; ++dx) {
                    int xx = (int)gx + dx;
                    if (xx < 0 || (uint32_t)xx >= grid_width) continue;
                    sum += background[(size_t)yy * grid_width + (uint32_t)xx];
                    ++n;
                }
            }
            smoothed[(size_t)gy * grid_width + gx] =
                (uint8_t)(n ? (sum + n / 2u) / n : background[(size_t)gy * grid_width + gx]);
        }
    }

    for (gy = 0; gy < height; ++gy) {
        uint32_t gx;
        uint32_t cell_y = gy / QW_SCAN_TONE_TILE;
        uint32_t next_y = cell_y + 1u < grid_height ? cell_y + 1u : cell_y;
        unsigned frac_y = gy % QW_SCAN_TONE_TILE;
        if (cell_y + 1u >= grid_height) frac_y = 0;
        for (gx = 0; gx < width; ++gx) {
            uint32_t cell_x = gx / QW_SCAN_TONE_TILE;
            uint32_t next_x = cell_x + 1u < grid_width ? cell_x + 1u : cell_x;
            unsigned frac_x = gx % QW_SCAN_TONE_TILE;
            unsigned b00;
            unsigned b10;
            unsigned b01;
            unsigned b11;
            unsigned top;
            unsigned bottom;
            unsigned local_background;
            unsigned value;
            size_t index = (size_t)gy * width + gx;
            if (cell_x + 1u >= grid_width) frac_x = 0;
            b00 = smoothed[(size_t)cell_y * grid_width + cell_x];
            b10 = smoothed[(size_t)cell_y * grid_width + next_x];
            b01 = smoothed[(size_t)next_y * grid_width + cell_x];
            b11 = smoothed[(size_t)next_y * grid_width + next_x];
            top = (b00 * (QW_SCAN_TONE_TILE - frac_x) +
                   b10 * frac_x + QW_SCAN_TONE_TILE / 2u) / QW_SCAN_TONE_TILE;
            bottom = (b01 * (QW_SCAN_TONE_TILE - frac_x) +
                      b11 * frac_x + QW_SCAN_TONE_TILE / 2u) / QW_SCAN_TONE_TILE;
            local_background = (top * (QW_SCAN_TONE_TILE - frac_y) +
                                bottom * frac_y + QW_SCAN_TONE_TILE / 2u) /
                               QW_SCAN_TONE_TILE;
            if (local_background < 80u) local_background = 80u;
            value = ((unsigned)pixels[index] * target + local_background / 2u) /
                    local_background;
            pixels[index] = (uint8_t)(value > 255u ? 255u : value);
        }
    }
    free(background);
    free(sorted);
    free(smoothed);
    return QW_STATUS_OK;
}
