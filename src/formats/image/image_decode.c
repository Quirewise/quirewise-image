#include "image_internal.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"
#include "stb_image.h"
#include "tiff_decode.h"
#include <webp/decode.h>
#include <webp/demux.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct QwWebPAnimation {
    const uint8_t *encoded;
    uint8_t *owned_encoded;
    WebPAnimDecoder *decoder;
    size_t frame_count;
    size_t next_frame;
    size_t pixel_count;
    size_t rgb_size;
    uint32_t width;
    uint32_t height;
};



void qw_webp_animation_free(QwWebPAnimation *animation)
{
    if (!animation) return;
    if (animation->decoder) WebPAnimDecoderDelete(animation->decoder);
    free(animation->owned_encoded);
    free(animation);
}

static int qw_webp_animation_create_impl(const void *data, size_t size,
                                         int copy_encoded,
                                         QwWebPAnimation **out_animation,
                                         size_t *out_frame_count)
{
    QwWebPAnimation *animation;
    WebPAnimDecoderOptions options;
    WebPAnimInfo info;
    WebPData webp_data;
    uint32_t parsed_width;
    uint32_t parsed_height;
    int has_animation;
    int parse_rc;
    size_t pixel_count;
    size_t rgb_size;
    if (!data || !size || !out_animation || !out_frame_count) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_animation = NULL;
    *out_frame_count = 0;
    if (!qw_webp_probe(data, size)) return QW_STATUS_ERR_UNSUPPORTED;
    parse_rc = qw_webp_parse(data, size, &parsed_width, &parsed_height,
                             &has_animation);
    if (parse_rc != QW_STATUS_OK) return parse_rc;
    if (!has_animation) return QW_STATUS_ERR_UNSUPPORTED;
    if (!qw_image_dimensions_within_limits(parsed_width, parsed_height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (!WebPAnimDecoderOptionsInit(&options)) return QW_STATUS_ERR_UNSUPPORTED;
    options.color_mode = MODE_RGBA;
    options.use_threads = 1;
    animation = (QwWebPAnimation *)calloc(1, sizeof(*animation));
    if (!animation) return QW_STATUS_ERR_OUT_OF_MEMORY;
    animation->encoded = (const uint8_t *)data;
    if (copy_encoded) {
        animation->owned_encoded = (uint8_t *)malloc(size);
        if (!animation->owned_encoded) {
            free(animation);
            return QW_STATUS_ERR_OUT_OF_MEMORY;
        }
        memcpy(animation->owned_encoded, data, size);
        animation->encoded = animation->owned_encoded;
    }
    webp_data.bytes = animation->encoded;
    webp_data.size = size;
    animation->decoder = WebPAnimDecoderNew(&webp_data, &options);
    if (!animation->decoder ||
        !WebPAnimDecoderGetInfo(animation->decoder, &info) ||
        !info.frame_count || !info.canvas_width || !info.canvas_height) {
        qw_webp_animation_free(animation);
        return QW_STATUS_ERR_DECODE;
    }
    if (info.canvas_width != parsed_width ||
        info.canvas_height != parsed_height) {
        qw_webp_animation_free(animation);
        return QW_STATUS_ERR_FORMAT;
    }
    if (!qw_image_dimensions_within_limits(info.canvas_width,
                                           info.canvas_height)) {
        qw_webp_animation_free(animation);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (!qw_checked_size_mul((size_t)info.canvas_width,
                               (size_t)info.canvas_height,
                               &pixel_count) ||
        pixel_count > SIZE_MAX / 4u ||
        !qw_checked_size_mul(pixel_count, 3u, &rgb_size)) {
        qw_webp_animation_free(animation);
        return QW_STATUS_ERR_FORMAT;
    }
    animation->frame_count = info.frame_count;
    animation->width = info.canvas_width;
    animation->height = info.canvas_height;
    animation->pixel_count = pixel_count;
    animation->rgb_size = rgb_size;
    *out_frame_count = animation->frame_count;
    *out_animation = animation;
    return QW_STATUS_OK;
}

int qw_webp_animation_create(const void *data, size_t size,
                             QwWebPAnimation **out_animation,
                             size_t *out_frame_count)
{
    return qw_webp_animation_create_impl(data, size, 1,
                                         out_animation, out_frame_count);
}

int qw_webp_animation_create_borrowed(const void *data, size_t size,
                                     QwWebPAnimation **out_animation,
                                     size_t *out_frame_count)
{
    return qw_webp_animation_create_impl(data, size, 0,
                                         out_animation, out_frame_count);
}

int qw_webp_animation_read(QwWebPAnimation *animation, size_t index,
                           QwPageImage *out_page)
{
    uint8_t *rgba = NULL;
    uint8_t *rgb = NULL;
    size_t pixels;
    size_t output_size;
    int timestamp;
    if (!animation || !out_page || index >= animation->frame_count) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    pixels = animation->pixel_count;
    output_size = animation->rgb_size;
    memset(out_page, 0, sizeof(*out_page));
    if (index < animation->next_frame) {
        WebPAnimDecoderReset(animation->decoder);
        animation->next_frame = 0;
    }
    while (animation->next_frame <= index) {
        if (!WebPAnimDecoderGetNext(animation->decoder, &rgba, &timestamp) || !rgba) {
            return QW_STATUS_ERR_DECODE;
        }
        if (animation->next_frame == index) {
            rgb = (uint8_t *)malloc(output_size ? output_size : 1u);
            if (!rgb) return QW_STATUS_ERR_OUT_OF_MEMORY;
            for (size_t i = 0; i < pixels; ++i) {
                uint32_t alpha = rgba[i * 4u + 3u];
                for (size_t channel = 0; channel < 3u; ++channel) {
                    uint32_t color = rgba[i * 4u + channel];
                    rgb[i * 3u + channel] = (uint8_t)(
                        (color * alpha + 255u * (255u - alpha) + 127u) / 255u);
                }
            }
        }
        ++animation->next_frame;
    }
    out_page->data = rgb;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = animation->width;
    out_page->height = animation->height;
    return QW_STATUS_OK;
}

int qw_image_decode_page(const void *data,
                         size_t size,
                         QwPageImage *out_page)
{
    stbi_uc *decoded = NULL;
    uint8_t *pixels = NULL;
    int width = 0;
    int height = 0;
    int channels = 0;
    int decoded_by_webp = 0;
    uint32_t webp_width = 0u;
    uint32_t webp_height = 0u;
    int webp_has_animation = 0;
    size_t pixel_count;
    size_t output_size;
    size_t output_channels;
    uint32_t jpeg_width = 0u;
    uint32_t jpeg_height = 0u;
    int jpeg_parsed = 0;
    int rc = QW_STATUS_ERR_DECODE;

    if (!data || !size || !out_page) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    memset(out_page, 0, sizeof(*out_page));
    if (qw_png_probe(data, size)) {
        return qw_png_decode_page(data, size, out_page);
    }
    if (qw_bmp_probe(data, size)) {
        return qw_bmp_decode_page(data, size, out_page);
    }
    if (qw_gif_probe(data, size)) {
        return qw_gif_decode_page(data, size, out_page);
    }
    if (qw_jpeg_probe(data, size)) {
        int jpeg_rc = qw_jpeg_parse(data, size, &jpeg_width, &jpeg_height);
        if (jpeg_rc != QW_STATUS_OK) return jpeg_rc;
        jpeg_parsed = 1;
    }
    if (qw_psd_probe(data, size)) {
        return qw_psd_decode_page(data, size, out_page);
    }
    if (qw_hdr_probe(data, size)) {
        return qw_hdr_decode_page(data, size, out_page);
    }
    if (qw_avif_probe(data, size)) {
        return qw_avif_decode_page(data, size, out_page);
    }
    if (qw_heic_probe(data, size)) {
        return qw_heic_decode_page(data, size, out_page);
    }
    if (qw_jxl_probe(data, size)) {
        return qw_jxl_decode_page(data, size, out_page);
    }
    if (qw_pnm_probe(data, size) || qw_pam_probe(data, size)) {
        return qw_pnm_decode_page(data, size, out_page);
    }
    if (qw_qoi_probe(data, size)) {
        return qw_qoi_decode_page(data, size, out_page);
    }
    if (qw_wbmp_probe(data, size)) {
        return qw_wbmp_decode_page(data, size, out_page);
    }
    if (qw_pcx_probe(data, size)) {
        return qw_pcx_decode_page(data, size, out_page);
    }
    if (qw_pfm_probe(data, size)) {
        return qw_pfm_decode_page(data, size, out_page);
    }
    if (qw_dds_probe(data, size)) {
        return qw_dds_decode_page(data, size, out_page);
    }
    if (qw_xpm_probe(data, size)) {
        return qw_xpm_decode_page(data, size, out_page);
    }
    if (qw_sgi_probe(data, size)) {
        return qw_sgi_decode_page(data, size, out_page);
    }
    if (qw_ras_probe(data, size)) {
        return qw_ras_decode_page(data, size, out_page);
    }
    if (qw_dcm_probe(data, size)) {
        return qw_dcm_decode_page(data, size, out_page);
    }
    if (qw_psb_probe(data, size)) {
        return qw_psb_decode_page(data, size, out_page);
    }
    if (qw_miff_probe(data, size)) {
        return qw_miff_decode_page(data, size, out_page);
    }
    if (qw_mng_probe(data, size)) {
        return qw_mng_decode_page(data, size, out_page);
    }
    if (qw_exr_probe(data, size)) {
        return qw_exr_decode_page(data, size, out_page);
    }
    if (qw_raw_probe(data, size)) {
        return qw_raw_decode_page(data, size, out_page);
    }
    if (qw_xcf_probe(data, size)) {
        return qw_xcf_decode_page(data, size, out_page);
    }
    if (qw_pcd_probe(data, size)) {
        return qw_pcd_decode_page(data, size, out_page);
    }
    if (qw_xbm_probe(data, size)) {
        return qw_xbm_decode_page(data, size, out_page);
    }
    if (qw_ico_probe(data, size)) {
        return qw_icon_decode_page(data, size, QW_ICON_CONTAINER_ICO,
                                   0u, out_page);
    }
    if (qw_cur_probe(data, size)) {
        return qw_icon_decode_page(data, size, QW_ICON_CONTAINER_CUR,
                                   0u, out_page);
    }
    if (qw_tiff_probe(data, size)) {
        return qw_tiff_decode_page((const uint8_t *)data, size, 0, out_page);
    }
    if (qw_tga_probe(data, size)) {
        return qw_tga_decode_page(data, size, out_page);
    }
    if (qw_pic_probe(data, size)) {
        return qw_pic_decode_page(data, size, out_page);
    }
    if (qw_webp_probe(data, size)) {
        rc = qw_webp_parse(data, size, &webp_width, &webp_height,
                           &webp_has_animation);
        if (rc != QW_STATUS_OK) return rc;
        if (webp_has_animation) return QW_STATUS_ERR_UNSUPPORTED;
        if (!qw_image_dimensions_within_limits(webp_width, webp_height))
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        decoded = WebPDecodeRGBA((const uint8_t *)data, size, &width, &height);
        channels = 4;
        decoded_by_webp = 1;
    } else {
        /* JPEG structure is validated by the project parser above. stb_image
         * remains the pixel decoder, but does not select the input format. */
        if (!jpeg_parsed) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if (size > INT_MAX) {
            return QW_STATUS_ERR_INVALID_ARGUMENT;
        }
        if (!qw_image_dimensions_within_limits(jpeg_width, jpeg_height)) {
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
        decoded = stbi_load_from_memory((const stbi_uc *)data, (int)size,
                                        &width, &height, &channels, 0);
    }
    if (!decoded || width <= 0 || height <= 0 ||
        (channels != 1 && channels != 2 && channels != 3 && channels != 4)) {
        goto cleanup;
    }
    if (decoded_by_webp &&
        ((uint32_t)width != webp_width || (uint32_t)height != webp_height)) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    if (jpeg_parsed &&
        ((uint32_t)width != jpeg_width || (uint32_t)height != jpeg_height)) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    if (!qw_image_dimensions_within_limits((uint32_t)width,
                                           (uint32_t)height)) {
        rc = QW_STATUS_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    if (!qw_checked_size_mul((size_t)width, (size_t)height, &pixel_count)) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    output_channels = (channels == 1 || channels == 2) ? 1u : 3u;
    if (!qw_checked_size_mul(pixel_count, output_channels, &output_size)) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    pixels = (uint8_t *)malloc(output_size ? output_size : 1);
    if (!pixels) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }

    if (channels == 1) {
        memcpy(pixels, decoded, output_size);
    } else if (channels == 3) {
        memcpy(pixels, decoded, output_size);
    } else {
        for (size_t i = 0; i < pixel_count; ++i) {
            if (channels == 2) {
                uint32_t gray = decoded[i * 2u];
                uint32_t alpha = decoded[i * 2u + 1u];
                pixels[i] = (uint8_t)((gray * alpha + 255u * (255u - alpha) + 127u) / 255u);
            } else {
                uint32_t alpha = decoded[i * 4u + 3u];
                for (size_t channel = 0; channel < 3u; ++channel) {
                    uint32_t color = decoded[i * 4u + channel];
                    pixels[i * 3u + channel] =
                        (uint8_t)((color * alpha + 255u * (255u - alpha) + 127u) / 255u);
                }
            }
        }
    }

    out_page->data = pixels;
    out_page->size = output_size;
    out_page->kind = output_channels == 1u
        ? QW_PAGE_IMAGE_GRAY8
        : QW_PAGE_IMAGE_RGB24;
    out_page->width = (uint32_t)width;
    out_page->height = (uint32_t)height;
    pixels = NULL;
    rc = QW_STATUS_OK;

cleanup:
    free(pixels);
    if (decoded) {
        if (decoded_by_webp) {
            WebPFree(decoded);
        } else {
            stbi_image_free(decoded);
        }
    }
    return rc;
}

void qw_image_release_page(QwPageImage *page)
{
    if (!page) {
        return;
    }
    free((void *)page->data);
    memset(page, 0, sizeof(*page));
}
