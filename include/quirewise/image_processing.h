#ifndef QUIREWISE_IMAGE_PROCESSING_H
#define QUIREWISE_IMAGE_PROCESSING_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(QUIREWISE_IMAGE_PROCESSING_BUILD_SHARED)
#  if defined(QUIREWISE_IMAGE_PROCESSING_EXPORTS)
#    define QUIREWISE_IMAGE_PROCESSING_API __declspec(dllexport)
#  else
#    define QUIREWISE_IMAGE_PROCESSING_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(QUIREWISE_IMAGE_PROCESSING_BUILD_SHARED)
#  define QUIREWISE_IMAGE_PROCESSING_API __attribute__((visibility("default")))
#else
#  define QUIREWISE_IMAGE_PROCESSING_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define QUIREWISE_IMAGE_PROCESSING_ABI_VERSION 1u
#define QW_GRAY_IMAGE_MAX_DIMENSION 32768u
#define QW_GRAY_IMAGE_MAX_PIXELS 20000000u

typedef enum QwImageProcessingStatus {
    QW_IMAGE_PROCESSING_OK = 0,
    QW_IMAGE_PROCESSING_ERR_INVALID_ARGUMENT = -1,
    QW_IMAGE_PROCESSING_ERR_OUT_OF_MEMORY = -2,
    QW_IMAGE_PROCESSING_ERR_UNSUPPORTED = -3,
    QW_IMAGE_PROCESSING_ERR_FORMAT = -4,
    QW_IMAGE_PROCESSING_ERR_RESOURCE_LIMIT = -5
} QwImageProcessingStatus;

/* Tightly packed Gray8 pixel storage owned by this value. Start with {0};
 * release it with qw_gray_image_release. Processing calls may replace pixels
 * and update dimensions. On error, the value remains releasable and may have
 * been modified. */
typedef struct QwGrayImage {
    uint8_t *pixels;
    size_t size;
    uint32_t width;
    uint32_t height;
} QwGrayImage;

typedef enum QwGrayImageSplitSide {
    QW_GRAY_IMAGE_SPLIT_LEFT = 0,
    QW_GRAY_IMAGE_SPLIT_RIGHT = 1
} QwGrayImageSplitSide;

/*! Return the ABI version compiled into the installed image-processing package. */
QUIREWISE_IMAGE_PROCESSING_API uint32_t qw_image_processing_abi_version(void);

/*! Copy tightly packed Gray8 pixels into an empty, zero-initialized value. */
QUIREWISE_IMAGE_PROCESSING_API QwImageProcessingStatus qw_gray_image_copy(
    const uint8_t *pixels, size_t size, uint32_t width, uint32_t height,
    QwGrayImage *out_image);
/*! Adopt a malloc-owned Gray8 buffer into an empty, zero-initialized value. */
QUIREWISE_IMAGE_PROCESSING_API QwImageProcessingStatus qw_gray_image_adopt(
    uint8_t *pixels, size_t size, uint32_t width, uint32_t height,
    QwGrayImage *out_image);
/*! Release pixels and reset the value; NULL is accepted. */
QUIREWISE_IMAGE_PROCESSING_API void qw_gray_image_release(QwGrayImage *image);

/*! Flatten uneven scan illumination before thresholding. */
QUIREWISE_IMAGE_PROCESSING_API QwImageProcessingStatus
qw_gray_image_normalize_illumination(QwGrayImage *image);
/*! Rotate the page so text baselines are horizontal. */
QUIREWISE_IMAGE_PROCESSING_API QwImageProcessingStatus qw_gray_image_deskew(
    QwGrayImage *image);
/*! Crop the page to its content plus the requested padding in pixels. */
QUIREWISE_IMAGE_PROCESSING_API QwImageProcessingStatus qw_gray_image_crop(
    QwGrayImage *image, unsigned padding);
/*! Rectify a page photographed at an angle into a rectangular image. */
QUIREWISE_IMAGE_PROCESSING_API QwImageProcessingStatus
qw_gray_image_correct_perspective(QwGrayImage *image);
/*! Remove page curvature from a photographed or scanned book page. */
QUIREWISE_IMAGE_PROCESSING_API QwImageProcessingStatus qw_gray_image_dewarp(
    QwGrayImage *image);

/* Creates an owned copy of one detected half-page. out_image must be empty and
 * zero-initialized. */
QUIREWISE_IMAGE_PROCESSING_API QwImageProcessingStatus
qw_gray_image_split_copy(const QwGrayImage *image,
                         QwGrayImageSplitSide side,
                         QwGrayImage *out_image);

/*! Return a static human-readable name for a status code. */
QUIREWISE_IMAGE_PROCESSING_API const char *qw_image_processing_status_string(
    int status);

#ifdef __cplusplus
}
#endif

#endif
