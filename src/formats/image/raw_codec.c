#include "image_internal.h"

#include <libraw/libraw.h>

#include <stdlib.h>
#include <string.h>

static int raw_error_to_status(int error)
{
    if (error == LIBRAW_UNSUFFICIENT_MEMORY) return QW_STATUS_ERR_OUT_OF_MEMORY;
    if (error == LIBRAW_TOO_BIG) return QW_STATUS_ERR_RESOURCE_LIMIT;
    return QW_STATUS_ERR_DECODE;
}

static int raw_process_with_libraw(libraw_data_t *raw,
                                   const void *data,
                                   size_t size,
                                   libraw_processed_image_t **out_processed)
{
    libraw_processed_image_t *processed;
    int raw_width;
    int raw_height;
    int image_width;
    int image_height;
    int result;
    int error = LIBRAW_SUCCESS;

    if (!raw || !data || !size || !out_processed)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_processed = NULL;
    result = libraw_open_buffer(raw, data, size);
    if (result != LIBRAW_SUCCESS) return raw_error_to_status(result);

    raw_width = libraw_get_raw_width(raw);
    raw_height = libraw_get_raw_height(raw);
    image_width = libraw_get_iwidth(raw);
    image_height = libraw_get_iheight(raw);
    if (raw_width <= 0 || raw_height <= 0 ||
        image_width <= 0 || image_height <= 0) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (!qw_image_dimensions_within_limits((uint32_t)raw_width,
                                          (uint32_t)raw_height) ||
        !qw_image_dimensions_within_limits((uint32_t)image_width,
                                          (uint32_t)image_height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }

    libraw_set_output_color(raw, 1); /* sRGB */
    libraw_set_output_bps(raw, 8);
    result = libraw_unpack(raw);
    if (result != LIBRAW_SUCCESS) {
        return raw_error_to_status(result);
    }
    result = libraw_dcraw_process(raw);
    if (result != LIBRAW_SUCCESS) {
        return raw_error_to_status(result);
    }
    processed = libraw_dcraw_make_mem_image(raw, &error);
    if (!processed) return raw_error_to_status(error);
    *out_processed = processed;
    return QW_STATUS_OK;
}

static int raw_processed_image_to_rgb24(
    const libraw_processed_image_t *processed,
    QwPageImage *out_page)
{
    uint8_t *pixels;
    size_t pixel_count;
    size_t sample_count;
    size_t source_size;
    size_t output_size;
    size_t sample_bytes;

    if (!processed || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (processed->type != LIBRAW_IMAGE_BITMAP) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (processed->width == 0u || processed->height == 0u ||
        processed->colors != 3u ||
        !processed->data ||
        (processed->bits != 8u && processed->bits != 16u)) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (!qw_image_dimensions_within_limits(processed->width,
                                          processed->height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (!qw_checked_size_mul((size_t)processed->width,
                             (size_t)processed->height, &pixel_count) ||
        !qw_checked_size_mul(pixel_count, 3u, &sample_count) ||
        !qw_checked_size_mul(sample_count, (size_t)(processed->bits / 8u),
                             &source_size) ||
        !qw_checked_size_mul(pixel_count, 3u, &output_size)) {
        return QW_STATUS_ERR_FORMAT;
    }
    if ((size_t)processed->data_size < source_size) {
        return QW_STATUS_ERR_FORMAT;
    }

    pixels = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!pixels) return QW_STATUS_ERR_OUT_OF_MEMORY;
    if (processed->bits == 8u) {
        memcpy(pixels, processed->data, output_size);
    } else {
        sample_bytes = sizeof(uint16_t);
        for (size_t i = 0u; i < sample_count; ++i) {
            uint16_t sample;
            memcpy(&sample, processed->data + i * sample_bytes,
                   sizeof(sample));
            pixels[i] = (uint8_t)(((uint32_t)sample + 128u) / 257u);
        }
    }

    out_page->data = pixels;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = processed->width;
    out_page->height = processed->height;
    return QW_STATUS_OK;
}

int qw_raw_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    libraw_data_t *raw = NULL;
    libraw_processed_image_t *processed = NULL;
    int rc;

    if (!data || !size || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    if (!qw_raw_probe(data, size)) return QW_STATUS_ERR_FORMAT;

    raw = libraw_init(LIBRAW_OPTIONS_NO_DATAERR_CALLBACK);
    if (!raw) return QW_STATUS_ERR_OUT_OF_MEMORY;
    rc = raw_process_with_libraw(raw, data, size, &processed);
    if (rc == QW_STATUS_OK) {
        rc = raw_processed_image_to_rgb24(processed, out_page);
    }

    if (processed) libraw_dcraw_clear_mem(processed);
    if (raw) libraw_close(raw);
    return rc;
}
