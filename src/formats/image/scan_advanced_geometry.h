#ifndef QUIREWISE_SCAN_ADVANCED_GEOMETRY_H
#define QUIREWISE_SCAN_ADVANCED_GEOMETRY_H

#include <stdint.h>

int qw_scan_perspective_gray(uint8_t **pixels,
                             uint32_t *width,
                             uint32_t *height);
int qw_scan_dewarp_gray(uint8_t **pixels,
                        uint32_t width,
                        uint32_t height);

#endif
