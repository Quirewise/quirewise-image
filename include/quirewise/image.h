#ifndef QUIREWISE_IMAGE_H
#define QUIREWISE_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(QUIREWISE_IMAGE_BUILD_SHARED)
#  if defined(QUIREWISE_IMAGE_EXPORTS)
#    define QUIREWISE_IMAGE_API __declspec(dllexport)
#  else
#    define QUIREWISE_IMAGE_API __declspec(dllimport)
#  endif
#elif defined(_WIN32) && defined(QW_IMAGE_BUILD_SHARED)
#  if defined(QW_IMAGE_EXPORTS)
#    define QUIREWISE_IMAGE_API __declspec(dllexport)
#  else
#    define QUIREWISE_IMAGE_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && (defined(QUIREWISE_IMAGE_BUILD_SHARED) || \
                            defined(QW_IMAGE_BUILD_SHARED))
#  define QUIREWISE_IMAGE_API __attribute__((visibility("default")))
#else
#  define QUIREWISE_IMAGE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define QUIREWISE_IMAGE_ABI_VERSION 1u

typedef enum QwImageStatus {
    QW_IMAGE_OK = 0,
    QW_IMAGE_ERR_INVALID_ARGUMENT = -1,
    QW_IMAGE_ERR_OUT_OF_MEMORY = -2,
    QW_IMAGE_ERR_IO = -3,
    QW_IMAGE_ERR_FORMAT = -4,
    QW_IMAGE_ERR_UNSUPPORTED = -5,
    QW_IMAGE_ERR_DECODE = -6,
    QW_IMAGE_ERR_EXTERNAL_DECODER_REQUIRED = -9,
    QW_IMAGE_ERR_RESOURCE_LIMIT = -11
} QwImageStatus;

typedef enum QwImagePixelFormat {
    QW_IMAGE_PIXEL_GRAY8 = 1,
    QW_IMAGE_PIXEL_RGB24 = 2
} QwImagePixelFormat;

/* Decoded pixel storage is owned by this value and released with qw_image_free. */
typedef struct QwImage {
    uint8_t *pixels;
    size_t size;
    size_t stride;
    uint32_t width;
    uint32_t height;
    QwImagePixelFormat format;
} QwImage;

QUIREWISE_IMAGE_API uint32_t qw_image_abi_version(void);

/* Returns 1 when the input matches a supported raster image probe, otherwise 0. */
QUIREWISE_IMAGE_API int qw_image_probe(const void *data, size_t size);

/* Returns 1 when a path has a supported raster-image filename extension. */
QUIREWISE_IMAGE_API int qw_image_name_is_supported(const char *name);

/* Decodes one still image to owned Gray8 or RGB24 pixels. */
QUIREWISE_IMAGE_API int qw_image_decode(const void *data,
                                        size_t size,
                                        QwImage *out_image);
QUIREWISE_IMAGE_API void qw_image_free(QwImage *image);
QUIREWISE_IMAGE_API const char *qw_image_status_string(int status);

#ifdef __cplusplus
}
#endif

#endif
