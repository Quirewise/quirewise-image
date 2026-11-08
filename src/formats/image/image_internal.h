#ifndef QUIREWISE_IMAGE_INTERNAL_H
#define QUIREWISE_IMAGE_INTERNAL_H

#include "quirewise/format.h"
#include "quirewise/document.h"
#include "../../core/architecture_metadata.h"
#include "../../core/byte_utils.h"
#include "../../core/page_document.h"

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

typedef struct QwWebPAnimation QwWebPAnimation;

typedef enum QwWebPCodec {
    QW_WEBP_CODEC_VP8 = 1,
    QW_WEBP_CODEC_VP8L = 2
} QwWebPCodec;

typedef struct QwWebPFrame {
    const uint8_t *alpha_data;
    size_t alpha_size;
    const uint8_t *image_data;
    size_t image_size;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t duration_ms;
    QwWebPCodec codec;
    uint8_t blend;
    uint8_t dispose;
    uint8_t has_alpha;
} QwWebPFrame;

typedef struct QwWebPDocument {
    QwWebPFrame *frames;
    size_t frame_count;
    uint32_t width;
    uint32_t height;
    uint32_t background_argb;
    uint16_t loop_count;
    uint8_t has_animation;
} QwWebPDocument;

typedef struct QwWebPVP8Header {
    uint32_t width;
    uint32_t height;
    uint32_t first_partition_size;
    size_t first_partition_offset;
    uint8_t version;
    uint8_t show_frame;
    uint8_t horizontal_scale;
    uint8_t vertical_scale;
    uint8_t color_space;
    uint8_t clamp_type;
    uint8_t segmentation_enabled;
    uint8_t segmentation_map_updated;
    uint8_t segmentation_data_updated;
    uint8_t segmentation_feature_mode;
    int8_t segment_quantizer_delta[4];
    int8_t segment_filter_delta[4];
    uint8_t segment_map_probability[3];
    uint8_t loop_filter_type;
    uint8_t loop_filter_level;
    uint8_t loop_filter_sharpness;
    uint8_t loop_filter_delta_enabled;
    int8_t reference_filter_delta[4];
    int8_t mode_filter_delta[4];
    uint8_t token_partition_count;
    uint8_t base_quantizer;
    int8_t quantizer_delta[5];
    uint8_t skip_coeff_enabled;
    uint8_t skip_coeff_probability;
    uint8_t coefficient_probabilities[4][8][3][11];
    size_t first_partition_position;
    uint32_t first_partition_range;
    uint32_t first_partition_value;
    uint8_t first_partition_bit_count;
} QwWebPVP8Header;

typedef struct QwWebPVP8Macroblock {
    uint8_t segment_id;
    uint8_t skip_coeff;
    uint8_t y_mode;
    uint8_t uv_mode;
    uint8_t b_mode[16];
} QwWebPVP8Macroblock;

typedef int (*QwWebPVP8MacroblockConsumer)(
    void *context,
    size_t macroblock_x,
    size_t macroblock_y,
    const QwWebPVP8Macroblock *macroblock,
    const int16_t coefficients[25][16]);

typedef struct QwWebPVP8TokenPartition {
    const uint8_t *data;
    size_t size;
} QwWebPVP8TokenPartition;

enum {
    QW_ICON_CONTAINER_ICO = 1,
    QW_ICON_CONTAINER_CUR = 2
};

#define QW_IMAGE_MAX_DIMENSION QW_INPUT_RESOURCE_MAX_IMAGE_DIMENSION
#define QW_IMAGE_MAX_DECODED_PIXELS QW_INPUT_RESOURCE_MAX_DECODED_PIXELS
#define QW_AVIF_MAX_OUTPUT_BYTES ((size_t)128u * 1024u * 1024u)
#define QW_HEIC_MAX_OUTPUT_BYTES ((size_t)128u * 1024u * 1024u)
#define QW_JXL_MAX_OUTPUT_BYTES ((size_t)128u * 1024u * 1024u)
#define QW_PCD_MAX_INPUT_BYTES ((size_t)64u * 1024u * 1024u)

int qw_heic_probe(const void *data, size_t size);
int qw_heic_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_heic_encode_page(const QwPageImage *page,
                        uint8_t **out_data,
                        size_t *out_size);
int qw_raw_probe(const void *data, size_t size);
int qw_raw_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_xcf_probe(const void *data, size_t size);
int qw_xcf_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_pcd_probe(const void *data, size_t size);
int qw_pcd_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_png_probe(const void *data, size_t size);
int qw_png_dimensions(const void *data, size_t size,
                     uint32_t *out_width, uint32_t *out_height);
int qw_png_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_bmp_probe(const void *data, size_t size);
int qw_bmp_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_tga_probe(const void *data, size_t size);
int qw_tga_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_pic_probe(const void *data, size_t size);
int qw_pic_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_webp_probe(const void *data, size_t size);
int qw_webp_parse(const void *data,
                  size_t size,
                  uint32_t *out_width,
                  uint32_t *out_height,
                  int *out_has_animation);
int qw_webp_document_parse(const void *data,
                           size_t size,
                           QwWebPDocument *out_document);
void qw_webp_document_clear(QwWebPDocument *document);
int qw_webp_vp8l_decode(const uint8_t *data,
                        size_t size,
                        uint32_t expected_width,
                        uint32_t expected_height,
                        uint8_t **out_rgba);
int qw_webp_alpha_decode(const uint8_t *data,
                         size_t size,
                         uint32_t width,
                         uint32_t height,
                         uint8_t **out_alpha);
int qw_webp_vp8_parse_keyframe_header(const uint8_t *data,
                                      size_t size,
                                      uint32_t expected_width,
                                      uint32_t expected_height,
                                      QwWebPVP8Header *out_header);
int qw_webp_vp8_decode_macroblock_modes(
    const uint8_t *data,
    size_t size,
    const QwWebPVP8Header *header,
    QwWebPVP8Macroblock **out_macroblocks,
    size_t *out_macroblock_count);
int qw_webp_vp8_decode_coefficients(
    const uint8_t *data,
    size_t size,
    const QwWebPVP8Header *header,
    const QwWebPVP8Macroblock *macroblocks,
    size_t macroblock_count,
    QwWebPVP8MacroblockConsumer consumer,
    void *consumer_context);
int qw_webp_vp8_parse_token_partitions(
    const uint8_t *data,
    size_t size,
    const QwWebPVP8Header *header,
    QwWebPVP8TokenPartition out_partitions[8],
    size_t *out_partition_count);
int qw_webp_vp8_decode_token_coefficients(
    const uint8_t *data,
    size_t size,
    const QwWebPVP8Header *header,
    const QwWebPVP8Macroblock *macroblocks,
    size_t macroblock_count);
int qw_webp_vp8_decode_rgba(const uint8_t *data,
                            size_t size,
                            uint32_t expected_width,
                            uint32_t expected_height,
                            uint8_t **out_rgba);
int qw_webp_vp8_bpred_above_right(const uint8_t *plane,
                                  size_t stride,
                                  size_t macroblock_columns,
                                  size_t macroblock_x,
                                  size_t macroblock_y,
                                  uint8_t out_samples[4]);
int qw_gif_probe(const void *data, size_t size);
int qw_gif_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_jpeg_probe(const void *data, size_t size);
int qw_jpeg_parse(const void *data,
                  size_t size,
                  uint32_t *out_width,
                  uint32_t *out_height);
int qw_psd_probe(const void *data, size_t size);
int qw_psd_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_hdr_probe(const void *data, size_t size);
int qw_hdr_decode_page(const void *data, size_t size, QwPageImage *out_page);

static inline int qw_image_dimensions_within_limits(uint32_t width,
                                                    uint32_t height)
{
    return width != 0u && height != 0u &&
           width <= QW_IMAGE_MAX_DIMENSION &&
           height <= QW_IMAGE_MAX_DIMENSION &&
           (size_t)width <= QW_IMAGE_MAX_DECODED_PIXELS / (size_t)height;
}

int qw_image_decode_page(const void *data,
                         size_t size,
                         QwPageImage *out_page);
void qw_image_release_page(QwPageImage *page);

/* Shared lifecycle for writers that consume exactly one page image.  The
 * provider-owned page is released after every acquire, including failed
 * acquires; encoded page payloads can optionally be decoded into a temporary
 * RGB/gray image before the format-specific writer runs. */
typedef struct QwImageWriterPage {
    QwPageImage page;
    QwPageImage decoded;
    const QwPageImage *source;
    int acquired;
    int decoded_owned;
} QwImageWriterPage;

static inline int qw_image_writer_acquire(const QwPageDocument *document,
                                          int decode_encoded,
                                          QwImageWriterPage *state)
{
    int rc;
    if (!document || !state) return QW_STATUS_ERR_INVALID_ARGUMENT;
    state->page = (QwPageImage){0};
    state->decoded = (QwPageImage){0};
    state->source = &state->page;
    state->acquired = 0;
    state->decoded_owned = 0;
    if (document->kind != QW_DOCUMENT_PAGE_SOURCE ||
        document->page_count != 1u || !document->acquire_page) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    rc = qw_page_document_acquire(document, 0u, &state->page);
    state->acquired = 1;
    if (rc != QW_STATUS_OK || !decode_encoded ||
        state->page.kind == QW_PAGE_IMAGE_GRAY8 ||
        state->page.kind == QW_PAGE_IMAGE_RGB24) {
        return rc;
    }
    rc = qw_image_decode_page(state->page.data, state->page.size,
                              &state->decoded);
    if (rc == QW_STATUS_OK) {
        state->source = &state->decoded;
        state->decoded_owned = 1;
    }
    return rc;
}

static inline void qw_image_writer_release(const QwPageDocument *document,
                                           QwImageWriterPage *state)
{
    if (!state) return;
    if (state->decoded_owned) qw_image_release_page(&state->decoded);
    if (state->acquired && document && document->release_page) {
        qw_page_document_release(document, &state->page);
    }
    state->source = NULL;
    state->acquired = 0;
    state->decoded_owned = 0;
}

static inline int qw_image_stream_write(FILE *stream,
                                        const void *data,
                                        size_t size,
                                        size_t max_output_bytes,
                                        size_t *written)
{
    if (!stream || !written || (size && !data) || size > SIZE_MAX - *written) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (max_output_bytes &&
        (*written > max_output_bytes || size > max_output_bytes - *written)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (size && fwrite(data, 1u, size, stream) != size) return QW_STATUS_ERR_IO;
    *written += size;
    return QW_STATUS_OK;
}

int qw_png_write_page_stream_payload(const QwPageDocument *document,
                                    const QwConvertOptions *options,
                                    FILE *stream,
                                    size_t max_output_bytes,
                                    size_t *out_size,
                                    size_t *out_pages);
int qw_qoi_probe(const void *data, size_t size);
int qw_qoi_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_qoi_encode_page(const QwPageImage *page,
                       uint8_t **out_data,
                       size_t *out_size);
int qw_wbmp_probe(const void *data, size_t size);
int qw_wbmp_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_wbmp_encode_page(const QwPageImage *page,
                        uint8_t **out_data,
                        size_t *out_size);
int qw_pnm_probe(const void *data, size_t size);
int qw_pam_probe(const void *data, size_t size);
int qw_pnm_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_pnm_encode_page(const QwPageImage *page, int pam,
                       uint8_t **out_data, size_t *out_size);
int qw_xbm_probe(const void *data, size_t size);
int qw_xbm_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_xbm_encode_page(const QwPageImage *page,
                       uint8_t **out_data, size_t *out_size);
int qw_pcx_probe(const void *data, size_t size);
int qw_pcx_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_pcx_encode_page(const QwPageImage *page,
                       uint8_t **out_data, size_t *out_size);
int qw_tga_encode_page(const QwPageImage *page,
                       uint8_t **out_data, size_t *out_size);
int qw_pfm_probe(const void *data, size_t size);
int qw_pfm_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_icns_probe(const void *data, size_t size);
int qw_icns_page_count(const void *data, size_t size, size_t *out_count);
int qw_icns_decode_page(const void *data, size_t size, size_t page_index,
                        QwPageImage *out_page);
int qw_dds_probe(const void *data, size_t size);
int qw_dds_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_xpm_probe(const void *data, size_t size);
int qw_xpm_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_sgi_probe(const void *data, size_t size);
int qw_sgi_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_ras_probe(const void *data, size_t size);
int qw_ras_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_dcm_probe(const void *data, size_t size);
int qw_dcm_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_psb_probe(const void *data, size_t size);
int qw_psb_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_miff_probe(const void *data, size_t size);
int qw_miff_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_mng_probe(const void *data, size_t size);
int qw_mng_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_apng_probe(const void *data, size_t size);
int qw_exr_probe(const void *data, size_t size);
int qw_exr_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_jp2_probe(const void *data, size_t size);
int qw_jp2_validate_structure(const void *data, size_t size);
int qw_jp2_decode_internal_gray8(const void *data,
                                 size_t size,
                                 QwPageImage *out_page);
int qw_jp2_decode_internal_gray8_reduced(const void *data,
                                         size_t size,
                                         uint8_t resolution_reduction,
                                         QwPageImage *out_page);
int qw_jp2_decode_page(const void *data, size_t size, QwPageImage *out_page);
int qw_avif_probe(const void *data, size_t size);
int qw_avif_decode_page(const void *data, size_t size,
                        QwPageImage *out_page);
int qw_avif_encode_page(const QwPageImage *page,
                        uint8_t **out_data, size_t *out_size);
int qw_jxl_probe(const void *data, size_t size);
int qw_jxl_decode_page(const void *data, size_t size,
                       QwPageImage *out_page);
int qw_jxl_encode_page(const QwPageImage *page,
                       uint8_t **out_data, size_t *out_size);
int qw_ico_probe(const void *data, size_t size);
int qw_cur_probe(const void *data, size_t size);
int qw_icon_page_count(const void *data, size_t size, int kind,
                       size_t *out_count);
int qw_icon_decode_page(const void *data, size_t size, int kind,
                        size_t page_index, QwPageImage *out_page);
int qw_icon_encode_page(const QwPageImage *page, int kind,
                        uint8_t **out_data, size_t *out_size);
int qw_image_name_is_supported(const char *name);
int qw_webp_animation_create(const void *data, size_t size,
                             QwWebPAnimation **out_animation,
                             size_t *out_frame_count);
/* The borrowed data must outlive the animation object. */
int qw_webp_animation_create_borrowed(const void *data, size_t size,
                                     QwWebPAnimation **out_animation,
                                     size_t *out_frame_count);
int qw_webp_animation_read(QwWebPAnimation *animation, size_t index,
                           QwPageImage *out_page);
int qw_webp_animation_frame_duration(const QwWebPAnimation *animation,
                                     size_t index,
                                     uint32_t *out_duration_ms);
void qw_webp_animation_free(QwWebPAnimation *animation);

#endif
