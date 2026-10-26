#include "image_internal.h"

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#include <stdlib.h>
#include <string.h>

static uint8_t pcd_clamp_rgb(double value)
{
    if (value <= 0.0) return 0u;
    if (value >= 255.0) return 255u;
    return (uint8_t)(value + 0.5);
}

int qw_pcd_probe(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    if (!bytes) return 0;
    return (size >= 7u && memcmp(bytes, "PCD_OPA", 7u) == 0) ||
           (size >= 0x807u && memcmp(bytes + 0x800u, "PCD_IPI", 7u) == 0);
}

int qw_pcd_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    const AVCodec *decoder;
    AVCodecContext *context = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    uint8_t *pixels = NULL;
    size_t pixel_count;
    size_t output_size;
    uint32_t width;
    uint32_t height;
    size_t chroma_width;
    int result;
    int rc = QW_STATUS_ERR_DECODE;
    if (!data || !size || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    if (!qw_pcd_probe(data, size)) return QW_STATUS_ERR_FORMAT;
    if (size > QW_PCD_MAX_INPUT_BYTES || size > INT_MAX) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }

    decoder = avcodec_find_decoder(AV_CODEC_ID_PHOTOCD);
    if (!decoder) return QW_STATUS_ERR_UNSUPPORTED;
    context = avcodec_alloc_context3(decoder);
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!context || !packet || !frame) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    context->max_pixels = (int64_t)QW_IMAGE_MAX_DECODED_PIXELS;
    context->thread_count = 1;
    context->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM |
                               AV_EF_BUFFER | AV_EF_EXPLODE;
    result = avcodec_open2(context, decoder, NULL);
    if (result < 0) {
        rc = result == AVERROR(ENOMEM) ? QW_STATUS_ERR_OUT_OF_MEMORY
                                       : QW_STATUS_ERR_UNSUPPORTED;
        goto cleanup;
    }
    result = av_new_packet(packet, (int)size);
    if (result < 0) {
        rc = result == AVERROR(ENOMEM) ? QW_STATUS_ERR_OUT_OF_MEMORY
                                       : QW_STATUS_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    memcpy(packet->data, data, size);
    result = avcodec_send_packet(context, packet);
    if (result < 0) {
        rc = result == AVERROR(ENOMEM) ? QW_STATUS_ERR_OUT_OF_MEMORY : QW_STATUS_ERR_DECODE;
        goto cleanup;
    }
    result = avcodec_receive_frame(context, frame);
    if (result < 0) {
        rc = result == AVERROR(ENOMEM) ? QW_STATUS_ERR_OUT_OF_MEMORY : QW_STATUS_ERR_DECODE;
        goto cleanup;
    }
    if (frame->format != AV_PIX_FMT_YUV420P || frame->width <= 0 ||
        frame->height <= 0 || !frame->data[0] || !frame->data[1] ||
        !frame->data[2]) {
        rc = QW_STATUS_ERR_UNSUPPORTED;
        goto cleanup;
    }
    width = (uint32_t)frame->width;
    height = (uint32_t)frame->height;
    if (!qw_image_dimensions_within_limits(width, height)) {
        rc = QW_STATUS_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    chroma_width = ((size_t)width + 1u) / 2u;
    if (frame->linesize[0] < frame->width ||
        frame->linesize[1] < (int)chroma_width ||
        frame->linesize[2] < (int)chroma_width ||
        !qw_checked_size_mul((size_t)width, (size_t)height, &pixel_count) ||
        !qw_checked_size_mul(pixel_count, 3u, &output_size)) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    pixels = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!pixels) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }

    for (uint32_t y = 0u; y < height; ++y) {
        const uint8_t *luma = frame->data[0] + (size_t)y *
                              (size_t)frame->linesize[0];
        const uint8_t *cb_row = frame->data[1] + (size_t)(y / 2u) *
                                (size_t)frame->linesize[1];
        const uint8_t *cr_row = frame->data[2] + (size_t)(y / 2u) *
                                (size_t)frame->linesize[2];
        for (uint32_t x = 0u; x < width; ++x) {
            int cb = (int)cb_row[x / 2u] - 128;
            int cr = (int)cr_row[x / 2u] - 128;
            double yy = (double)luma[x];
            size_t destination = ((size_t)y * width + x) * 3u;
            pixels[destination] = pcd_clamp_rgb(yy + 1.5748 * cr);
            pixels[destination + 1u] = pcd_clamp_rgb(
                yy - 0.1873 * cb - 0.4681 * cr);
            pixels[destination + 2u] = pcd_clamp_rgb(yy + 1.8556 * cb);
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
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    return rc;
}
