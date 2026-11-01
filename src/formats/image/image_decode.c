#include "image_internal.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"
#include "stb_image.h"
#include "tiff_decode.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct QwWebPAnimation {
    uint8_t *owned_encoded;
    QwWebPDocument document;
    uint8_t *canvas;
    size_t next_frame;
    size_t pixel_count;
    size_t rgb_size;
    size_t rgba_size;
    uint32_t width;
    uint32_t height;
};

/* Parse and decode every WebP frame through Quirewise-owned implementations. */
static int webp_decode_frame(const QwWebPFrame *frame, uint8_t **out_rgba)
{
    uint8_t *decoded = NULL;
    uint8_t *alpha = NULL;
    size_t pixel_count;
    int rc;

    if (!frame || !out_rgba || !frame->image_data || !frame->image_size ||
        !frame->width || !frame->height ||
        (frame->codec != QW_WEBP_CODEC_VP8 &&
         frame->codec != QW_WEBP_CODEC_VP8L)) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_rgba = NULL;
    if (!qw_image_dimensions_within_limits(frame->width, frame->height))
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    if (frame->codec == QW_WEBP_CODEC_VP8L) {
        rc = qw_webp_vp8l_decode(frame->image_data, frame->image_size,
                                 frame->width, frame->height, &decoded);
    } else {
        rc = qw_webp_vp8_decode_rgba(frame->image_data, frame->image_size,
                                     frame->width, frame->height, &decoded);
    }
    if (rc != QW_STATUS_OK) return rc;
    if (!decoded) return QW_STATUS_ERR_DECODE;
    if (frame->alpha_data) {
        if (!qw_checked_size_mul((size_t)frame->width,
                                 (size_t)frame->height, &pixel_count)) {
            free(decoded);
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
        rc = qw_webp_alpha_decode(frame->alpha_data, frame->alpha_size,
                                  frame->width, frame->height, &alpha);
        if (rc != QW_STATUS_OK) {
            free(decoded);
            free(alpha);
            return rc;
        }
        for (size_t i = 0u; i < pixel_count; ++i)
            decoded[i * 4u + 3u] = alpha[i];
        free(alpha);
    }
    *out_rgba = decoded;
    return QW_STATUS_OK;
}

static void webp_canvas_fill(QwWebPAnimation *animation)
{
    uint32_t argb = animation->document.background_argb;
    uint8_t red = (uint8_t)(argb >> 16);
    uint8_t green = (uint8_t)(argb >> 8);
    uint8_t blue = (uint8_t)argb;
    uint8_t alpha = (uint8_t)(argb >> 24);
    for (size_t i = 0u; i < animation->pixel_count; ++i) {
        animation->canvas[i * 4u] = red;
        animation->canvas[i * 4u + 1u] = green;
        animation->canvas[i * 4u + 2u] = blue;
        animation->canvas[i * 4u + 3u] = alpha;
    }
}

static void webp_canvas_fill_frame(QwWebPAnimation *animation,
                                   const QwWebPFrame *frame)
{
    uint32_t argb = animation->document.background_argb;
    uint8_t color[4] = {
        (uint8_t)(argb >> 16), (uint8_t)(argb >> 8),
        (uint8_t)argb, (uint8_t)(argb >> 24)
    };
    for (uint32_t y = 0u; y < frame->height; ++y) {
        size_t start = ((size_t)(frame->y + y) * animation->width + frame->x) * 4u;
        for (uint32_t x = 0u; x < frame->width; ++x) {
            memcpy(animation->canvas + start + (size_t)x * 4u,
                   color, sizeof(color));
        }
    }
}

static void webp_blend_pixel(uint8_t *destination, const uint8_t *source)
{
    uint32_t source_alpha = source[3];
    uint32_t destination_alpha = destination[3];
    uint32_t alpha_numerator = source_alpha * 255u +
        destination_alpha * (255u - source_alpha);
    if (!alpha_numerator) {
        memset(destination, 0, 4u);
        return;
    }
    for (size_t channel = 0u; channel < 3u; ++channel) {
        uint32_t numerator = (uint32_t)source[channel] * source_alpha * 255u +
            (uint32_t)destination[channel] * destination_alpha *
                (255u - source_alpha);
        destination[channel] = (uint8_t)(
            (numerator + alpha_numerator / 2u) / alpha_numerator);
    }
    destination[3] = (uint8_t)((alpha_numerator + 127u) / 255u);
}

static void webp_canvas_composite_frame(QwWebPAnimation *animation,
                                        const QwWebPFrame *frame,
                                        const uint8_t *rgba)
{
    for (uint32_t y = 0u; y < frame->height; ++y) {
        size_t destination_start =
            ((size_t)(frame->y + y) * animation->width + frame->x) * 4u;
        size_t source_start = (size_t)y * frame->width * 4u;
        for (uint32_t x = 0u; x < frame->width; ++x) {
            uint8_t *destination = animation->canvas + destination_start +
                                   (size_t)x * 4u;
            const uint8_t *source = rgba + source_start + (size_t)x * 4u;
            if (!frame->has_alpha || frame->blend) {
                memcpy(destination, source, 4u);
            } else {
                webp_blend_pixel(destination, source);
            }
        }
    }
}

void qw_webp_animation_free(QwWebPAnimation *animation)
{
    if (!animation) return;
    qw_webp_document_clear(&animation->document);
    free(animation->canvas);
    free(animation->owned_encoded);
    free(animation);
}

static int qw_webp_animation_create_impl(const void *data, size_t size,
                                         int copy_encoded,
                                         QwWebPAnimation **out_animation,
                                         size_t *out_frame_count)
{
    QwWebPAnimation *animation;
    QwWebPDocument document = {0};
    size_t pixel_count;
    size_t rgb_size;
    size_t rgba_size;
    size_t total_frame_pixels = 0u;
    int rc;
    if (!data || !size || !out_animation || !out_frame_count)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_animation = NULL;
    *out_frame_count = 0u;
    if (!qw_webp_probe(data, size)) return QW_STATUS_ERR_UNSUPPORTED;
    rc = qw_webp_document_parse(data, size, &document);
    if (rc != QW_STATUS_OK) return rc;
    if (!document.has_animation) {
        qw_webp_document_clear(&document);
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (!qw_image_dimensions_within_limits(document.width, document.height)) {
        qw_webp_document_clear(&document);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    for (size_t i = 0u; i < document.frame_count; ++i) {
        size_t frame_pixels;
        QwWebPFrame *frame = &document.frames[i];
        if (!qw_image_dimensions_within_limits(frame->width, frame->height) ||
            !qw_checked_size_mul((size_t)frame->width,
                                 (size_t)frame->height, &frame_pixels) ||
            frame_pixels > (size_t)1000000000u - total_frame_pixels) {
            qw_webp_document_clear(&document);
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
        total_frame_pixels += frame_pixels;
    }
    animation = (QwWebPAnimation *)calloc(1u, sizeof(*animation));
    if (!animation) {
        qw_webp_document_clear(&document);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    if (copy_encoded) {
        animation->owned_encoded = (uint8_t *)malloc(size);
        if (!animation->owned_encoded) {
            qw_webp_document_clear(&document);
            free(animation);
            return QW_STATUS_ERR_OUT_OF_MEMORY;
        }
        memcpy(animation->owned_encoded, data, size);
        qw_webp_document_clear(&document);
        rc = qw_webp_document_parse(animation->owned_encoded, size,
                                    &animation->document);
        if (rc != QW_STATUS_OK) {
            free(animation->owned_encoded);
            free(animation);
            return rc;
        }
    } else {
        animation->document = document;
        memset(&document, 0, sizeof(document));
    }
    if (!qw_checked_size_mul((size_t)animation->document.width,
                             (size_t)animation->document.height,
                             &pixel_count) ||
        pixel_count > SIZE_MAX / 4u ||
        !qw_checked_size_mul(pixel_count, 4u, &rgba_size) ||
        !qw_checked_size_mul(pixel_count, 3u, &rgb_size)) {
        qw_webp_animation_free(animation);
        return QW_STATUS_ERR_FORMAT;
    }
    animation->canvas = (uint8_t *)malloc(rgba_size ? rgba_size : 1u);
    if (!animation->canvas) {
        qw_webp_animation_free(animation);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    animation->width = animation->document.width;
    animation->height = animation->document.height;
    animation->pixel_count = pixel_count;
    animation->rgb_size = rgb_size;
    animation->rgba_size = rgba_size;
    webp_canvas_fill(animation);
    *out_frame_count = animation->document.frame_count;
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
    uint8_t *rgb;
    size_t output_size;
    if (!animation || !out_page || index >= animation->document.frame_count)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    output_size = animation->rgb_size;
    memset(out_page, 0, sizeof(*out_page));
    if (index < animation->next_frame) {
        webp_canvas_fill(animation);
        animation->next_frame = 0u;
    }
    while (animation->next_frame <= index) {
        size_t current = animation->next_frame;
        QwWebPFrame *frame = &animation->document.frames[current];
        uint8_t *rgba = NULL;
        int rc;
        if (current > 0u &&
            animation->document.frames[current - 1u].dispose) {
            webp_canvas_fill_frame(animation,
                &animation->document.frames[current - 1u]);
        }
        rc = webp_decode_frame(frame, &rgba);
        if (rc != QW_STATUS_OK) return rc;
        webp_canvas_composite_frame(animation, frame, rgba);
        free(rgba);
        ++animation->next_frame;
    }
    rgb = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!rgb) return QW_STATUS_ERR_OUT_OF_MEMORY;
    for (size_t i = 0u; i < animation->pixel_count; ++i) {
        uint32_t alpha = animation->canvas[i * 4u + 3u];
        for (size_t channel = 0u; channel < 3u; ++channel) {
            uint32_t color = animation->canvas[i * 4u + channel];
            rgb[i * 3u + channel] = (uint8_t)(
                (color * alpha + 255u * (255u - alpha) + 127u) / 255u);
        }
    }
    out_page->data = rgb;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = animation->width;
    out_page->height = animation->height;
    return QW_STATUS_OK;
}

int qw_webp_animation_frame_duration(const QwWebPAnimation *animation,
                                     size_t index,
                                     uint32_t *out_duration_ms)
{
    if (!animation || !out_duration_ms ||
        index >= animation->document.frame_count) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_duration_ms = animation->document.frames[index].duration_ms;
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
    if (qw_jp2_probe(data, size)) {
        return qw_jp2_decode_page(data, size, out_page);
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
        QwWebPDocument document = {0};
        QwWebPFrame *frame;
        rc = qw_webp_document_parse((const uint8_t *)data, size, &document);
        if (rc != QW_STATUS_OK) return rc;
        if (document.has_animation) {
            qw_webp_document_clear(&document);
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        webp_width = document.width;
        webp_height = document.height;
        if (!qw_image_dimensions_within_limits(webp_width, webp_height)) {
            qw_webp_document_clear(&document);
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
        frame = &document.frames[0];
        {
            uint8_t *webp_rgba = NULL;
            rc = webp_decode_frame(frame, &webp_rgba);
            qw_webp_document_clear(&document);
            if (rc != QW_STATUS_OK) return rc;
            decoded = webp_rgba;
            width = (int)webp_width;
            height = (int)webp_height;
            decoded_by_webp = 1;
        }
        channels = 4;
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
            free(decoded);
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
