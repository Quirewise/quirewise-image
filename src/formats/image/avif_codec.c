#include "image_internal.h"

#include <avif/avif.h>

#include <stdlib.h>
#include <string.h>

static int avif_result_to_error(avifResult result)
{
    if (result == AVIF_RESULT_OUT_OF_MEMORY) return QW_STATUS_ERR_OUT_OF_MEMORY;
    if (result == AVIF_RESULT_INVALID_ARGUMENT) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (result == AVIF_RESULT_NO_CODEC_AVAILABLE ||
        result == AVIF_RESULT_NOT_IMPLEMENTED) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    return QW_STATUS_ERR_DECODE;
}

int qw_avif_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    avifDecoder *decoder = NULL;
    avifRGBImage rgb;
    uint8_t *pixels = NULL;
    size_t pixel_count;
    size_t output_size;
    size_t rgb_size;
    uint32_t width;
    uint32_t height;
    avifResult avif_rc;
    int rc = QW_STATUS_ERR_DECODE;

    if (!data || !size || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    memset(&rgb, 0, sizeof(rgb));
    if (!qw_avif_probe(data, size)) return QW_STATUS_ERR_FORMAT;

    decoder = avifDecoderCreate();
    if (!decoder) return QW_STATUS_ERR_OUT_OF_MEMORY;
    decoder->maxThreads = 1;
    decoder->imageSizeLimit = (uint32_t)QW_IMAGE_MAX_DECODED_PIXELS;
    decoder->imageDimensionLimit = QW_IMAGE_MAX_DIMENSION;
    decoder->imageCountLimit = 2u;
    avif_rc = avifDecoderSetIOMemory(decoder, (const uint8_t *)data, size);
    if (avif_rc != AVIF_RESULT_OK) {
        rc = avif_result_to_error(avif_rc);
        goto cleanup;
    }
    avif_rc = avifDecoderParse(decoder);
    if (avif_rc != AVIF_RESULT_OK) {
        rc = avif_result_to_error(avif_rc);
        goto cleanup;
    }
    if (decoder->imageSequenceTrackPresent || decoder->imageCount != 1 ||
        decoder->progressiveState != AVIF_PROGRESSIVE_STATE_UNAVAILABLE) {
        rc = QW_STATUS_ERR_UNSUPPORTED;
        goto cleanup;
    }
    if (!decoder->image || !decoder->image->width || !decoder->image->height) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    if (decoder->image->transformFlags != AVIF_TRANSFORM_NONE) {
        rc = QW_STATUS_ERR_UNSUPPORTED;
        goto cleanup;
    }
    width = decoder->image->width;
    height = decoder->image->height;
    if (!qw_image_dimensions_within_limits(width, height)) {
        rc = QW_STATUS_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    avif_rc = avifDecoderNextImage(decoder);
    if (avif_rc != AVIF_RESULT_OK) {
        rc = avif_result_to_error(avif_rc);
        goto cleanup;
    }
    if (!decoder->image || decoder->image->width != width ||
        decoder->image->height != height ||
        decoder->image->transformFlags != AVIF_TRANSFORM_NONE) {
        rc = QW_STATUS_ERR_UNSUPPORTED;
        goto cleanup;
    }
    if (!qw_checked_size_mul((size_t)width, (size_t)height, &pixel_count) ||
        !qw_checked_size_mul(pixel_count, 3u, &output_size) ||
        !qw_checked_size_mul((size_t)width, 4u, &rgb_size)) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }

    avifRGBImageSetDefaults(&rgb, decoder->image);
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = 8u;
    rgb.maxThreads = 1;
    avif_rc = avifRGBImageAllocatePixels(&rgb);
    if (avif_rc != AVIF_RESULT_OK || !rgb.pixels) {
        rc = avif_rc == AVIF_RESULT_OK
            ? QW_STATUS_ERR_OUT_OF_MEMORY
            : avif_result_to_error(avif_rc);
        goto cleanup;
    }
    if (rgb.width != width || rgb.height != height || rgb.rowBytes < rgb_size ||
        !qw_checked_size_mul((size_t)rgb.rowBytes, (size_t)height, &rgb_size)) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    avif_rc = avifImageYUVToRGB(decoder->image, &rgb);
    if (avif_rc != AVIF_RESULT_OK) {
        rc = avif_result_to_error(avif_rc);
        goto cleanup;
    }
    pixels = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!pixels) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (uint32_t y = 0u; y < height; ++y) {
        const uint8_t *source = rgb.pixels + (size_t)y * rgb.rowBytes;
        uint8_t *destination = pixels + (size_t)y * width * 3u;
        for (uint32_t x = 0u; x < width; ++x) {
            uint32_t alpha = source[(size_t)x * 4u + 3u];
            for (size_t channel = 0u; channel < 3u; ++channel) {
                uint32_t color = source[(size_t)x * 4u + channel];
                destination[(size_t)x * 3u + channel] =
                    (uint8_t)((color * alpha + 255u * (255u - alpha) + 127u) /
                              255u);
            }
        }
    }
    out_page->data = pixels;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = width;
    out_page->height = height;
    pixels = NULL;
    rc = QW_STATUS_OK;

cleanup:
    free(pixels);
    avifRGBImageFreePixels(&rgb);
    avifDecoderDestroy(decoder);
    return rc;
}

int qw_avif_encode_page(const QwPageImage *page,
                        uint8_t **out_data,
                        size_t *out_size)
{
    avifImage *image = NULL;
    avifEncoder *encoder = NULL;
    avifRWData encoded = AVIF_DATA_EMPTY;
    avifRGBImage rgb;
    const uint8_t *source = NULL;
    uint8_t *gray_rgb = NULL;
    uint8_t *result = NULL;
    size_t pixel_count;
    size_t source_size;
    size_t row_bytes;
    unsigned channels;
    avifResult avif_rc;
    int rc = QW_STATUS_ERR_UNSUPPORTED;

    if (!page || !out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    memset(&rgb, 0, sizeof(rgb));
    if (!page->data || !qw_image_dimensions_within_limits(page->width,
                                                          page->height)) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (page->kind == QW_PAGE_IMAGE_GRAY8) {
        channels = 1u;
    } else if (page->kind == QW_PAGE_IMAGE_RGB24) {
        channels = 3u;
    } else {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (!qw_checked_size_mul((size_t)page->width, (size_t)page->height,
                             &pixel_count) ||
        !qw_checked_size_mul(pixel_count, channels, &source_size) ||
        page->size < source_size ||
        !qw_checked_size_mul((size_t)page->width, 3u, &row_bytes) ||
        row_bytes > UINT32_MAX || pixel_count > SIZE_MAX / 3u) {
        return QW_STATUS_ERR_FORMAT;
    }
    source = page->data;
    if (channels == 1u) {
        gray_rgb = (uint8_t *)malloc(pixel_count * 3u);
        if (!gray_rgb) return QW_STATUS_ERR_OUT_OF_MEMORY;
        for (size_t i = 0u; i < pixel_count; ++i) {
            gray_rgb[i * 3u] = page->data[i];
            gray_rgb[i * 3u + 1u] = page->data[i];
            gray_rgb[i * 3u + 2u] = page->data[i];
        }
        source = gray_rgb;
    }

    image = avifImageCreate(page->width, page->height, 8u,
                            AVIF_PIXEL_FORMAT_YUV444);
    if (!image) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    image->yuvRange = AVIF_RANGE_FULL;
    image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
    image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
    image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT709;
    avifRGBImageSetDefaults(&rgb, image);
    rgb.format = AVIF_RGB_FORMAT_RGB;
    rgb.depth = 8u;
    rgb.maxThreads = 1;
    rgb.pixels = (uint8_t *)source;
    rgb.rowBytes = (uint32_t)row_bytes;
    avif_rc = avifImageRGBToYUV(image, &rgb);
    if (avif_rc != AVIF_RESULT_OK) {
        rc = avif_result_to_error(avif_rc);
        goto cleanup;
    }

    encoder = avifEncoderCreate();
    if (!encoder) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    encoder->maxThreads = 1;
    encoder->speed = 7;
    encoder->quality = 90;
    encoder->qualityAlpha = 100;
    avif_rc = avifEncoderWrite(encoder, image, &encoded);
    if (avif_rc != AVIF_RESULT_OK) {
        rc = avif_result_to_error(avif_rc);
        goto cleanup;
    }
    if (!encoded.data || !encoded.size) {
        rc = QW_STATUS_ERR_DECODE;
        goto cleanup;
    }
    if (encoded.size > QW_AVIF_MAX_OUTPUT_BYTES) {
        rc = QW_STATUS_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    result = (uint8_t *)malloc(encoded.size);
    if (!result) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    memcpy(result, encoded.data, encoded.size);
    *out_data = result;
    *out_size = encoded.size;
    result = NULL;
    rc = QW_STATUS_OK;

cleanup:
    free(result);
    free(gray_rgb);
    avifRWDataFree(&encoded);
    if (encoder) avifEncoderDestroy(encoder);
    if (image) avifImageDestroy(image);
    return rc;
}
