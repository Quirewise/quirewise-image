#include "image_internal.h"

#include "../../core/byte_utils.h"

#include <libheif/heif.h>

#include <stdlib.h>
#include <string.h>

#define QW_HEIC_MAX_DECODE_MEMORY_BYTES ((uint64_t)512u * 1024u * 1024u)

typedef struct QwHeicOutputBuffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    int error;
} QwHeicOutputBuffer;

static int heic_error_to_status(heif_error error)
{
    if (error.code == heif_error_Memory_allocation_error) {
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    if (error.code == heif_error_Unsupported_filetype ||
        error.code == heif_error_Unsupported_feature) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    return QW_STATUS_ERR_DECODE;
}

static void heic_set_security_limits(heif_context *context)
{
    heif_security_limits *limits = heif_context_get_security_limits(context);
    if (!limits) return;
    limits->max_image_size_pixels = (uint64_t)QW_IMAGE_MAX_DECODED_PIXELS;
    limits->max_total_memory = QW_HEIC_MAX_DECODE_MEMORY_BYTES;
}

int qw_heic_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    heif_context *context = NULL;
    heif_image_handle *handle = NULL;
    heif_image *image = NULL;
    const uint8_t *source = NULL;
    uint8_t *pixels = NULL;
    size_t source_stride = 0u;
    size_t source_row_bytes;
    size_t source_size;
    size_t pixel_count;
    size_t output_size;
    size_t channels;
    int width;
    int height;
    int has_alpha;
    heif_error error;
    int rc = QW_STATUS_ERR_DECODE;

    if (!data || !size || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    if (!qw_heic_probe(data, size)) return QW_STATUS_ERR_FORMAT;

    context = heif_context_alloc();
    if (!context) return QW_STATUS_ERR_OUT_OF_MEMORY;
    heic_set_security_limits(context);
    error = heif_context_read_from_memory_without_copy(context, data, size, NULL);
    if (error.code != heif_error_Ok) {
        rc = heic_error_to_status(error);
        goto cleanup;
    }
    if (heif_context_get_number_of_top_level_images(context) != 1) {
        rc = QW_STATUS_ERR_UNSUPPORTED;
        goto cleanup;
    }
    error = heif_context_get_primary_image_handle(context, &handle);
    if (error.code != heif_error_Ok) {
        rc = heic_error_to_status(error);
        goto cleanup;
    }
    width = heif_image_handle_get_width(handle);
    height = heif_image_handle_get_height(handle);
    if (width <= 0 || height <= 0) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    if (!qw_image_dimensions_within_limits((uint32_t)width, (uint32_t)height)) {
        rc = QW_STATUS_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }

    has_alpha = heif_image_handle_has_alpha_channel(handle);
    error = heif_decode_image(
        handle, &image, heif_colorspace_RGB,
        has_alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB,
        NULL);
    if (error.code != heif_error_Ok) {
        rc = heic_error_to_status(error);
        goto cleanup;
    }
    width = heif_image_get_width(image, heif_channel_interleaved);
    height = heif_image_get_height(image, heif_channel_interleaved);
    if (width <= 0 || height <= 0) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    if (!qw_image_dimensions_within_limits((uint32_t)width, (uint32_t)height)) {
        rc = QW_STATUS_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    channels = has_alpha ? 4u : 3u;
    if (!qw_checked_size_mul((size_t)width, channels, &source_row_bytes) ||
        !qw_checked_size_mul((size_t)width, (size_t)height, &pixel_count) ||
        !qw_checked_size_mul(pixel_count, 3u, &output_size)) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    source = heif_image_get_plane_readonly2(
        image, heif_channel_interleaved, &source_stride);
    if (!source || source_stride < source_row_bytes ||
        !qw_checked_size_mul(source_stride, (size_t)height, &source_size)) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    pixels = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!pixels) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (int y = 0; y < height; ++y) {
        const uint8_t *source_row = source + (size_t)y * source_stride;
        uint8_t *destination_row = pixels + (size_t)y * (size_t)width * 3u;
        for (int x = 0; x < width; ++x) {
            const uint8_t *source_pixel = source_row + (size_t)x * channels;
            uint8_t *destination_pixel = destination_row + (size_t)x * 3u;
            if (has_alpha) {
                uint32_t alpha = source_pixel[3];
                for (size_t channel = 0u; channel < 3u; ++channel) {
                    uint32_t color = source_pixel[channel];
                    destination_pixel[channel] = (uint8_t)(
                        (color * alpha + 255u * (255u - alpha) + 127u) / 255u);
                }
            } else {
                memcpy(destination_pixel, source_pixel, 3u);
            }
        }
    }
    out_page->data = pixels;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = (uint32_t)width;
    out_page->height = (uint32_t)height;
    pixels = NULL;
    rc = QW_STATUS_OK;

cleanup:
    free(pixels);
    if (image) heif_image_release(image);
    if (handle) heif_image_handle_release(handle);
    if (context) heif_context_free(context);
    return rc;
}

static heif_error heic_output_error(int out_of_memory)
{
    heif_error error;
    error.code = out_of_memory
        ? heif_error_Memory_allocation_error
        : heif_error_Encoding_error;
    error.subcode = heif_suberror_Unspecified;
    error.message = out_of_memory
        ? "Could not allocate HEIC output buffer"
        : "HEIC output exceeds its configured size limit";
    return error;
}

static heif_error heic_write_output(heif_context *context,
                                    const void *data,
                                    size_t size,
                                    void *userdata)
{
    QwHeicOutputBuffer *output = (QwHeicOutputBuffer *)userdata;
    size_t required;
    (void)context;
    if (!output || (size && !data)) return heic_output_error(0);
    if (output->error != QW_STATUS_OK) {
        return heic_output_error(output->error == QW_STATUS_ERR_OUT_OF_MEMORY);
    }
    if (size > QW_HEIC_MAX_OUTPUT_BYTES - output->size) {
        output->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return heic_output_error(0);
    }
    required = output->size + size;
    if (required > output->capacity) {
        size_t capacity = output->capacity ? output->capacity : 64u * 1024u;
        uint8_t *next;
        while (capacity < required) {
            capacity = capacity > QW_HEIC_MAX_OUTPUT_BYTES / 2u
                ? QW_HEIC_MAX_OUTPUT_BYTES
                : capacity * 2u;
        }
        next = (uint8_t *)realloc(output->data, capacity);
        if (!next) {
            output->error = QW_STATUS_ERR_OUT_OF_MEMORY;
            return heic_output_error(1);
        }
        output->data = next;
        output->capacity = capacity;
    }
    if (size) memcpy(output->data + output->size, data, size);
    output->size = required;
    return (heif_error){heif_error_Ok, heif_suberror_Unspecified, ""};
}

int qw_heic_encode_page(const QwPageImage *page,
                        uint8_t **out_data,
                        size_t *out_size)
{
    heif_context *context = NULL;
    heif_encoder *encoder = NULL;
    heif_image *image = NULL;
    heif_writer writer;
    QwHeicOutputBuffer output;
    uint8_t *plane = NULL;
    size_t pixel_count;
    size_t source_size;
    size_t source_row_bytes;
    size_t row_bytes;
    size_t plane_stride = 0u;
    unsigned channels;
    heif_error error;
    int rc = QW_STATUS_ERR_UNSUPPORTED;

    if (!page || !out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    memset(&output, 0, sizeof(output));
    if (!page->data) return QW_STATUS_ERR_FORMAT;
    if (!qw_image_dimensions_within_limits(page->width, page->height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
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
        !qw_checked_size_mul((size_t)page->width, channels,
                             &source_row_bytes) ||
        !qw_checked_size_mul((size_t)page->width, 3u, &row_bytes) ||
        page->size < source_size) {
        return QW_STATUS_ERR_FORMAT;
    }

    context = heif_context_alloc();
    if (!context) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    heic_set_security_limits(context);
    error = heif_context_get_encoder_for_format(
        context, heif_compression_HEVC, &encoder);
    if (error.code != heif_error_Ok) {
        rc = heic_error_to_status(error);
        goto cleanup;
    }
    error = heif_encoder_set_lossy_quality(encoder, 90);
    if (error.code != heif_error_Ok) {
        rc = heic_error_to_status(error);
        goto cleanup;
    }
    error = heif_image_create((int)page->width, (int)page->height,
                              heif_colorspace_RGB,
                              heif_chroma_interleaved_RGB, &image);
    if (error.code != heif_error_Ok) {
        rc = heic_error_to_status(error);
        goto cleanup;
    }
    error = heif_image_add_plane_safe(image, heif_channel_interleaved,
                                      (int)page->width, (int)page->height, 24,
                                      heif_context_get_security_limits(context));
    if (error.code != heif_error_Ok) {
        rc = heic_error_to_status(error);
        goto cleanup;
    }
    plane = heif_image_get_plane2(image, heif_channel_interleaved,
                                  &plane_stride);
    if (!plane || plane_stride < row_bytes) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    for (uint32_t y = 0u; y < page->height; ++y) {
        const uint8_t *source_row = page->data + (size_t)y * source_row_bytes;
        uint8_t *destination_row = plane + (size_t)y * (size_t)plane_stride;
        if (channels == 3u) {
            memcpy(destination_row, source_row, row_bytes);
        } else {
            for (uint32_t x = 0u; x < page->width; ++x) {
                uint8_t gray = source_row[x];
                destination_row[(size_t)x * 3u] = gray;
                destination_row[(size_t)x * 3u + 1u] = gray;
                destination_row[(size_t)x * 3u + 2u] = gray;
            }
        }
    }
    error = heif_context_encode_image(context, image, encoder, NULL, NULL);
    if (error.code != heif_error_Ok) {
        rc = heic_error_to_status(error);
        goto cleanup;
    }
    writer.writer_api_version = 1;
    writer.write = heic_write_output;
    error = heif_context_write(context, &writer, &output);
    if (output.error != QW_STATUS_OK) {
        rc = output.error;
        goto cleanup;
    }
    if (error.code != heif_error_Ok) {
        rc = heic_error_to_status(error);
        goto cleanup;
    }
    if (!output.data || !output.size) {
        rc = QW_STATUS_ERR_DECODE;
        goto cleanup;
    }
    *out_data = output.data;
    *out_size = output.size;
    output.data = NULL;
    rc = QW_STATUS_OK;

cleanup:
    free(output.data);
    if (image) heif_image_release(image);
    if (encoder) heif_encoder_release(encoder);
    if (context) heif_context_free(context);
    return rc;
}
