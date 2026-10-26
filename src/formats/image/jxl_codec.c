#include "image_internal.h"

#include <jxl/cms.h>
#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/encode.h>

#include <stdlib.h>
#include <string.h>

int qw_jxl_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    JxlDecoder *decoder = NULL;
    const JxlCmsInterface *cms;
    JxlBasicInfo info;
    JxlColorEncoding output_color;
    JxlPixelFormat pixel_format;
    uint8_t *decoded = NULL;
    uint8_t *pixels = NULL;
    size_t pixel_count = 0u;
    size_t decoded_size = 0u;
    size_t output_size = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    unsigned color_channels = 0u;
    unsigned decoded_channels = 0u;
    unsigned frame_count = 0u;
    int have_basic_info = 0;
    int have_color_encoding = 0;
    int have_output_buffer = 0;
    int have_full_image = 0;
    int rc = QW_STATUS_ERR_DECODE;

    if (!data || !size || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    if (!qw_jxl_probe(data, size)) return QW_STATUS_ERR_FORMAT;

    decoder = JxlDecoderCreate(NULL);
    if (!decoder) return QW_STATUS_ERR_OUT_OF_MEMORY;
    cms = JxlGetDefaultCms();
    if (!cms ||
        JxlDecoderSetCms(decoder, *cms) != JXL_DEC_SUCCESS ||
        JxlDecoderSetKeepOrientation(decoder, JXL_FALSE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetUnpremultiplyAlpha(decoder, JXL_TRUE) != JXL_DEC_SUCCESS ||
        JxlDecoderSubscribeEvents(decoder,
            JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING |
            JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetInput(decoder, (const uint8_t *)data, size) !=
            JXL_DEC_SUCCESS) {
        rc = QW_STATUS_ERR_UNSUPPORTED;
        goto cleanup;
    }
    JxlDecoderCloseInput(decoder);

    for (;;) {
        JxlDecoderStatus status = JxlDecoderProcessInput(decoder);
        if (status == JXL_DEC_BASIC_INFO) {
            if (JxlDecoderGetBasicInfo(decoder, &info) != JXL_DEC_SUCCESS) {
                rc = QW_STATUS_ERR_FORMAT;
                goto cleanup;
            }
            have_basic_info = 1;
            if (!info.xsize || !info.ysize) {
                rc = QW_STATUS_ERR_FORMAT;
                goto cleanup;
            }
            if (!qw_image_dimensions_within_limits(info.xsize, info.ysize)) {
                rc = QW_STATUS_ERR_RESOURCE_LIMIT;
                goto cleanup;
            }
            if (info.have_animation ||
                (info.num_color_channels != 1u &&
                 info.num_color_channels != 3u) ||
                info.num_extra_channels != (info.alpha_bits ? 1u : 0u)) {
                rc = QW_STATUS_ERR_UNSUPPORTED;
                goto cleanup;
            }
            width = info.xsize;
            height = info.ysize;
            color_channels = info.num_color_channels;
            decoded_channels = color_channels + (info.alpha_bits ? 1u : 0u);
            if (!qw_checked_size_mul((size_t)width, (size_t)height,
                                     &pixel_count) ||
                !qw_checked_size_mul(pixel_count, color_channels,
                                     &output_size)) {
                rc = QW_STATUS_ERR_RESOURCE_LIMIT;
                goto cleanup;
            }
        } else if (status == JXL_DEC_COLOR_ENCODING) {
            if (!have_basic_info) {
                rc = QW_STATUS_ERR_FORMAT;
                goto cleanup;
            }
            JxlColorEncodingSetToSRGB(&output_color,
                                      color_channels == 1u
                                          ? JXL_TRUE : JXL_FALSE);
            if (JxlDecoderSetOutputColorProfile(decoder, &output_color,
                                                NULL, 0u) != JXL_DEC_SUCCESS) {
                rc = QW_STATUS_ERR_UNSUPPORTED;
                goto cleanup;
            }
            have_color_encoding = 1;
        } else if (status == JXL_DEC_FRAME) {
            if (++frame_count > 1u) {
                rc = QW_STATUS_ERR_UNSUPPORTED;
                goto cleanup;
            }
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            if (!have_basic_info || !have_color_encoding ||
                have_output_buffer) {
                rc = QW_STATUS_ERR_FORMAT;
                goto cleanup;
            }
            pixel_format.num_channels = decoded_channels;
            pixel_format.data_type = JXL_TYPE_UINT8;
            pixel_format.endianness = JXL_NATIVE_ENDIAN;
            pixel_format.align = 0u;
            if (JxlDecoderImageOutBufferSize(decoder, &pixel_format,
                                             &decoded_size) != JXL_DEC_SUCCESS ||
                decoded_size < pixel_count * decoded_channels ||
                decoded_size > QW_IMAGE_MAX_DECODED_PIXELS * 4u) {
                rc = QW_STATUS_ERR_RESOURCE_LIMIT;
                goto cleanup;
            }
            decoded = (uint8_t *)malloc(decoded_size ? decoded_size : 1u);
            if (!decoded) {
                rc = QW_STATUS_ERR_OUT_OF_MEMORY;
                goto cleanup;
            }
            if (JxlDecoderSetImageOutBuffer(decoder, &pixel_format, decoded,
                                            decoded_size) != JXL_DEC_SUCCESS) {
                rc = QW_STATUS_ERR_DECODE;
                goto cleanup;
            }
            have_output_buffer = 1;
        } else if (status == JXL_DEC_FULL_IMAGE) {
            have_full_image = 1;
        } else if (status == JXL_DEC_SUCCESS) {
            if (!have_basic_info || !have_color_encoding ||
                !have_output_buffer || !have_full_image || frame_count != 1u) {
                rc = QW_STATUS_ERR_FORMAT;
                goto cleanup;
            }
            break;
        } else if (status == JXL_DEC_ERROR ||
                   status == JXL_DEC_NEED_MORE_INPUT) {
            rc = QW_STATUS_ERR_DECODE;
            goto cleanup;
        } else {
            rc = QW_STATUS_ERR_UNSUPPORTED;
            goto cleanup;
        }
    }

    pixels = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!pixels) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    if (decoded_channels == color_channels) {
        memcpy(pixels, decoded, output_size);
    } else if (color_channels == 1u) {
        for (size_t i = 0u; i < pixel_count; ++i) {
            uint32_t gray = decoded[i * 2u];
            uint32_t alpha = decoded[i * 2u + 1u];
            pixels[i] = (uint8_t)((gray * alpha + 255u * (255u - alpha) +
                                   127u) / 255u);
        }
    } else {
        for (size_t i = 0u; i < pixel_count; ++i) {
            uint32_t alpha = decoded[i * 4u + 3u];
            for (size_t channel = 0u; channel < 3u; ++channel) {
                uint32_t color = decoded[i * 4u + channel];
                pixels[i * 3u + channel] =
                    (uint8_t)((color * alpha + 255u * (255u - alpha) +
                               127u) / 255u);
            }
        }
    }

    out_page->data = pixels;
    out_page->size = output_size;
    out_page->kind = color_channels == 1u
        ? QW_PAGE_IMAGE_GRAY8 : QW_PAGE_IMAGE_RGB24;
    out_page->width = width;
    out_page->height = height;
    pixels = NULL;
    rc = QW_STATUS_OK;

cleanup:
    free(pixels);
    free(decoded);
    if (decoder) JxlDecoderDestroy(decoder);
    return rc;
}

int qw_jxl_encode_page(const QwPageImage *page,
                       uint8_t **out_data,
                       size_t *out_size)
{
    JxlEncoder *encoder = NULL;
    JxlEncoderFrameSettings *settings;
    JxlBasicInfo info;
    JxlColorEncoding color_encoding;
    JxlPixelFormat pixel_format;
    uint8_t *encoded = NULL;
    size_t capacity = 0u;
    size_t pixel_count;
    size_t source_size;
    size_t output_size = 0u;
    unsigned channels;
    JxlEncoderStatus status;
    int rc = QW_STATUS_ERR_UNSUPPORTED;

    if (!page || !out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    if (!page->data || !page->width || !page->height) {
        return QW_STATUS_ERR_FORMAT;
    }
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
        page->size < source_size) {
        return QW_STATUS_ERR_FORMAT;
    }

    encoder = JxlEncoderCreate(NULL);
    if (!encoder) return QW_STATUS_ERR_OUT_OF_MEMORY;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = page->width;
    info.ysize = page->height;
    info.bits_per_sample = 8u;
    info.num_color_channels = channels;
    info.num_extra_channels = 0u;
    info.alpha_bits = 0u;
    info.have_animation = JXL_FALSE;
    info.orientation = JXL_ORIENT_IDENTITY;
    info.uses_original_profile = JXL_TRUE;
    status = JxlEncoderSetBasicInfo(encoder, &info);
    if (status != JXL_ENC_SUCCESS) goto cleanup;
    JxlColorEncodingSetToSRGB(&color_encoding,
                              channels == 1u ? JXL_TRUE : JXL_FALSE);
    status = JxlEncoderSetColorEncoding(encoder, &color_encoding);
    if (status != JXL_ENC_SUCCESS) goto cleanup;
    settings = JxlEncoderFrameSettingsCreate(encoder, NULL);
    if (!settings) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    status = JxlEncoderSetFrameLossless(settings, JXL_TRUE);
    if (status != JXL_ENC_SUCCESS) goto cleanup;
    status = JxlEncoderFrameSettingsSetOption(
        settings, JXL_ENC_FRAME_SETTING_EFFORT, 7);
    if (status != JXL_ENC_SUCCESS) goto cleanup;

    pixel_format.num_channels = channels;
    pixel_format.data_type = JXL_TYPE_UINT8;
    pixel_format.endianness = JXL_NATIVE_ENDIAN;
    pixel_format.align = 0u;
    status = JxlEncoderAddImageFrame(settings, &pixel_format, page->data,
                                     source_size);
    if (status != JXL_ENC_SUCCESS) goto cleanup;
    JxlEncoderCloseInput(encoder);

    capacity = 64u * 1024u;
    if (capacity > QW_JXL_MAX_OUTPUT_BYTES) capacity = QW_JXL_MAX_OUTPUT_BYTES;
    encoded = (uint8_t *)malloc(capacity);
    if (!encoded) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (;;) {
        uint8_t *next_out = encoded + output_size;
        size_t available = capacity - output_size;
        status = JxlEncoderProcessOutput(encoder, &next_out, &available);
        output_size = (size_t)(next_out - encoded);
        if (status == JXL_ENC_SUCCESS) break;
        if (status != JXL_ENC_NEED_MORE_OUTPUT) goto cleanup;
        if (capacity >= QW_JXL_MAX_OUTPUT_BYTES) {
            rc = QW_STATUS_ERR_RESOURCE_LIMIT;
            goto cleanup;
        }
        {
            size_t new_capacity = capacity > QW_JXL_MAX_OUTPUT_BYTES / 2u
                ? QW_JXL_MAX_OUTPUT_BYTES : capacity * 2u;
            uint8_t *new_encoded =
                (uint8_t *)realloc(encoded, new_capacity);
            if (!new_encoded) {
                rc = QW_STATUS_ERR_OUT_OF_MEMORY;
                goto cleanup;
            }
            encoded = new_encoded;
            capacity = new_capacity;
        }
    }
    if (!output_size || output_size > QW_JXL_MAX_OUTPUT_BYTES) {
        rc = output_size ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_DECODE;
        goto cleanup;
    }
    *out_data = encoded;
    *out_size = output_size;
    encoded = NULL;
    rc = QW_STATUS_OK;

cleanup:
    free(encoded);
    if (encoder) JxlEncoderDestroy(encoder);
    return rc;
}
