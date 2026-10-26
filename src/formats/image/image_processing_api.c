#include "quirewise/image_processing.h"

#include "scan_advanced_geometry.h"
#include "scan_geometry.h"
#include "scan_tone.h"
#include "quirewise/status.h"

#include <stdlib.h>
#include <string.h>

static QwImageProcessingStatus gray_image_pixel_count(
    uint32_t width, uint32_t height, size_t *out_count)
{
    size_t count;
    if (!width || !height || !out_count)
        return QW_IMAGE_PROCESSING_ERR_INVALID_ARGUMENT;
    if (width > QW_GRAY_IMAGE_MAX_DIMENSION ||
        height > QW_GRAY_IMAGE_MAX_DIMENSION ||
        (size_t)width > (size_t)QW_GRAY_IMAGE_MAX_PIXELS / height)
        return QW_IMAGE_PROCESSING_ERR_RESOURCE_LIMIT;
    count = (size_t)width * height;
    *out_count = count;
    return QW_IMAGE_PROCESSING_OK;
}

static QwImageProcessingStatus gray_image_validate(const QwGrayImage *image)
{
    size_t count;
    QwImageProcessingStatus status;
    if (!image || !image->pixels)
        return QW_IMAGE_PROCESSING_ERR_INVALID_ARGUMENT;
    status = gray_image_pixel_count(image->width, image->height, &count);
    if (status != QW_IMAGE_PROCESSING_OK) return status;
    return image->size < count ? QW_IMAGE_PROCESSING_ERR_FORMAT
                               : QW_IMAGE_PROCESSING_OK;
}

static int gray_image_output_empty(const QwGrayImage *image)
{
    return image && !image->pixels && !image->size &&
           !image->width && !image->height;
}

static QwImageProcessingStatus gray_image_update_size(QwGrayImage *image)
{
    size_t count;
    if (!image || !image->pixels || !image->width || !image->height ||
        (size_t)image->width > SIZE_MAX / image->height) {
        if (image) image->size = 0u;
        return QW_IMAGE_PROCESSING_ERR_RESOURCE_LIMIT;
    }
    count = (size_t)image->width * image->height;
    image->size = count;
    if (image->width > QW_GRAY_IMAGE_MAX_DIMENSION ||
        image->height > QW_GRAY_IMAGE_MAX_DIMENSION ||
        count > (size_t)QW_GRAY_IMAGE_MAX_PIXELS) {
        return QW_IMAGE_PROCESSING_ERR_RESOURCE_LIMIT;
    }
    if (!count) {
        image->size = 0u;
        return QW_IMAGE_PROCESSING_ERR_FORMAT;
    }
    return QW_IMAGE_PROCESSING_OK;
}

static QwImageProcessingStatus gray_image_map_status(int status)
{
    switch (status) {
    case QW_STATUS_OK: return QW_IMAGE_PROCESSING_OK;
    case QW_STATUS_ERR_INVALID_ARGUMENT:
        return QW_IMAGE_PROCESSING_ERR_INVALID_ARGUMENT;
    case QW_STATUS_ERR_OUT_OF_MEMORY:
        return QW_IMAGE_PROCESSING_ERR_OUT_OF_MEMORY;
    case QW_STATUS_ERR_UNSUPPORTED:
        return QW_IMAGE_PROCESSING_ERR_UNSUPPORTED;
    case QW_STATUS_ERR_RESOURCE_LIMIT:
        return QW_IMAGE_PROCESSING_ERR_RESOURCE_LIMIT;
    default: return QW_IMAGE_PROCESSING_ERR_FORMAT;
    }
}

uint32_t qw_image_processing_abi_version(void)
{
    return QUIREWISE_IMAGE_PROCESSING_ABI_VERSION;
}

QwImageProcessingStatus qw_gray_image_copy(const uint8_t *pixels, size_t size,
                                           uint32_t width, uint32_t height,
                                           QwGrayImage *out_image)
{
    size_t count;
    uint8_t *copy;
    QwImageProcessingStatus status;
    if (!pixels || !gray_image_output_empty(out_image))
        return QW_IMAGE_PROCESSING_ERR_INVALID_ARGUMENT;
    status = gray_image_pixel_count(width, height, &count);
    if (status != QW_IMAGE_PROCESSING_OK) return status;
    if (size < count) return QW_IMAGE_PROCESSING_ERR_FORMAT;
    copy = (uint8_t *)malloc(count);
    if (!copy) return QW_IMAGE_PROCESSING_ERR_OUT_OF_MEMORY;
    memcpy(copy, pixels, count);
    out_image->pixels = copy;
    out_image->size = count;
    out_image->width = width;
    out_image->height = height;
    return QW_IMAGE_PROCESSING_OK;
}

QwImageProcessingStatus qw_gray_image_adopt(uint8_t *pixels, size_t size,
                                            uint32_t width, uint32_t height,
                                            QwGrayImage *out_image)
{
    size_t count;
    QwImageProcessingStatus status;
    if (!pixels || !gray_image_output_empty(out_image))
        return QW_IMAGE_PROCESSING_ERR_INVALID_ARGUMENT;
    status = gray_image_pixel_count(width, height, &count);
    if (status != QW_IMAGE_PROCESSING_OK) return status;
    if (size < count) return QW_IMAGE_PROCESSING_ERR_FORMAT;
    out_image->pixels = pixels;
    out_image->size = count;
    out_image->width = width;
    out_image->height = height;
    return QW_IMAGE_PROCESSING_OK;
}

void qw_gray_image_release(QwGrayImage *image)
{
    if (!image) return;
    free(image->pixels);
    *image = (QwGrayImage){0};
}

QwImageProcessingStatus qw_gray_image_normalize_illumination(
    QwGrayImage *image)
{
    QwImageProcessingStatus valid = gray_image_validate(image);
    if (valid != QW_IMAGE_PROCESSING_OK) return valid;
    return gray_image_map_status(qw_scan_normalize_illumination(
        image->pixels, image->width, image->height));
}

QwImageProcessingStatus qw_gray_image_deskew(QwGrayImage *image)
{
    int status;
    QwImageProcessingStatus valid = gray_image_validate(image);
    if (valid != QW_IMAGE_PROCESSING_OK) return valid;
    status = qw_scan_deskew_gray(&image->pixels, &image->width, &image->height);
    if (status != QW_STATUS_OK) return gray_image_map_status(status);
    return gray_image_update_size(image);
}

QwImageProcessingStatus qw_gray_image_crop(QwGrayImage *image,
                                           unsigned padding)
{
    int status;
    QwImageProcessingStatus valid = gray_image_validate(image);
    if (valid != QW_IMAGE_PROCESSING_OK) return valid;
    status = qw_scan_crop_gray(&image->pixels, &image->width, &image->height,
                               padding);
    if (status != QW_STATUS_OK) return gray_image_map_status(status);
    return gray_image_update_size(image);
}

QwImageProcessingStatus qw_gray_image_correct_perspective(QwGrayImage *image)
{
    int status;
    QwImageProcessingStatus valid = gray_image_validate(image);
    if (valid != QW_IMAGE_PROCESSING_OK) return valid;
    status = qw_scan_perspective_gray(&image->pixels, &image->width,
                                      &image->height);
    if (status != QW_STATUS_OK) return gray_image_map_status(status);
    return gray_image_update_size(image);
}

QwImageProcessingStatus qw_gray_image_dewarp(QwGrayImage *image)
{
    int status;
    QwImageProcessingStatus valid = gray_image_validate(image);
    if (valid != QW_IMAGE_PROCESSING_OK) return valid;
    status = qw_scan_dewarp_gray(&image->pixels, image->width, image->height);
    if (status != QW_STATUS_OK) return gray_image_map_status(status);
    return gray_image_update_size(image);
}

QwImageProcessingStatus qw_gray_image_split_copy(
    const QwGrayImage *image, QwGrayImageSplitSide side,
    QwGrayImage *out_image)
{
    uint8_t *pixels = NULL;
    uint32_t width = 0u;
    size_t count;
    int status;
    QwImageProcessingStatus valid = gray_image_validate(image);
    if (valid != QW_IMAGE_PROCESSING_OK) return valid;
    if (!gray_image_output_empty(out_image) ||
        (side != QW_GRAY_IMAGE_SPLIT_LEFT &&
         side != QW_GRAY_IMAGE_SPLIT_RIGHT))
        return QW_IMAGE_PROCESSING_ERR_INVALID_ARGUMENT;
    status = qw_scan_split_gray_copy(image->pixels, image->width, image->height,
                                     (unsigned)side, &pixels, &width);
    if (status != QW_STATUS_OK) return gray_image_map_status(status);
    valid = gray_image_pixel_count(width, image->height, &count);
    if (valid != QW_IMAGE_PROCESSING_OK) {
        free(pixels);
        return valid;
    }
    out_image->pixels = pixels;
    out_image->size = count;
    out_image->width = width;
    out_image->height = image->height;
    return QW_IMAGE_PROCESSING_OK;
}

const char *qw_image_processing_status_string(int status)
{
    switch (status) {
    case QW_IMAGE_PROCESSING_OK: return "ok";
    case QW_IMAGE_PROCESSING_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QW_IMAGE_PROCESSING_ERR_OUT_OF_MEMORY: return "out of memory";
    case QW_IMAGE_PROCESSING_ERR_UNSUPPORTED: return "unsupported operation";
    case QW_IMAGE_PROCESSING_ERR_FORMAT: return "invalid Gray8 image";
    case QW_IMAGE_PROCESSING_ERR_RESOURCE_LIMIT:
        return "image processing resource limit exceeded";
    default: return "unknown image processing status";
    }
}
