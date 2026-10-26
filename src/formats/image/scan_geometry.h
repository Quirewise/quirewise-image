#ifndef QUIREWISE_SCAN_GEOMETRY_H
#define QUIREWISE_SCAN_GEOMETRY_H

#include <stdint.h>

int qw_scan_deskew_gray(uint8_t **pixels, uint32_t *width, uint32_t *height);
int qw_scan_crop_gray(uint8_t **pixels,
                      uint32_t *width,
                      uint32_t *height,
                      unsigned padding);
int qw_scan_split_gray(uint8_t **pixels,
                       uint32_t *width,
                       uint32_t *height,
                       unsigned side);
int qw_scan_split_gray_copy(const uint8_t *pixels,
                            uint32_t width,
                            uint32_t height,
                            unsigned side,
                            uint8_t **out_pixels,
                            uint32_t *out_width);

#endif
