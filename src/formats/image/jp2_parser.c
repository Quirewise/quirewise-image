#include "image_internal.h"
#include "jp2_packed_headers.h"
#include "jp2_packet_body.h"
#include "jp2_packet_header.h"
#include "jp2_packet_progression.h"
#include "jp2_dwt.h"

#include "quirewise/status.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define QW_JP2_BOX_TYPE(a, b, c, d) \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | \
     ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

enum {
    QW_JP2_BOX_SIGNATURE = QW_JP2_BOX_TYPE('j', 'P', ' ', ' '),
    QW_JP2_BOX_FILE_TYPE = QW_JP2_BOX_TYPE('f', 't', 'y', 'p'),
    QW_JP2_BOX_HEADER = QW_JP2_BOX_TYPE('j', 'p', '2', 'h'),
    QW_JP2_BOX_CODESTREAM = QW_JP2_BOX_TYPE('j', 'p', '2', 'c'),
    QW_JP2_BOX_IMAGE_HEADER = QW_JP2_BOX_TYPE('i', 'h', 'd', 'r'),
    QW_JP2_BOX_COMPONENT_BITS = QW_JP2_BOX_TYPE('b', 'p', 'c', 'c'),
    QW_JP2_BOX_COLOR = QW_JP2_BOX_TYPE('c', 'o', 'l', 'r'),
    QW_JP2_BOX_COMPONENT_MAPPING = QW_JP2_BOX_TYPE('c', 'm', 'a', 'p'),
    QW_JP2_BOX_PALETTE = QW_JP2_BOX_TYPE('p', 'c', 'l', 'r'),
    QW_JP2_BOX_CHANNEL_DEFINITION = QW_JP2_BOX_TYPE('c', 'd', 'e', 'f'),
    QW_JP2_BRAND_JP2 = QW_JP2_BOX_TYPE('j', 'p', '2', ' '),
    QW_JP2_BRAND_JPX = QW_JP2_BOX_TYPE('j', 'p', 'x', ' '),
    QW_JP2_BRAND_JPX_BASELINE = QW_JP2_BOX_TYPE('j', 'p', 'x', 'b'),
    QW_J2K_MARKER_SOC = 0x4fu,
    QW_J2K_MARKER_SIZ = 0x51u,
    QW_J2K_MARKER_COD = 0x52u,
    QW_J2K_MARKER_COC = 0x53u,
    QW_J2K_MARKER_RGN = 0x5eu,
    QW_J2K_MARKER_PPM = 0x60u,
    QW_J2K_MARKER_PPT = 0x61u,
    QW_J2K_MARKER_POC = 0x5fu,
    QW_J2K_MARKER_QCD = 0x5cu,
    QW_J2K_MARKER_QCC = 0x5du,
    QW_J2K_MARKER_SOT = 0x90u,
    QW_J2K_MARKER_SOP = 0x91u,
    QW_J2K_MARKER_EPH = 0x92u,
    QW_J2K_MARKER_SOD = 0x93u,
    QW_J2K_MARKER_EOC = 0xd9u,
    QW_JP2_MAX_TRACKED_TILE_STATES = 262144u,
    QW_JP2_MAX_TRACKED_PACKET_KEYS = 262144u,
    QW_JP2_MAX_TRACKED_PACKET_CODEBLOCKS = 262144u,
    QW_JP2_MAX_TRACKED_PACKET_BODY_BYTES = 64u * 1024u * 1024u,
    QW_JP2_MAX_TRACKED_PACKET_SEGMENT_METADATA_BYTES =
        QW_JP2_PACKET_SEGMENT_METADATA_MAX_BYTES,
    QW_JP2_MAX_PACKET_SEGMENT_LENGTHS = 1048576u,
    QW_JP2_MAX_TRACKED_QUANTIZATION_BYTES = 16u * 1024u * 1024u,
    QW_JP2_MAX_INTERNAL_DECODE_BYTES = 384u * 1024u * 1024u,
    QW_JP2_MAX_CODEBLOCK_COEFFICIENTS = 4096u,
    QW_JP2_MAX_CODEBLOCK_DIMENSION = 1024u
};

typedef struct QwJp2Box {
    uint32_t type;
    size_t payload_offset;
    size_t payload_size;
    size_t end_offset;
} QwJp2Box;

typedef struct QwJp2ContainerHeader {
    uint32_t width;
    uint32_t height;
    uint16_t component_count;
    uint8_t bits_per_component;
    const uint8_t *component_bits;
    size_t component_bits_size;
    const uint8_t *palette_payload;
    size_t palette_payload_size;
    const uint8_t *component_mapping_payload;
    size_t component_mapping_payload_size;
    const uint8_t *palette_entries;
    size_t palette_entry_stride;
    size_t palette_column_offsets[UINT8_MAX];
    uint8_t palette_column_precision[UINT8_MAX];
    uint8_t palette_column_signed[UINT8_MAX];
    uint8_t palette_column_count;
    uint16_t palette_entry_count;
    uint8_t palette_columns[3];
    uint8_t palette_mapping_types[3];
    int has_image_header;
    int has_component_bits;
    int has_palette;
    int has_component_mapping_box;
    int has_color;
    int has_channel_mapping;
    int has_channel_definition;
    int has_rgb_palette_mapping;
    int has_gray_palette_mapping;
    const uint8_t *channel_definition;
    size_t channel_definition_size;
    int has_enumerated_color_space;
    uint8_t color_method;
    size_t color_box_count;
    uint32_t enumerated_color_space;
} QwJp2ContainerHeader;

typedef struct QwJp2CodestreamHeader {
    uint32_t width;
    uint32_t height;
    uint32_t tile_count;
    uint16_t component_count;
    uint16_t layer_count;
    uint8_t decomposition_levels;
    uint8_t progression_order;
    uint8_t multiple_component_transform;
    uint8_t wavelet_transform;
} QwJp2CodestreamHeader;

typedef struct QwJp2InternalDecodeContext {
    struct QwJp2InternalDecodeContext *component_contexts;
    void *coefficient_plane;
    void *scratch;
    uint8_t *coverage;
    uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t component_x_origin;
    uint32_t component_y_origin;
    uint8_t decomposition_levels;
    uint8_t source_decomposition_levels;
    uint8_t resolution_reduction;
    uint8_t roi_shift;
    uint8_t wavelet_transform;
    uint8_t precision;
    uint8_t quantization_style;
    uint8_t quantization_guard_bits;
    uint8_t quantization_subband_count;
    uint8_t quantization_exponents[
        3u * (QW_JP2_PACKET_MAX_RESOLUTION_LEVELS - 1u) + 1u];
    uint16_t quantization_mantissas[
        3u * (QW_JP2_PACKET_MAX_RESOLUTION_LEVELS - 1u) + 1u];
    size_t pixel_count;
    size_t covered_count;
    size_t scratch_capacity;
    uint16_t component_context_count;
    uint8_t multiple_component_transform;
    uint8_t sycc_color_space;
    uint8_t alpha_channel_type;
    uint8_t rgb_component_indices[3];
    uint8_t alpha_component_index;
    uint8_t component_x_sampling[4];
    uint8_t component_y_sampling[4];
    uint8_t output_channels;
    uint8_t mosaic_output_mode;
    uint8_t rgb_mosaic_output_mode;
    uint8_t use_per_coefficient_reconstruction;
    int initialized;
} QwJp2InternalDecodeContext;

static void qw_jp2_internal_decode_context_clear(
    QwJp2InternalDecodeContext *context)
{
    if (!context) return;
    if (context->component_contexts) {
        for (uint16_t i = 0u; i < context->component_context_count; ++i) {
            qw_jp2_internal_decode_context_clear(
                &context->component_contexts[i]);
        }
        free(context->component_contexts);
    }
    free(context->coefficient_plane);
    free(context->scratch);
    free(context->coverage);
    free(context->pixels);
    memset(context, 0, sizeof(*context));
}

static int qw_jp2_internal_decode_context_init(
    QwJp2InternalDecodeContext *context,
    uint32_t width,
    uint32_t height,
    uint8_t decomposition_levels,
    size_t reserved_bytes)
{
    size_t coefficient_bytes;
    size_t coverage_bytes;
    size_t scratch_bytes;
    size_t scratch_capacity;
    size_t allocation_bytes;
    size_t output_bytes;
    uint8_t output_channels;

    if (!context || context->initialized || width == 0u || height == 0u ||
        !qw_image_dimensions_within_limits(width, height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (!qw_checked_size_mul((size_t)width, (size_t)height,
                             &context->pixel_count) ||
        context->pixel_count > SIZE_MAX / sizeof(int64_t) ||
        context->pixel_count > SIZE_MAX - 7u) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    output_channels = context->output_channels == 3u ? 3u : 1u;
    if (!qw_checked_size_mul(context->pixel_count,
                             (size_t)output_channels, &output_bytes)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    coefficient_bytes = context->pixel_count *
                        sizeof(int64_t);
    coverage_bytes = (context->pixel_count + 7u) / 8u;
    scratch_capacity = width > height ? (size_t)width : (size_t)height;
    if (reserved_bytes > (size_t)QW_JP2_MAX_INTERNAL_DECODE_BYTES ||
        coefficient_bytes >
            (size_t)QW_JP2_MAX_INTERNAL_DECODE_BYTES - reserved_bytes ||
        !qw_checked_size_mul(scratch_capacity,
                             sizeof(int64_t), &scratch_bytes) ||
        scratch_bytes >
            (size_t)QW_JP2_MAX_INTERNAL_DECODE_BYTES - reserved_bytes -
                coefficient_bytes) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    allocation_bytes = reserved_bytes + coefficient_bytes + scratch_bytes;
    if (coverage_bytes >
            (size_t)QW_JP2_MAX_INTERNAL_DECODE_BYTES - allocation_bytes ||
        output_bytes >
            (size_t)QW_JP2_MAX_INTERNAL_DECODE_BYTES - allocation_bytes -
                coverage_bytes) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }

    context->coefficient_plane = calloc(
        context->pixel_count, sizeof(int64_t));
    context->coverage = (uint8_t *)calloc(coverage_bytes, 1u);
    context->scratch = malloc(scratch_bytes);
    if (!context->coefficient_plane || !context->coverage ||
        !context->scratch) {
        qw_jp2_internal_decode_context_clear(context);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    context->width = width;
    context->height = height;
    context->decomposition_levels = decomposition_levels;
    context->scratch_capacity = scratch_capacity;
    context->output_channels = output_channels;
    context->initialized = 1;
    return QW_STATUS_OK;
}

static int qw_jp2_internal_mosaic_context_init(
    QwJp2InternalDecodeContext *context,
    uint32_t width,
    uint32_t height,
    uint8_t decomposition_levels)
{
    size_t coverage_bytes;
    size_t allocation_bytes;

    if (!context || context->initialized || width == 0u || height == 0u ||
        !qw_image_dimensions_within_limits(width, height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (!qw_checked_size_mul((size_t)width, (size_t)height,
                             &context->pixel_count) ||
        context->pixel_count > SIZE_MAX - 7u) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    coverage_bytes = (context->pixel_count + 7u) / 8u;
    if (coverage_bytes > (size_t)QW_JP2_MAX_INTERNAL_DECODE_BYTES ||
        context->pixel_count >
            (size_t)QW_JP2_MAX_INTERNAL_DECODE_BYTES - coverage_bytes) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    allocation_bytes = context->pixel_count + coverage_bytes;
    if (allocation_bytes > (size_t)QW_JP2_MAX_INTERNAL_DECODE_BYTES) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    context->pixels = (uint8_t *)calloc(context->pixel_count, 1u);
    context->coverage = (uint8_t *)calloc(coverage_bytes, 1u);
    if (!context->pixels || !context->coverage) {
        qw_jp2_internal_decode_context_clear(context);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    context->width = width;
    context->height = height;
    context->decomposition_levels = decomposition_levels;
    context->output_channels = 1u;
    context->mosaic_output_mode = 1u;
    context->initialized = 1;
    return QW_STATUS_OK;
}

typedef struct QwJp2ComponentCodingParameters {
    uint8_t decomposition_levels;
    uint8_t codeblock_width_exponent;
    uint8_t codeblock_height_exponent;
    uint8_t codeblock_style;
    uint8_t wavelet_transform;
    uint8_t precinct_sizes_signaled;
    uint8_t precinct_x_exponents[QW_JP2_PACKET_MAX_RESOLUTION_LEVELS];
    uint8_t precinct_y_exponents[QW_JP2_PACKET_MAX_RESOLUTION_LEVELS];
} QwJp2ComponentCodingParameters;

typedef struct QwJp2CodParameters {
    uint16_t layer_count;
    uint8_t progression_order;
    uint8_t decomposition_levels;
    uint8_t multiple_component_transform;
    uint8_t wavelet_transform;
    uint8_t coding_style_flags;
    QwJp2ComponentCodingParameters component;
} QwJp2CodParameters;

enum {
    QW_JP2_MAX_SUBBANDS =
        3u * (QW_JP2_PACKET_MAX_RESOLUTION_LEVELS - 1u) + 1u
};

typedef struct QwJp2QuantizationMarker {
    const uint8_t *payload;
    size_t payload_size;
    uint8_t present;
} QwJp2QuantizationMarker;

typedef struct QwJp2QuantizationParameters {
    uint8_t style;
    uint8_t guard_bits;
    uint8_t present;
    uint8_t subband_count;
    uint8_t exponents[QW_JP2_MAX_SUBBANDS];
    uint16_t mantissas[QW_JP2_MAX_SUBBANDS];
} QwJp2QuantizationParameters;

static int qw_jp2_internal_quantization_supported(
    uint8_t wavelet_transform,
    const QwJp2QuantizationParameters *quantization)
{
    if (!quantization || !quantization->present) return 0;
    if (wavelet_transform == 1u) return quantization->style == 0u;
    if (wavelet_transform == 0u) {
        return (quantization->style == 1u || quantization->style == 2u) &&
               sizeof(double) == sizeof(int64_t);
    }
    return 0;
}

static void qw_jp2_internal_decode_set_quantization(
    QwJp2InternalDecodeContext *context,
    const QwJp2QuantizationParameters *quantization)
{
    if (!context || !quantization) return;
    context->quantization_style = quantization->style;
    context->quantization_guard_bits = quantization->guard_bits;
    context->quantization_subband_count = quantization->subband_count;
    memcpy(context->quantization_exponents, quantization->exponents,
           sizeof(context->quantization_exponents));
    memcpy(context->quantization_mantissas, quantization->mantissas,
           sizeof(context->quantization_mantissas));
}

static int qw_jp2_internal_rgb_context_init(
    QwJp2InternalDecodeContext *context,
    uint32_t width,
    uint32_t height,
    uint8_t decomposition_levels,
    const uint8_t component_decomposition_levels[4],
    uint8_t wavelet_transform,
    uint8_t multiple_component_transform,
    uint16_t component_count,
    uint8_t alpha_channel_type,
    const uint8_t rgb_component_indices[3],
    uint8_t alpha_component_index,
    const uint8_t component_x_sampling[4],
    const uint8_t component_y_sampling[4],
    const uint8_t precisions[4],
    const QwJp2QuantizationParameters quantizations[4],
    const uint8_t roi_shifts[4],
    size_t externally_reserved_bytes,
    int rgb_mosaic_mode)
{
    size_t pixel_count;
    size_t output_bytes;
    size_t coefficient_bytes;
    size_t coverage_bytes;
    size_t scratch_bytes;
    size_t component_reservation;
    size_t component_array_bytes;
    size_t reserved_bytes;
    size_t scratch_capacity;
    uint8_t resolved_decomposition_levels[4] = {0u};
    uint32_t resolved_component_widths[4] = {0u};
    uint32_t resolved_component_heights[4] = {0u};
    uint8_t resolved_component_x_sampling[4] = {0u};
    uint8_t resolved_component_y_sampling[4] = {0u};
    uint8_t used_components = 0u;
    uint8_t all_components;

    if (!context || context->initialized || !width || !height ||
        !quantizations || !roi_shifts ||
        !precisions || !rgb_component_indices ||
        ((component_x_sampling == NULL) !=
         (component_y_sampling == NULL)) ||
        (component_count != 3u && component_count != 4u) ||
        (component_count == 3u && alpha_channel_type != 0u) ||
        (component_count == 4u &&
         alpha_channel_type != 1u && alpha_channel_type != 2u) ||
        (rgb_mosaic_mode != 0 && rgb_mosaic_mode != 1) ||
        externally_reserved_bytes >
            (size_t)QW_JP2_MAX_INTERNAL_DECODE_BYTES ||
        !qw_image_dimensions_within_limits(width, height) ||
        (wavelet_transform != 0u && wavelet_transform != 1u)) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    all_components = (uint8_t)((1u << component_count) - 1u);
    for (uint16_t component = 0u; component < component_count; ++component) {
        uint8_t x_sampling = component_x_sampling
            ? component_x_sampling[component] : 1u;
        uint8_t y_sampling = component_y_sampling
            ? component_y_sampling[component] : 1u;
        resolved_decomposition_levels[component] =
            component_decomposition_levels
                ? component_decomposition_levels[component]
                : decomposition_levels;
        if (!x_sampling || !y_sampling) return QW_STATUS_ERR_INVALID_ARGUMENT;
        if (rgb_mosaic_mode && (x_sampling != 1u || y_sampling != 1u)) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        resolved_component_x_sampling[component] = x_sampling;
        resolved_component_y_sampling[component] = y_sampling;
        resolved_component_widths[component] =
            width / x_sampling + (width % x_sampling != 0u ? 1u : 0u);
        resolved_component_heights[component] =
            height / y_sampling + (height % y_sampling != 0u ? 1u : 0u);
        if (resolved_decomposition_levels[component] > 32u ||
            (rgb_mosaic_mode &&
             resolved_decomposition_levels[component] !=
                 decomposition_levels)) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
    }
    for (size_t channel = 0u; channel < 3u; ++channel) {
        uint8_t component_index = rgb_component_indices[channel];
        uint8_t component_bit;
        if (component_index >= component_count) {
            return QW_STATUS_ERR_INVALID_ARGUMENT;
        }
        component_bit = (uint8_t)(1u << component_index);
        if (used_components & component_bit) {
            return QW_STATUS_ERR_INVALID_ARGUMENT;
        }
        used_components |= component_bit;
    }
    if (alpha_channel_type != 0u) {
        uint8_t alpha_bit;
        if (alpha_component_index >= component_count) {
            return QW_STATUS_ERR_INVALID_ARGUMENT;
        }
        alpha_bit = (uint8_t)(1u << alpha_component_index);
        if (used_components & alpha_bit) {
            return QW_STATUS_ERR_INVALID_ARGUMENT;
        }
        used_components |= alpha_bit;
    }
    if (used_components != all_components ||
        (multiple_component_transform == 1u &&
         (rgb_component_indices[0] != 0u ||
          rgb_component_indices[1] != 1u ||
          rgb_component_indices[2] != 2u ||
          (component_count == 4u && alpha_component_index != 3u)))) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (!qw_checked_size_mul((size_t)width, (size_t)height, &pixel_count) ||
        pixel_count > SIZE_MAX / 3u || pixel_count > SIZE_MAX - 7u) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    output_bytes = pixel_count * 3u;
    coverage_bytes = (pixel_count + 7u) / 8u;
    coefficient_bytes = 0u;
    scratch_bytes = 0u;
    scratch_capacity = 0u;
    component_reservation = 0u;
    if (!rgb_mosaic_mode) {
        if (pixel_count > SIZE_MAX / sizeof(int64_t)) {
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
        coefficient_bytes = pixel_count * sizeof(int64_t);
        scratch_capacity = width > height ? (size_t)width : (size_t)height;
        if (!qw_checked_size_mul(scratch_capacity, sizeof(int64_t),
                                 &scratch_bytes)) {
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
        component_reservation = coefficient_bytes;
        if (scratch_bytes > SIZE_MAX - component_reservation) {
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
        component_reservation += scratch_bytes;
        if (coverage_bytes > SIZE_MAX - component_reservation) {
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
        component_reservation += coverage_bytes;
        if (pixel_count > SIZE_MAX - component_reservation) {
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
        component_reservation += pixel_count;
    }
    component_array_bytes = (size_t)component_count *
                            sizeof(*context->component_contexts);
    reserved_bytes = externally_reserved_bytes;
    if (output_bytes > (size_t)QW_JP2_MAX_INTERNAL_DECODE_BYTES -
                           reserved_bytes ||
        component_array_bytes >
            (size_t)QW_JP2_MAX_INTERNAL_DECODE_BYTES - reserved_bytes -
                output_bytes) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    reserved_bytes += output_bytes + component_array_bytes;
    if (rgb_mosaic_mode) {
        if (coverage_bytes >
            (size_t)QW_JP2_MAX_INTERNAL_DECODE_BYTES - reserved_bytes) {
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
        reserved_bytes += coverage_bytes;
    }
    for (size_t i = 0u; i < component_count; ++i) {
        if (!qw_jp2_internal_quantization_supported(wavelet_transform,
                                                     &quantizations[i])) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if (rgb_mosaic_mode) continue;
        if (component_reservation >
            (size_t)QW_JP2_MAX_INTERNAL_DECODE_BYTES - reserved_bytes) {
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
        reserved_bytes += component_reservation;
    }

    context->component_contexts = (QwJp2InternalDecodeContext *)calloc(
        component_count, sizeof(*context->component_contexts));
    if (!context->component_contexts) return QW_STATUS_ERR_OUT_OF_MEMORY;
    context->component_context_count = component_count;
    if (rgb_mosaic_mode) {
        context->pixels = (uint8_t *)malloc(output_bytes ? output_bytes : 1u);
        context->coverage = (uint8_t *)calloc(coverage_bytes, 1u);
        if (!context->pixels || !context->coverage) {
            qw_jp2_internal_decode_context_clear(context);
            return QW_STATUS_ERR_OUT_OF_MEMORY;
        }
    }
    context->width = width;
    context->height = height;
    context->pixel_count = pixel_count;
    context->decomposition_levels = decomposition_levels;
    context->source_decomposition_levels = decomposition_levels;
    context->wavelet_transform = wavelet_transform;
    context->multiple_component_transform = multiple_component_transform;
    context->alpha_channel_type = alpha_channel_type;
    memcpy(context->rgb_component_indices, rgb_component_indices,
           sizeof(context->rgb_component_indices));
    context->alpha_component_index = alpha_component_index;
    memcpy(context->component_x_sampling, resolved_component_x_sampling,
           sizeof(context->component_x_sampling));
    memcpy(context->component_y_sampling, resolved_component_y_sampling,
           sizeof(context->component_y_sampling));
    context->output_channels = 3u;
    context->rgb_mosaic_output_mode = (uint8_t)rgb_mosaic_mode;

    for (uint16_t i = 0u; i < component_count; ++i) {
        QwJp2InternalDecodeContext *component =
            &context->component_contexts[i];
        if (!rgb_mosaic_mode) {
            int rc = qw_jp2_internal_decode_context_init(
                component, resolved_component_widths[i],
                resolved_component_heights[i],
                resolved_decomposition_levels[i],
                reserved_bytes - component_reservation);
            if (rc != QW_STATUS_OK) {
                qw_jp2_internal_decode_context_clear(context);
                return rc;
            }
        }
        component->width = rgb_mosaic_mode
            ? width : resolved_component_widths[i];
        component->height = rgb_mosaic_mode
            ? height : resolved_component_heights[i];
        component->decomposition_levels =
            resolved_decomposition_levels[i];
        component->source_decomposition_levels =
            resolved_decomposition_levels[i];
        component->wavelet_transform = wavelet_transform;
        component->precision = precisions[i];
        component->roi_shift = roi_shifts[i];
        qw_jp2_internal_decode_set_quantization(component, &quantizations[i]);
    }
    context->initialized = 1;
    return QW_STATUS_OK;
}

static QwJp2InternalDecodeContext *qw_jp2_internal_component_context(
    QwJp2InternalDecodeContext *context,
    uint16_t component_index)
{
    if (!context) return NULL;
    if (context->component_context_count == 0u) {
        return component_index == 0u ? context : NULL;
    }
    if (!context->component_contexts ||
        component_index >= context->component_context_count) {
        return NULL;
    }
    return &context->component_contexts[component_index];
}

typedef struct QwJp2PacketPrecinctState {
    uint16_t component_index;
    uint8_t resolution_level;
    uint32_t precinct_index;
    uint64_t reference_x;
    uint64_t reference_y;
    QwJp2PacketSubbandHeaderState subbands[3];
    QwJp2PacketTagTree *trees[6];
    QwJp2PacketCodeBlockState *codeblocks[3];
    QwJp2PacketCodeBlockBody *bodies[3];
} QwJp2PacketPrecinctState;

typedef struct QwJp2TilePacketState {
    QwJp2PacketOrderEntry *packet_order;
    size_t packet_count;
    size_t packet_order_capacity;
    QwJp2PacketCodeBlockContribution *contributions;
    size_t contribution_capacity;
    uint32_t *segment_lengths;
    size_t segment_length_capacity;
    size_t next_packet_index;
    size_t *packet_precinct_indices;
    size_t packet_precinct_capacity;
    QwJp2PacketPrecinctState *precinct_states;
    size_t precinct_state_count;
    size_t precinct_state_capacity;
    uint8_t *subband_max_bitplanes;
    QwJp2PacketTileGeometry tile_geometry;
    QwJp2PacketComponentGeometry component_geometry[4];
    uint8_t component_codeblock_styles[4];
    uint8_t coding_style_flags;
    uint8_t initialized;
} QwJp2TilePacketState;

typedef struct QwJp2TileCodingState {
    uint16_t layer_count;
    uint8_t component_resolution_counts[4];
    uint8_t tile_coc_resolution_counts[4];
    uint8_t tile_coc_mask;
    uint8_t component_roi_shifts[4];
    uint8_t tile_rgn_mask;
    uint8_t initialized;
    uint8_t first_tile_part_seen;
    uint8_t tile_cod_seen;
    uint8_t tile_poc_seen;
    QwJp2QuantizationParameters internal_quantization;
    QwJp2QuantizationParameters internal_quantizations[4];
    QwJp2TilePacketState packet_state;
} QwJp2TileCodingState;

static uint16_t qw_jp2_read_u16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

static uint32_t qw_jp2_read_u32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static uint64_t qw_jp2_read_u64(const uint8_t *data)
{
    return ((uint64_t)qw_jp2_read_u32(data) << 32) |
           (uint64_t)qw_jp2_read_u32(data + 4u);
}

static int qw_jp2_collect_packed_headers(
    const QwJp2PackedHeaderSegment *segments,
    size_t segment_count,
    uint8_t **out_data,
    size_t *out_size)
{
    size_t capacity = 0u;
    size_t collected_size = 0u;
    uint8_t *data;
    int rc;

    if (!out_data || !out_size || (!segments && segment_count != 0u)) {
        return QW_STATUS_ERR_FORMAT;
    }
    *out_data = NULL;
    *out_size = 0u;
    if (segment_count == 0u) return QW_STATUS_OK;
    if (segment_count > (size_t)QW_JP2_PACKED_HEADER_MAX_SEGMENTS) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    for (size_t i = 0u; i < segment_count; ++i) {
        if (segments[i].payload_size >
            (size_t)QW_JP2_PACKED_HEADER_MAX_BYTES - capacity) {
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
        capacity += segments[i].payload_size;
    }
    data = (uint8_t *)malloc(capacity ? capacity : 1u);
    if (!data) return QW_STATUS_ERR_OUT_OF_MEMORY;
    rc = qw_jp2_packed_header_collect(segments, segment_count, data,
                                      capacity, &collected_size);
    if (rc != QW_JP2_PACKED_HEADER_OK) {
        free(data);
        return rc == QW_JP2_PACKED_HEADER_ERR_RESOURCE
                   ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_FORMAT;
    }
    *out_data = data;
    *out_size = collected_size;
    return QW_STATUS_OK;
}

static int qw_jp2_read_box(const uint8_t *data,
                           size_t parent_end,
                           size_t offset,
                           QwJp2Box *out_box)
{
    uint32_t box_length;
    uint64_t extended_length;
    size_t header_size = 8u;
    uint64_t resolved_length;

    if (!data || !out_box || offset > parent_end ||
        parent_end - offset < 8u) {
        return 0;
    }
    box_length = qw_jp2_read_u32(data + offset);
    out_box->type = qw_jp2_read_u32(data + offset + 4u);
    if (box_length == 1u) {
        if (parent_end - offset < 16u) return 0;
        extended_length = qw_jp2_read_u64(data + offset + 8u);
        resolved_length = extended_length;
        header_size = 16u;
    } else if (box_length == 0u) {
        resolved_length = (uint64_t)(parent_end - offset);
    } else {
        resolved_length = (uint64_t)box_length;
    }
    if (resolved_length < (uint64_t)header_size ||
        resolved_length > (uint64_t)(parent_end - offset) ||
        resolved_length > (uint64_t)SIZE_MAX) {
        return 0;
    }
    out_box->payload_offset = offset + header_size;
    out_box->payload_size = (size_t)resolved_length - header_size;
    out_box->end_offset = offset + (size_t)resolved_length;
    return 1;
}

static int qw_jp2_brand_is_jpx(uint32_t brand)
{
    return brand == QW_JP2_BRAND_JPX ||
           brand == QW_JP2_BRAND_JPX_BASELINE;
}

static int qw_jp2_validate_component_bits(uint8_t bits)
{
    unsigned precision;
    if (bits == 0xffu) return 0;
    precision = (unsigned)(bits & 0x7fu) + 1u;
    return precision <= 32u;
}

static int qw_jp2_container_has_rgb_palette_mapping(
    QwJp2ContainerHeader *header);
static int qw_jp2_container_has_gray_palette_mapping(
    QwJp2ContainerHeader *header);
static uint8_t qw_jp2_internal_signed_sample_to_u8(int64_t sample,
                                                   uint8_t precision);
static uint8_t qw_jp2_internal_unsigned_sample_to_u8(uint64_t sample,
                                                     uint8_t precision);

static int qw_jp2_parse_palette_layout(QwJp2ContainerHeader *header)
{
    const uint8_t *palette;
    uint16_t entry_count;
    uint8_t column_count;
    size_t entry_stride = 0u;
    size_t entries_offset;
    size_t entries_size;

    if (!header || !header->has_palette || !header->palette_payload) {
        return QW_STATUS_ERR_FORMAT;
    }
    palette = header->palette_payload;
    if (header->palette_payload_size < 3u) return QW_STATUS_ERR_FORMAT;
    entry_count = qw_jp2_read_u16(palette);
    column_count = palette[2];
    if (entry_count == 0u || column_count == 0u) {
        return QW_STATUS_ERR_FORMAT;
    }
    entries_offset = 3u + (size_t)column_count;
    if (entries_offset > header->palette_payload_size) {
        return QW_STATUS_ERR_FORMAT;
    }

    for (uint16_t column = 0u; column < (uint16_t)column_count; ++column) {
        uint8_t bits = palette[3u + (size_t)column];
        uint8_t precision = (uint8_t)((bits & 0x7fu) + 1u);
        size_t column_width;

        if (precision > 38u) return QW_STATUS_ERR_UNSUPPORTED;
        column_width = ((size_t)precision + 7u) / 8u;
        if (entry_stride > SIZE_MAX - column_width) {
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
        header->palette_column_offsets[column] = entry_stride;
        header->palette_column_precision[column] = precision;
        header->palette_column_signed[column] = (uint8_t)(bits >> 7);
        entry_stride += column_width;
    }
    if (entry_stride == 0u ||
        !qw_checked_size_mul((size_t)entry_count, entry_stride,
                             &entries_size)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (entries_size != header->palette_payload_size - entries_offset) {
        return QW_STATUS_ERR_FORMAT;
    }

    header->palette_entries = palette + entries_offset;
    header->palette_entry_stride = entry_stride;
    header->palette_column_count = column_count;
    header->palette_entry_count = entry_count;
    return QW_STATUS_OK;
}

static int qw_jp2_parse_image_header(const uint8_t *data,
                                    size_t size,
                                    QwJp2ContainerHeader *header)
{
    size_t offset = 0u;
    int rc;
    if (!data || !header) return QW_STATUS_ERR_FORMAT;
    while (offset < size) {
        QwJp2Box box;
        if (!qw_jp2_read_box(data, size, offset, &box)) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (box.type == QW_JP2_BOX_IMAGE_HEADER) {
            const uint8_t *payload = data + box.payload_offset;
            if (header->has_image_header || box.payload_size != 14u) {
                return QW_STATUS_ERR_FORMAT;
            }
            header->height = qw_jp2_read_u32(payload);
            header->width = qw_jp2_read_u32(payload + 4u);
            header->component_count = qw_jp2_read_u16(payload + 8u);
            header->bits_per_component = payload[10];
            if (payload[11] != 7u || payload[12] > 1u || payload[13] > 1u ||
                !header->width || !header->height ||
                !header->component_count) {
                return QW_STATUS_ERR_FORMAT;
            }
            if (header->component_count > 4u) {
                return QW_STATUS_ERR_UNSUPPORTED;
            }
            if (header->bits_per_component != 0xffu &&
                !qw_jp2_validate_component_bits(header->bits_per_component)) {
                return QW_STATUS_ERR_UNSUPPORTED;
            }
            if (!qw_image_dimensions_within_limits(header->width,
                                                   header->height)) {
                return QW_STATUS_ERR_RESOURCE_LIMIT;
            }
            header->has_image_header = 1;
        } else if (box.type == QW_JP2_BOX_COMPONENT_BITS) {
            if (!header->has_image_header || header->has_component_bits ||
                header->bits_per_component != 0xffu) {
                return QW_STATUS_ERR_FORMAT;
            }
            header->component_bits = data + box.payload_offset;
            header->component_bits_size = box.payload_size;
            header->has_component_bits = 1;
        } else if (box.type == QW_JP2_BOX_COLOR) {
            const uint8_t *payload = data + box.payload_offset;
            if (box.payload_size < 3u) {
                return QW_STATUS_ERR_FORMAT;
            }
            if (payload[0] == 1u) {
                if (box.payload_size < 7u) return QW_STATUS_ERR_FORMAT;
            } else if (payload[0] == 2u || payload[0] == 3u) {
                if (box.payload_size <= 3u) return QW_STATUS_ERR_FORMAT;
            } else {
                return QW_STATUS_ERR_UNSUPPORTED;
            }
            if (header->color_box_count == SIZE_MAX) {
                return QW_STATUS_ERR_RESOURCE_LIMIT;
            }
            if (header->color_box_count == 0u) {
                header->color_method = payload[0];
                if (payload[0] == 1u) {
                    header->has_enumerated_color_space = 1;
                    header->enumerated_color_space =
                        qw_jp2_read_u32(payload + 3u);
                }
            }
            ++header->color_box_count;
            header->has_color = 1;
        } else if (box.type == QW_JP2_BOX_COMPONENT_MAPPING) {
            if (header->has_component_mapping_box) {
                return QW_STATUS_ERR_FORMAT;
            }
            header->component_mapping_payload = data + box.payload_offset;
            header->component_mapping_payload_size = box.payload_size;
            header->has_component_mapping_box = 1;
            header->has_channel_mapping = 1;
        } else if (box.type == QW_JP2_BOX_PALETTE) {
            if (header->has_palette) return QW_STATUS_ERR_FORMAT;
            header->palette_payload = data + box.payload_offset;
            header->palette_payload_size = box.payload_size;
            header->has_palette = 1;
            header->has_channel_mapping = 1;
        } else if (box.type == QW_JP2_BOX_CHANNEL_DEFINITION) {
            const uint8_t *payload = data + box.payload_offset;
            uint16_t definition_count;
            size_t expected_size;
            if (header->has_channel_definition || box.payload_size < 2u) {
                return QW_STATUS_ERR_FORMAT;
            }
            definition_count = qw_jp2_read_u16(payload);
            expected_size = 2u + (size_t)definition_count * 6u;
            if (box.payload_size != expected_size) {
                return QW_STATUS_ERR_FORMAT;
            }
            header->has_channel_definition = 1;
            header->channel_definition = payload;
            header->channel_definition_size = box.payload_size;
        }
        offset = box.end_offset;
    }
    if (!header->has_image_header || !header->has_color) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (header->bits_per_component == 0xffu) {
        if (!header->has_component_bits ||
            header->component_bits_size != header->component_count) {
            return QW_STATUS_ERR_FORMAT;
        }
        for (size_t i = 0u; i < header->component_bits_size; ++i) {
            if (!qw_jp2_validate_component_bits(header->component_bits[i])) {
                return QW_STATUS_ERR_UNSUPPORTED;
            }
        }
    } else if (header->has_component_bits) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (header->has_palette) {
        rc = qw_jp2_parse_palette_layout(header);
        if (rc != QW_STATUS_OK) return rc;
    }
    header->has_rgb_palette_mapping =
        qw_jp2_container_has_rgb_palette_mapping(header);
    header->has_gray_palette_mapping =
        qw_jp2_container_has_gray_palette_mapping(header);
    return QW_STATUS_OK;
}

static int qw_jp2_container_has_rgb_palette_mapping(
    QwJp2ContainerHeader *header)
{
    const uint8_t *mapping;

    if (!header || !header->has_palette ||
        !header->has_component_mapping_box ||
        header->component_count != 1u || header->has_channel_definition ||
        header->color_box_count != 1u || header->color_method != 1u ||
        !header->has_enumerated_color_space ||
        header->enumerated_color_space != 16u ||
        !header->palette_entries || header->palette_entry_count == 0u ||
        header->palette_column_count == 0u ||
        header->component_mapping_payload_size != 12u) {
        return 0;
    }

    mapping = header->component_mapping_payload;
    for (uint16_t channel = 0u; channel < 3u; ++channel) {
        const uint8_t *entry = mapping + 4u * (size_t)channel;
        uint8_t mapping_type = entry[2];
        uint8_t column = entry[3];
        if (qw_jp2_read_u16(entry) != 0u || mapping_type > 1u ||
            (mapping_type == 0u && column != 0u) ||
            (mapping_type == 1u && column >= header->palette_column_count)) {
            return 0;
        }
        header->palette_mapping_types[channel] = mapping_type;
        header->palette_columns[channel] = column;
    }
    return 1;
}

static int qw_jp2_container_has_gray_palette_mapping(
    QwJp2ContainerHeader *header)
{
    const uint8_t *mapping;

    if (!header || !header->has_palette ||
        !header->has_component_mapping_box ||
        header->component_count != 1u || header->has_channel_definition ||
        header->color_box_count != 1u || header->color_method != 1u ||
        !header->has_enumerated_color_space ||
        header->enumerated_color_space != 17u ||
        !header->palette_entries || header->palette_entry_count == 0u ||
        header->palette_column_count == 0u ||
        header->component_mapping_payload_size != 4u) {
        return 0;
    }

    mapping = header->component_mapping_payload;
    if (qw_jp2_read_u16(mapping) != 0u || mapping[2] > 1u ||
        (mapping[2] == 0u && mapping[3] != 0u) ||
        (mapping[2] == 1u && mapping[3] >= header->palette_column_count)) {
        return 0;
    }

    header->palette_mapping_types[0] = mapping[2];
    header->palette_columns[0] = mapping[3];
    return 1;
}

static int qw_jp2_apply_rgb_palette_mapping(
    QwJp2InternalDecodeContext *context,
    const QwJp2ContainerHeader *container)
{
    size_t output_size;

    if (!context || !context->initialized || !context->pixels ||
        context->output_channels != 3u || !container ||
        !container->has_rgb_palette_mapping ||
        !container->palette_entries ||
        container->palette_entry_count == 0u ||
        !qw_checked_size_mul(context->pixel_count, 3u, &output_size)) {
        return QW_STATUS_ERR_FORMAT;
    }
    (void)output_size;
    for (size_t pixel = context->pixel_count; pixel > 0u; --pixel) {
        size_t index = pixel - 1u;
        uint8_t palette_index = context->pixels[index];
        if ((uint16_t)palette_index >= container->palette_entry_count) {
            return QW_STATUS_ERR_FORMAT;
        }
        for (size_t channel = 0u; channel < 3u; ++channel) {
            uint8_t value = palette_index;
            if (container->palette_mapping_types[channel] == 1u) {
                uint8_t column = container->palette_columns[channel];
                const uint8_t *sample = container->palette_entries +
                    (size_t)palette_index * container->palette_entry_stride +
                    container->palette_column_offsets[column];
                uint8_t precision =
                    container->palette_column_precision[column];
                size_t sample_bytes = ((size_t)precision + 7u) / 8u;
                uint64_t raw = 0u;
                uint64_t range = UINT64_C(1) << precision;

                for (size_t byte = 0u; byte < sample_bytes; ++byte) {
                    raw = (raw << 8) | sample[byte];
                }
                raw &= range - 1u;
                if (container->palette_column_signed[column]) {
                    uint64_t sign_bit = UINT64_C(1) << (precision - 1u);
                    int64_t signed_sample = (raw & sign_bit) != 0u
                        ? (int64_t)raw - (int64_t)range
                        : (int64_t)raw;
                    value = qw_jp2_internal_signed_sample_to_u8(
                        signed_sample, precision);
                } else {
                    value = qw_jp2_internal_unsigned_sample_to_u8(
                        raw, precision);
                }
            }
            context->pixels[3u * index + channel] = value;
        }
    }
    return QW_STATUS_OK;
}

static int qw_jp2_apply_gray_palette_mapping(
    QwJp2InternalDecodeContext *context,
    const QwJp2ContainerHeader *container)
{
    if (!context || !context->initialized || !context->pixels ||
        context->output_channels != 1u || !container ||
        !container->has_gray_palette_mapping ||
        !container->palette_entries || container->palette_entry_count == 0u) {
        return QW_STATUS_ERR_FORMAT;
    }
    for (size_t pixel = 0u; pixel < context->pixel_count; ++pixel) {
        uint8_t palette_index = context->pixels[pixel];
        if ((uint16_t)palette_index >= container->palette_entry_count) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (container->palette_mapping_types[0] == 1u) {
            uint8_t column = container->palette_columns[0];
            const uint8_t *sample = container->palette_entries +
                (size_t)palette_index * container->palette_entry_stride +
                container->palette_column_offsets[column];
            uint8_t precision = container->palette_column_precision[column];
            size_t sample_bytes = ((size_t)precision + 7u) / 8u;
            uint64_t raw = 0u;
            uint64_t range = UINT64_C(1) << precision;

            for (size_t byte = 0u; byte < sample_bytes; ++byte) {
                raw = (raw << 8) | sample[byte];
            }
            raw &= range - 1u;
            if (container->palette_column_signed[column]) {
                uint64_t sign_bit = UINT64_C(1) << (precision - 1u);
                int64_t signed_sample = (raw & sign_bit) != 0u
                    ? (int64_t)raw - (int64_t)range
                    : (int64_t)raw;
                context->pixels[pixel] =
                    qw_jp2_internal_signed_sample_to_u8(signed_sample,
                                                        precision);
            } else {
                context->pixels[pixel] =
                    qw_jp2_internal_unsigned_sample_to_u8(raw, precision);
            }
        }
    }
    return QW_STATUS_OK;
}

static int qw_jp2_channel_definition_rgb_alpha_type(
    const QwJp2ContainerHeader *container,
    uint16_t component_count,
    uint8_t *out_alpha_channel_type,
    uint8_t out_rgb_component_indices[3],
    uint8_t *out_alpha_component_index)
{
    uint16_t definition_count;
    uint8_t seen_channels = 0u;
    uint8_t seen_color_associations = 0u;
    uint8_t alpha_channel_type = 0u;
    uint8_t alpha_component_index = 0u;

    if (!container || !out_alpha_channel_type ||
        !out_rgb_component_indices || !out_alpha_component_index ||
        (component_count != 3u && component_count != 4u)) {
        return 0;
    }
    if (!container->has_channel_definition) {
        if (component_count != 3u) return 0;
        out_rgb_component_indices[0] = 0u;
        out_rgb_component_indices[1] = 1u;
        out_rgb_component_indices[2] = 2u;
        *out_alpha_component_index = 0u;
        *out_alpha_channel_type = 0u;
        return 1;
    }
    if (!container->channel_definition ||
        container->channel_definition_size < 2u) {
        return 0;
    }
    definition_count = qw_jp2_read_u16(container->channel_definition);
    if (definition_count != component_count ||
        container->channel_definition_size !=
            2u + (size_t)definition_count * 6u) {
        return 0;
    }
    for (uint16_t i = 0u; i < definition_count; ++i) {
        const uint8_t *description =
            container->channel_definition + 2u + (size_t)i * 6u;
        uint16_t channel = qw_jp2_read_u16(description);
        uint16_t type = qw_jp2_read_u16(description + 2u);
        uint16_t association = qw_jp2_read_u16(description + 4u);
        uint8_t channel_bit;

        if (channel >= component_count) return 0;
        channel_bit = (uint8_t)(1u << channel);
        if (seen_channels & channel_bit) return 0;
        seen_channels |= channel_bit;
        if (type == 0u) {
            uint8_t association_bit;
            if (association < 1u || association > 3u) {
                return 0;
            }
            association_bit = (uint8_t)(1u << (association - 1u));
            if (seen_color_associations & association_bit) return 0;
            seen_color_associations |= association_bit;
            out_rgb_component_indices[association - 1u] =
                (uint8_t)channel;
        } else if (component_count == 4u && alpha_channel_type == 0u &&
                   association == 0u && (type == 1u || type == 2u)) {
            alpha_channel_type = (uint8_t)type;
            alpha_component_index = (uint8_t)channel;
        } else {
            return 0;
        }
    }
    if (seen_channels != (uint8_t)((1u << component_count) - 1u) ||
        seen_color_associations != 0x07u ||
        (component_count == 4u && alpha_channel_type == 0u)) {
        return 0;
    }
    *out_alpha_channel_type = alpha_channel_type;
    *out_alpha_component_index = alpha_component_index;
    return 1;
}

static int qw_jp2_check_component_bits(const QwJp2ContainerHeader *container,
                                       uint16_t component_index,
                                       uint8_t codestream_bits)
{
    uint8_t expected_bits;
    if (!container) return 1;
    expected_bits = container->bits_per_component == 0xffu
        ? container->component_bits[component_index]
        : container->bits_per_component;
    return expected_bits == codestream_bits;
}

static int qw_jp2_parse_quantization_marker(
    const uint8_t *payload,
    size_t payload_size,
    uint16_t component_count,
    int component_specific,
    QwJp2QuantizationParameters *out_parameters,
    uint16_t *out_component_index)
{
    size_t component_width = component_specific
                                ? (component_count < 257u ? 1u : 2u)
                                : 0u;
    size_t step_size_count;
    size_t step_size_bytes;
    size_t step_offset;
    size_t quantization_bytes;
    uint16_t component_index = 0u;
    uint8_t style;
    QwJp2QuantizationParameters parameters = {0};

    if (!payload || !out_parameters || !component_count ||
        (component_specific && !out_component_index) ||
        payload_size < component_width + 1u) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (component_specific) {
        component_index = component_width == 1u
                              ? payload[0]
                              : qw_jp2_read_u16(payload);
        if (component_index >= component_count) return QW_STATUS_ERR_FORMAT;
    }
    step_offset = component_width + 1u;
    style = (uint8_t)(payload[component_width] & 0x1fu);
    if (style > 2u) return QW_STATUS_ERR_UNSUPPORTED;
    step_size_bytes = style == 0u ? 1u : 2u;
    quantization_bytes = payload_size - step_offset;
    if (style == 1u) {
        step_size_count = 1u;
        if (quantization_bytes != step_size_bytes) {
            return QW_STATUS_ERR_FORMAT;
        }
    } else {
        if (style == 2u && quantization_bytes % step_size_bytes != 0u) {
            return QW_STATUS_ERR_FORMAT;
        }
        step_size_count = quantization_bytes / step_size_bytes;
    }
    if (step_size_count == 0u ||
        step_size_count > (size_t)QW_JP2_MAX_SUBBANDS ||
        (style != 1u && (step_size_count - 1u) % 3u != 0u)) {
        return QW_STATUS_ERR_FORMAT;
    }

    parameters.style = style;
    parameters.guard_bits = (uint8_t)(payload[component_width] >> 5u);
    parameters.present = 1u;
    parameters.subband_count = (uint8_t)step_size_count;
    for (size_t i = 0u; i < step_size_count; ++i) {
        if (style == 0u) {
            parameters.exponents[i] = (uint8_t)(payload[step_offset + i] >> 3u);
            parameters.mantissas[i] = 0u;
        } else {
            uint16_t step = qw_jp2_read_u16(payload + step_offset + 2u * i);
            parameters.exponents[i] = (uint8_t)(step >> 11u);
            parameters.mantissas[i] = (uint16_t)(step & 0x07ffu);
        }
    }
    *out_parameters = parameters;
    if (component_specific) *out_component_index = component_index;
    return QW_STATUS_OK;
}

static int qw_jp2_quantization_max_bitplanes(
    const QwJp2QuantizationParameters *parameters,
    uint8_t decomposition_levels,
    uint8_t resolution_level,
    size_t subband_index,
    uint8_t *out_max_bitplanes)
{
    int exponent;
    int bitplanes;
    if (!parameters || !parameters->present || !out_max_bitplanes ||
        decomposition_levels > 32u || resolution_level > decomposition_levels) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (parameters->style == 1u) {
        int decomposition_level;
        if (parameters->subband_count != 1u) return QW_STATUS_ERR_FORMAT;
        decomposition_level = resolution_level == 0u
            ? (int)decomposition_levels
            : (int)decomposition_levels - (int)resolution_level + 1;
        exponent = (int)parameters->exponents[0] -
                   (int)decomposition_levels + decomposition_level;
    } else {
        if (subband_index >= (size_t)parameters->subband_count) {
            return QW_STATUS_ERR_FORMAT;
        }
        exponent = (int)parameters->exponents[subband_index];
    }
    bitplanes = (int)parameters->guard_bits + exponent - 1;
    if (bitplanes < 1 || bitplanes > QW_JP2_PACKET_MAX_BIT_PLANES) {
        return QW_STATUS_ERR_FORMAT;
    }
    *out_max_bitplanes = (uint8_t)bitplanes;
    return QW_STATUS_OK;
}

static int qw_jp2_quantization_fill_component_bitplanes(
    const QwJp2QuantizationParameters *parameters,
    uint8_t decomposition_levels,
    uint8_t roi_shift,
    uint8_t out_subband_max_bitplanes[QW_JP2_MAX_SUBBANDS])
{
    if (!parameters || !parameters->present || !out_subband_max_bitplanes ||
        decomposition_levels > 32u) {
        return QW_STATUS_ERR_FORMAT;
    }
    memset(out_subband_max_bitplanes, 0, QW_JP2_MAX_SUBBANDS);
    for (uint8_t resolution = 0u; resolution <= decomposition_levels;
         ++resolution) {
        size_t resolution_band_count = resolution == 0u ? 1u : 3u;
        for (size_t band = 0u; band < resolution_band_count; ++band) {
            size_t subband_index = resolution == 0u
                ? 0u
                : 1u + 3u * ((size_t)resolution - 1u) + band;
            int rc = qw_jp2_quantization_max_bitplanes(
                parameters, decomposition_levels, resolution, subband_index,
                &out_subband_max_bitplanes[subband_index]);
            if (rc != QW_STATUS_OK) return rc;
            if ((unsigned)out_subband_max_bitplanes[subband_index] +
                    (unsigned)roi_shift >
                (unsigned)QW_JP2_PACKET_MAX_BIT_PLANES) {
                return QW_STATUS_ERR_UNSUPPORTED;
            }
            out_subband_max_bitplanes[subband_index] = (uint8_t)(
                out_subband_max_bitplanes[subband_index] + roi_shift);
        }
    }
    return QW_STATUS_OK;
}

static int qw_jp2_marker_has_no_segment(uint8_t marker)
{
    /* T.800 A.1.3 reserves FF30-FF3F as marker codes without parameters. */
    return marker >= 0x30u && marker <= 0x3fu;
}

static int qw_jp2_parse_coc_payload(const uint8_t *payload,
                                   size_t payload_size,
                                   uint16_t component_count,
                                   uint16_t *out_component_index,
                                   QwJp2ComponentCodingParameters *out_parameters)
{
    size_t component_width = component_count < 257u ? 1u : 2u;
    size_t sp_offset = component_width + 1u;
    size_t expected_payload_size = component_width + 6u;
    uint16_t component_index;
    uint8_t coding_style;
    QwJp2ComponentCodingParameters parameters = {0};

    if (!payload || !out_component_index || !out_parameters ||
        component_count == 0u) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (payload_size < expected_payload_size) return QW_STATUS_ERR_FORMAT;
    component_index = component_width == 1u
                          ? payload[0]
                          : qw_jp2_read_u16(payload);
    coding_style = payload[component_width];
    parameters.decomposition_levels = payload[sp_offset];
    parameters.codeblock_width_exponent = payload[sp_offset + 1u];
    parameters.codeblock_height_exponent = payload[sp_offset + 2u];
    parameters.codeblock_style = payload[sp_offset + 3u];
    parameters.wavelet_transform = payload[sp_offset + 4u];
    if (component_index >= component_count || (coding_style & 0xfeu) ||
        parameters.decomposition_levels > 32u) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (coding_style & 0x01u) {
        expected_payload_size +=
            (size_t)parameters.decomposition_levels + 1u;
    }
    if (payload_size != expected_payload_size) return QW_STATUS_ERR_FORMAT;
    if (parameters.wavelet_transform > 1u) return QW_STATUS_ERR_UNSUPPORTED;
    parameters.precinct_sizes_signaled = (uint8_t)(coding_style & 0x01u);
    for (uint32_t resolution = 0u;
         resolution <= parameters.decomposition_levels; ++resolution) {
        uint8_t packed_precinct = parameters.precinct_sizes_signaled
            ? payload[sp_offset + 5u + resolution] : 0xffu;
        uint8_t precinct_x = (uint8_t)(packed_precinct & 0x0fu);
        uint8_t precinct_y = (uint8_t)(packed_precinct >> 4u);
        if (resolution != 0u && (!precinct_x || !precinct_y)) {
            return QW_STATUS_ERR_FORMAT;
        }
        parameters.precinct_x_exponents[resolution] = precinct_x;
        parameters.precinct_y_exponents[resolution] = precinct_y;
    }
    *out_component_index = component_index;
    *out_parameters = parameters;
    return QW_STATUS_OK;
}

static int qw_jp2_parse_rgn_payload(const uint8_t *payload,
                                   size_t payload_size,
                                   uint16_t component_count,
                                   uint16_t *out_component_index,
                                   uint8_t *out_roi_shift)
{
    size_t component_width = component_count < 257u ? 1u : 2u;
    uint16_t component_index;

    if (!payload || !out_component_index || !out_roi_shift ||
        component_count == 0u ||
        payload_size != component_width + 2u) {
        return QW_STATUS_ERR_FORMAT;
    }
    component_index = component_width == 1u
        ? payload[0] : qw_jp2_read_u16(payload);
    if (component_index >= component_count) return QW_STATUS_ERR_FORMAT;
    if (payload[component_width] != 0u) return QW_STATUS_ERR_UNSUPPORTED;
    *out_component_index = component_index;
    *out_roi_shift = payload[component_width + 1u];
    return QW_STATUS_OK;
}

static int qw_jp2_parse_cod_payload(const uint8_t *payload,
                                   size_t payload_size,
                                   uint16_t component_count,
                                   QwJp2CodParameters *out_parameters)
{
    QwJp2CodParameters parameters;
    size_t expected_payload_size = 10u;

    if (!payload || !out_parameters || component_count == 0u ||
        payload_size < expected_payload_size || (payload[0] & 0xf8u) ||
        payload[1] > QW_JP2_PROGRESSION_CPRL || payload[4] > 1u) {
        return QW_STATUS_ERR_FORMAT;
    }
    memset(&parameters, 0, sizeof(parameters));
    parameters.layer_count = qw_jp2_read_u16(payload + 2u);
    parameters.decomposition_levels = payload[5];
    parameters.component.decomposition_levels = payload[5];
    parameters.component.codeblock_width_exponent = payload[6];
    parameters.component.codeblock_height_exponent = payload[7];
    parameters.component.codeblock_style = payload[8];
    parameters.component.wavelet_transform = payload[9];
    if (!parameters.layer_count || parameters.decomposition_levels > 32u ||
        (payload[4] && component_count < 3u)) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (payload[0] & 0x01u) {
        expected_payload_size +=
            (size_t)parameters.decomposition_levels + 1u;
    }
    if (payload_size != expected_payload_size) return QW_STATUS_ERR_FORMAT;
    if (parameters.component.wavelet_transform > 1u)
        return QW_STATUS_ERR_UNSUPPORTED;
    parameters.progression_order = payload[1];
    parameters.multiple_component_transform = payload[4];
    parameters.wavelet_transform = parameters.component.wavelet_transform;
    parameters.coding_style_flags = payload[0];
    parameters.component.precinct_sizes_signaled =
        (uint8_t)(payload[0] & 0x01u);
    for (uint32_t resolution = 0u;
         resolution <= parameters.decomposition_levels; ++resolution) {
        uint8_t packed_precinct = parameters.component.precinct_sizes_signaled
            ? payload[10u + resolution] : 0xffu;
        uint8_t precinct_x = (uint8_t)(packed_precinct & 0x0fu);
        uint8_t precinct_y = (uint8_t)(packed_precinct >> 4u);
        if (resolution != 0u && (!precinct_x || !precinct_y)) {
            return QW_STATUS_ERR_FORMAT;
        }
        parameters.component.precinct_x_exponents[resolution] = precinct_x;
        parameters.component.precinct_y_exponents[resolution] = precinct_y;
    }
    *out_parameters = parameters;
    return QW_STATUS_OK;
}

static void qw_jp2_apply_component_coding(
    QwJp2PacketComponentGeometry *geometry,
    const QwJp2ComponentCodingParameters *coding)
{
    if (!geometry || !coding) return;
    geometry->decomposition_levels = coding->decomposition_levels;
    geometry->wavelet_transform = coding->wavelet_transform;
    geometry->codeblock_width_exponent =
        coding->codeblock_width_exponent;
    geometry->codeblock_height_exponent =
        coding->codeblock_height_exponent;
    geometry->codeblock_style = coding->codeblock_style;
    memcpy(geometry->precinct_x_exponents, coding->precinct_x_exponents,
           sizeof(geometry->precinct_x_exponents));
    memcpy(geometry->precinct_y_exponents, coding->precinct_y_exponents,
           sizeof(geometry->precinct_y_exponents));
}

static int qw_jp2_packet_status_to_image(int status)
{
    if (status == QW_JP2_PACKET_OK) return QW_STATUS_OK;
    if (status == QW_JP2_PACKET_ERR_RESOURCE) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (status == QW_JP2_PACKET_ERR_MEMORY) {
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    if (status == QW_JP2_PACKET_ERR_UNSUPPORTED) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    return QW_STATUS_ERR_FORMAT;
}

static void qw_jp2_packet_precinct_state_clear(
    QwJp2PacketPrecinctState *state)
{
    if (!state) return;
    for (size_t band = 0u; band < 3u; ++band) {
        if (state->bodies[band]) {
            for (size_t i = 0u;
                 i < state->subbands[band].codeblock_state_capacity; ++i) {
                free(state->bodies[band][i].data);
                free(state->bodies[band][i].segment_lengths);
            }
        }
        free(state->bodies[band]);
        qw_jp2_packet_tag_tree_free(state->trees[band * 2u]);
        qw_jp2_packet_tag_tree_free(state->trees[band * 2u + 1u]);
        free(state->codeblocks[band]);
    }
    memset(state, 0, sizeof(*state));
}

static void qw_jp2_tile_packet_state_clear(QwJp2TilePacketState *state)
{
    if (!state) return;
    for (size_t i = 0u; i < state->precinct_state_count; ++i) {
        qw_jp2_packet_precinct_state_clear(&state->precinct_states[i]);
    }
    free(state->precinct_states);
    free(state->packet_precinct_indices);
    free(state->packet_order);
    free(state->contributions);
    free(state->segment_lengths);
    free(state->subband_max_bitplanes);
    memset(state, 0, sizeof(*state));
}

static void qw_jp2_tile_coding_states_clear(QwJp2TileCodingState *states,
                                            uint32_t tile_count)
{
    if (!states) return;
    for (uint32_t tile = 0u; tile < tile_count; ++tile) {
        qw_jp2_tile_packet_state_clear(&states[tile].packet_state);
    }
}

static uint64_t qw_jp2_packet_precinct_key(
    const QwJp2PacketOrderEntry *packet)
{
    return ((uint64_t)packet->component_index << 40u) |
           ((uint64_t)packet->resolution_level << 32u) |
           (uint64_t)packet->precinct_index;
}

static size_t qw_jp2_packet_precinct_hash(uint64_t key)
{
    key ^= key >> 30u;
    key *= UINT64_C(0xbf58476d1ce4e5b9);
    key ^= key >> 27u;
    key *= UINT64_C(0x94d049bb133111eb);
    key ^= key >> 31u;
    return (size_t)key;
}

static int qw_jp2_packet_precinct_state_init(
    QwJp2PacketPrecinctState *state,
    const QwJp2PacketTileGeometry *tile,
    const QwJp2PacketComponentGeometry *component,
    const uint8_t *subband_max_bitplanes,
    const QwJp2PacketOrderEntry *packet,
    size_t codeblock_budget_remaining,
    size_t *out_codeblock_count)
{
    QwJp2PacketPrecinctState parsed = {0};
    size_t band_count;
    size_t codeblock_count = 0u;

    if (!state || !tile || !component || !subband_max_bitplanes || !packet ||
        !out_codeblock_count) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    band_count = packet->resolution_level == 0u ? 1u : 3u;
    parsed.component_index = packet->component_index;
    parsed.resolution_level = packet->resolution_level;
    parsed.precinct_index = packet->precinct_index;
    parsed.reference_x = packet->reference_x;
    parsed.reference_y = packet->reference_y;

    for (size_t band = 0u; band < band_count; ++band) {
        uint8_t orientation = packet->resolution_level == 0u
                                  ? QW_JP2_SUBBAND_LL
                                  : (uint8_t)(QW_JP2_SUBBAND_HL + band);
        size_t subband_index = packet->resolution_level == 0u
            ? 0u
            : 1u + 3u * ((size_t)packet->resolution_level - 1u) + band;
        QwJp2PacketPrecinctCodeBlockGrid grid;
        int packet_rc = qw_jp2_packet_precinct_codeblock_grid_compute(
            tile, component, packet, orientation, &grid);
        if (packet_rc != QW_JP2_PACKET_OK) {
            qw_jp2_packet_precinct_state_clear(&parsed);
            return qw_jp2_packet_status_to_image(packet_rc);
        }
        if (grid.codeblock_count == 0u) continue;
        if ((size_t)grid.codeblock_count >
                codeblock_budget_remaining - codeblock_count ||
            (size_t)grid.codeblock_count >
                SIZE_MAX / sizeof(*parsed.codeblocks[band])) {
            qw_jp2_packet_precinct_state_clear(&parsed);
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }

        packet_rc = qw_jp2_packet_tag_tree_create(
            grid.codeblocks_wide, grid.codeblocks_high,
            &parsed.trees[band * 2u]);
        if (packet_rc == QW_JP2_PACKET_OK) {
            packet_rc = qw_jp2_packet_tag_tree_create(
                grid.codeblocks_wide, grid.codeblocks_high,
                &parsed.trees[band * 2u + 1u]);
        }
        if (packet_rc != QW_JP2_PACKET_OK) {
            qw_jp2_packet_precinct_state_clear(&parsed);
            return qw_jp2_packet_status_to_image(packet_rc);
        }
        parsed.codeblocks[band] = (QwJp2PacketCodeBlockState *)calloc(
            (size_t)grid.codeblock_count, sizeof(*parsed.codeblocks[band]));
        if (!parsed.codeblocks[band]) {
            qw_jp2_packet_precinct_state_clear(&parsed);
            return QW_STATUS_ERR_OUT_OF_MEMORY;
        }
        parsed.subbands[band].codeblocks_wide = grid.codeblocks_wide;
        parsed.subbands[band].codeblocks_high = grid.codeblocks_high;
        parsed.subbands[band].codeblock_state_capacity =
            (size_t)grid.codeblock_count;
        parsed.bodies[band] = (QwJp2PacketCodeBlockBody *)calloc(
            (size_t)grid.codeblock_count, sizeof(*parsed.bodies[band]));
        if (!parsed.bodies[band]) {
            qw_jp2_packet_precinct_state_clear(&parsed);
            return QW_STATUS_ERR_OUT_OF_MEMORY;
        }
        for (uint32_t i = 0u; i < grid.codeblock_count; ++i) {
            packet_rc = qw_jp2_packet_codeblock_state_init(
                &parsed.codeblocks[band][i],
                subband_max_bitplanes[subband_index]);
            if (packet_rc != QW_JP2_PACKET_OK) {
                qw_jp2_packet_precinct_state_clear(&parsed);
                return qw_jp2_packet_status_to_image(packet_rc);
            }
        }
        parsed.subbands[band].inclusion_tree = parsed.trees[band * 2u];
        parsed.subbands[band].zero_bitplane_tree =
            parsed.trees[band * 2u + 1u];
        parsed.subbands[band].codeblocks = parsed.codeblocks[band];
        codeblock_count += (size_t)grid.codeblock_count;
    }

    *state = parsed;
    *out_codeblock_count = codeblock_count;
    return QW_STATUS_OK;
}

static int qw_jp2_tile_packet_state_init(
    QwJp2TileCodingState *tile_state,
    const QwJp2PacketTileGeometry *tile,
    const QwJp2PacketComponentGeometry *components,
    const QwJp2QuantizationParameters *quantizations,
    const uint8_t *roi_shifts,
    uint16_t component_count,
    uint8_t progression_order,
    uint8_t coding_style_flags,
    const uint8_t *poc_payload,
    size_t poc_payload_size,
    size_t *packet_budget_remaining,
    size_t *codeblock_budget_remaining,
    size_t *quantization_budget_remaining)
{
    QwJp2TilePacketState parsed = {0};
    QwJp2PacketProgressionVolume *volumes = NULL;
    QwJp2PacketOrderEntry *packet_order = NULL;
    uint64_t *map_keys = NULL;
    uint32_t *map_values = NULL;
    size_t volume_count = 0u;
    size_t packet_count = 0u;
    size_t filled_packet_count = 0u;
    size_t map_capacity = 0u;
    size_t codeblock_count = 0u;
    int packet_rc;
    int status = QW_STATUS_ERR_FORMAT;

    if (!tile_state || !tile || !components || !quantizations || !roi_shifts ||
        !component_count ||
        component_count > 4u || !packet_budget_remaining ||
        !codeblock_budget_remaining || !quantization_budget_remaining ||
        tile_state->packet_state.initialized) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (poc_payload) {
        packet_rc = qw_jp2_packet_progression_parse_poc(
            poc_payload, poc_payload_size, component_count,
            tile_state->layer_count, tile_state->component_resolution_counts,
            NULL, 0u, &volume_count);
        if (packet_rc != QW_JP2_PACKET_OK) {
            return qw_jp2_packet_status_to_image(packet_rc);
        }
        if (volume_count > SIZE_MAX / sizeof(*volumes)) {
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
        volumes = (QwJp2PacketProgressionVolume *)malloc(
            volume_count * sizeof(*volumes));
        if (!volumes) return QW_STATUS_ERR_OUT_OF_MEMORY;
        packet_rc = qw_jp2_packet_progression_parse_poc(
            poc_payload, poc_payload_size, component_count,
            tile_state->layer_count, tile_state->component_resolution_counts,
            volumes, volume_count, &filled_packet_count);
        if (packet_rc != QW_JP2_PACKET_OK || filled_packet_count != volume_count) {
            status = packet_rc == QW_JP2_PACKET_OK
                         ? QW_STATUS_ERR_FORMAT
                         : qw_jp2_packet_status_to_image(packet_rc);
            goto cleanup;
        }
    }

    packet_rc = qw_jp2_packet_progression_expand_volumes(
        tile, components, component_count, tile_state->layer_count,
        progression_order, volumes, volume_count, NULL, 0u, &packet_count);
    if (packet_rc != QW_JP2_PACKET_OK) {
        status = qw_jp2_packet_status_to_image(packet_rc);
        goto cleanup;
    }
    if (packet_count > *packet_budget_remaining ||
        packet_count > SIZE_MAX / sizeof(*packet_order) ||
        packet_count > SIZE_MAX / sizeof(*parsed.packet_precinct_indices) ||
        packet_count > SIZE_MAX / 2u) {
        status = QW_STATUS_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    if (packet_count != 0u) {
        size_t quantization_bytes;
        if ((size_t)component_count >
                SIZE_MAX / (size_t)QW_JP2_MAX_SUBBANDS) {
            status = QW_STATUS_ERR_RESOURCE_LIMIT;
            goto cleanup;
        }
        quantization_bytes = (size_t)component_count *
                             (size_t)QW_JP2_MAX_SUBBANDS;
        if (quantization_bytes > *quantization_budget_remaining) {
            status = QW_STATUS_ERR_RESOURCE_LIMIT;
            goto cleanup;
        }
        parsed.subband_max_bitplanes = (uint8_t *)calloc(
            quantization_bytes, sizeof(*parsed.subband_max_bitplanes));
        if (!parsed.subband_max_bitplanes) {
            status = QW_STATUS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        for (uint16_t component = 0u; component < component_count;
             ++component) {
            status = qw_jp2_quantization_fill_component_bitplanes(
                &quantizations[component],
                components[component].decomposition_levels,
                roi_shifts[component],
                parsed.subband_max_bitplanes +
                    (size_t)component * (size_t)QW_JP2_MAX_SUBBANDS);
            if (status != QW_STATUS_OK) goto cleanup;
        }
    }
    if (packet_count != 0u) {
        packet_order = (QwJp2PacketOrderEntry *)malloc(
            packet_count * sizeof(*packet_order));
        parsed.packet_precinct_indices = (size_t *)malloc(
            packet_count * sizeof(*parsed.packet_precinct_indices));
        if (!packet_order || !parsed.packet_precinct_indices) {
            status = QW_STATUS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        packet_rc = qw_jp2_packet_progression_expand_volumes(
            tile, components, component_count, tile_state->layer_count,
            progression_order, volumes, volume_count, packet_order,
            packet_count, &filled_packet_count);
        if (packet_rc != QW_JP2_PACKET_OK ||
            filled_packet_count != packet_count) {
            status = packet_rc == QW_JP2_PACKET_OK
                         ? QW_STATUS_ERR_FORMAT
                         : qw_jp2_packet_status_to_image(packet_rc);
            goto cleanup;
        }
        parsed.precinct_states = (QwJp2PacketPrecinctState *)calloc(
            packet_count, sizeof(*parsed.precinct_states));
        if (!parsed.precinct_states) {
            status = QW_STATUS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        map_capacity = 1u;
        while (map_capacity < packet_count * 2u) map_capacity *= 2u;
        map_keys = (uint64_t *)calloc(map_capacity, sizeof(*map_keys));
        map_values = (uint32_t *)calloc(map_capacity, sizeof(*map_values));
        if (!map_keys || !map_values) {
            status = QW_STATUS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        for (size_t packet_index = 0u; packet_index < packet_count;
             ++packet_index) {
            const QwJp2PacketOrderEntry *packet = &packet_order[packet_index];
            uint64_t stored_key;
            size_t slot;
            size_t context_index;

            if (packet->component_index >= component_count) {
                status = QW_STATUS_ERR_FORMAT;
                goto cleanup;
            }
            stored_key = qw_jp2_packet_precinct_key(packet) + 1u;
            slot = qw_jp2_packet_precinct_hash(stored_key - 1u) &
                   (map_capacity - 1u);
            while (map_keys[slot] != 0u && map_keys[slot] != stored_key) {
                slot = (slot + 1u) & (map_capacity - 1u);
            }
            if (map_keys[slot] == stored_key) {
                context_index = (size_t)map_values[slot] - 1u;
            } else {
                size_t context_codeblocks = 0u;
                if (parsed.precinct_state_count >=
                    (size_t)QW_JP2_MAX_TRACKED_PACKET_KEYS) {
                    status = QW_STATUS_ERR_RESOURCE_LIMIT;
                    goto cleanup;
                }
                context_index = parsed.precinct_state_count;
                status = qw_jp2_packet_precinct_state_init(
                    &parsed.precinct_states[context_index], tile,
                    &components[packet->component_index],
                    parsed.subband_max_bitplanes +
                        (size_t)packet->component_index *
                            (size_t)QW_JP2_MAX_SUBBANDS,
                    packet,
                    *codeblock_budget_remaining - codeblock_count,
                    &context_codeblocks);
                if (status != QW_STATUS_OK) goto cleanup;
                if (context_codeblocks >
                    *codeblock_budget_remaining - codeblock_count) {
                    qw_jp2_packet_precinct_state_clear(
                        &parsed.precinct_states[context_index]);
                    status = QW_STATUS_ERR_RESOURCE_LIMIT;
                    goto cleanup;
                }
                codeblock_count += context_codeblocks;
                ++parsed.precinct_state_count;
                map_keys[slot] = stored_key;
                map_values[slot] = (uint32_t)(context_index + 1u);
            }
            parsed.packet_precinct_indices[packet_index] = context_index;
        }
    }

    parsed.packet_order = packet_order;
    parsed.packet_count = packet_count;
    parsed.packet_order_capacity = packet_count;
    parsed.packet_precinct_capacity = packet_count;
    parsed.precinct_state_capacity = packet_count;
    parsed.tile_geometry = *tile;
    memcpy(parsed.component_geometry, components,
           (size_t)component_count * sizeof(*components));
    parsed.coding_style_flags = coding_style_flags;
    parsed.initialized = 1u;
    for (uint16_t component = 0u; component < component_count; ++component) {
        parsed.component_codeblock_styles[component] =
            components[component].codeblock_style;
    }
    tile_state->packet_state = parsed;
    *packet_budget_remaining -= packet_count;
    *codeblock_budget_remaining -= codeblock_count;
    if (parsed.subband_max_bitplanes) {
        *quantization_budget_remaining -=
            (size_t)component_count * (size_t)QW_JP2_MAX_SUBBANDS;
    }
    packet_order = NULL;
    memset(&parsed, 0, sizeof(parsed));
    status = QW_STATUS_OK;

cleanup:
    free(volumes);
    free(packet_order);
    free(map_keys);
    free(map_values);
    if (status != QW_STATUS_OK) {
        qw_jp2_tile_packet_state_clear(&parsed);
    }
    return status;
}

static uint64_t qw_jp2_packet_order_key(
    const QwJp2PacketOrderEntry *packet)
{
    return ((uint64_t)packet->layer_index << 48u) |
           ((uint64_t)packet->component_index << 40u) |
           ((uint64_t)packet->resolution_level << 32u) |
           (uint64_t)packet->precinct_index;
}

static int qw_jp2_packet_hash_capacity(size_t item_count,
                                       size_t *out_capacity)
{
    size_t required;
    size_t capacity = 1u;
    if (!out_capacity || item_count > SIZE_MAX / 2u) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    required = item_count * 2u;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2u) return QW_STATUS_ERR_RESOURCE_LIMIT;
        capacity *= 2u;
    }
    *out_capacity = capacity;
    return QW_STATUS_OK;
}

static int qw_jp2_packet_array_reserve(void **array,
                                       size_t element_size,
                                       size_t *capacity,
                                       size_t required,
                                       size_t maximum_count)
{
    size_t new_capacity;
    void *grown;
    if (!array || !element_size || !capacity || required > maximum_count) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (required <= *capacity) return QW_STATUS_OK;
    new_capacity = *capacity ? *capacity : 8u;
    if (new_capacity > maximum_count) new_capacity = maximum_count;
    while (new_capacity < required) {
        if (new_capacity > maximum_count / 2u) {
            new_capacity = maximum_count;
            break;
        }
        new_capacity *= 2u;
    }
    if (new_capacity < required || new_capacity > SIZE_MAX / element_size) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    grown = realloc(*array, new_capacity * element_size);
    if (!grown) return QW_STATUS_ERR_OUT_OF_MEMORY;
    *array = grown;
    *capacity = new_capacity;
    return QW_STATUS_OK;
}

static int qw_jp2_tile_packet_state_append_poc(
    QwJp2TilePacketState *state,
    uint16_t component_count,
    uint16_t layer_count,
    const uint8_t *poc_payload,
    size_t poc_payload_size,
    size_t *packet_budget_remaining,
    size_t *codeblock_budget_remaining)
{
    QwJp2PacketProgressionVolume *volumes = NULL;
    QwJp2PacketOrderEntry *new_packets = NULL;
    uint64_t *packet_keys = NULL;
    uint64_t *context_keys = NULL;
    uint32_t *context_values = NULL;
    uint8_t resolution_counts[4] = {0u};
    size_t volume_count = 0u;
    size_t filled_volume_count = 0u;
    size_t candidate_count = 0u;
    size_t filled_packet_count = 0u;
    size_t packet_map_capacity = 0u;
    size_t context_map_capacity = 0u;
    size_t maximum_packet_count =
        (size_t)QW_JP2_MAX_TRACKED_PACKET_KEYS;
    int packet_rc;
    int status = QW_STATUS_ERR_FORMAT;

    if (!state || !state->initialized || component_count == 0u ||
        component_count > 4u || !layer_count || !poc_payload ||
        poc_payload_size == 0u || !packet_budget_remaining ||
        !codeblock_budget_remaining) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    for (uint16_t component = 0u; component < component_count; ++component) {
        if (state->component_geometry[component].decomposition_levels > 32u) {
            return QW_STATUS_ERR_FORMAT;
        }
        resolution_counts[component] = (uint8_t)(
            state->component_geometry[component].decomposition_levels + 1u);
    }
    packet_rc = qw_jp2_packet_progression_parse_poc(
        poc_payload, poc_payload_size, component_count, layer_count,
        resolution_counts, NULL, 0u, &volume_count);
    if (packet_rc != QW_JP2_PACKET_OK) {
        return qw_jp2_packet_status_to_image(packet_rc);
    }
    if (volume_count > SIZE_MAX / sizeof(*volumes)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    volumes = (QwJp2PacketProgressionVolume *)malloc(
        volume_count * sizeof(*volumes));
    if (!volumes) return QW_STATUS_ERR_OUT_OF_MEMORY;
    packet_rc = qw_jp2_packet_progression_parse_poc(
        poc_payload, poc_payload_size, component_count, layer_count,
        resolution_counts, volumes, volume_count, &filled_volume_count);
    if (packet_rc != QW_JP2_PACKET_OK || filled_volume_count != volume_count) {
        status = packet_rc == QW_JP2_PACKET_OK
                     ? QW_STATUS_ERR_FORMAT
                     : qw_jp2_packet_status_to_image(packet_rc);
        goto cleanup;
    }
    packet_rc = qw_jp2_packet_progression_expand_volumes(
        &state->tile_geometry, state->component_geometry, component_count,
        layer_count, 0u, volumes, volume_count, NULL, 0u, &candidate_count);
    if (packet_rc != QW_JP2_PACKET_OK) {
        status = qw_jp2_packet_status_to_image(packet_rc);
        goto cleanup;
    }
    if (candidate_count > maximum_packet_count ||
        state->packet_count > maximum_packet_count - candidate_count ||
        candidate_count > SIZE_MAX / sizeof(*new_packets)) {
        status = QW_STATUS_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    if (candidate_count != 0u) {
        new_packets = (QwJp2PacketOrderEntry *)malloc(
            candidate_count * sizeof(*new_packets));
        if (!new_packets) {
            status = QW_STATUS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        packet_rc = qw_jp2_packet_progression_expand_volumes(
            &state->tile_geometry, state->component_geometry, component_count,
            layer_count, 0u, volumes, volume_count, new_packets,
            candidate_count, &filled_packet_count);
        if (packet_rc != QW_JP2_PACKET_OK ||
            filled_packet_count != candidate_count) {
            status = packet_rc == QW_JP2_PACKET_OK
                         ? QW_STATUS_ERR_FORMAT
                         : qw_jp2_packet_status_to_image(packet_rc);
            goto cleanup;
        }
    }

    status = qw_jp2_packet_hash_capacity(
        state->packet_count + candidate_count, &packet_map_capacity);
    if (status != QW_STATUS_OK) goto cleanup;
    status = qw_jp2_packet_hash_capacity(
        state->precinct_state_count + candidate_count,
        &context_map_capacity);
    if (status != QW_STATUS_OK) goto cleanup;
    packet_keys = (uint64_t *)calloc(packet_map_capacity, sizeof(*packet_keys));
    context_keys = (uint64_t *)calloc(context_map_capacity,
                                      sizeof(*context_keys));
    context_values = (uint32_t *)calloc(context_map_capacity,
                                        sizeof(*context_values));
    if (!packet_keys || !context_keys || !context_values) {
        status = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (size_t i = 0u; i < state->packet_count; ++i) {
        uint64_t stored_key = qw_jp2_packet_order_key(&state->packet_order[i]) + 1u;
        size_t slot = qw_jp2_packet_precinct_hash(stored_key - 1u) &
                      (packet_map_capacity - 1u);
        while (packet_keys[slot] != 0u && packet_keys[slot] != stored_key) {
            slot = (slot + 1u) & (packet_map_capacity - 1u);
        }
        packet_keys[slot] = stored_key;
    }
    for (size_t i = 0u; i < state->precinct_state_count; ++i) {
        const QwJp2PacketPrecinctState *precinct = &state->precinct_states[i];
        uint64_t stored_key = (((uint64_t)precinct->component_index << 40u) |
                               ((uint64_t)precinct->resolution_level << 32u) |
                               (uint64_t)precinct->precinct_index) + 1u;
        size_t slot = qw_jp2_packet_precinct_hash(stored_key - 1u) &
                      (context_map_capacity - 1u);
        while (context_keys[slot] != 0u && context_keys[slot] != stored_key) {
            slot = (slot + 1u) & (context_map_capacity - 1u);
        }
        context_keys[slot] = stored_key;
        context_values[slot] = (uint32_t)(i + 1u);
    }

    status = qw_jp2_packet_array_reserve(
        (void **)&state->packet_order, sizeof(*state->packet_order),
        &state->packet_order_capacity,
        state->packet_count + candidate_count, maximum_packet_count);
    if (status != QW_STATUS_OK) goto cleanup;
    status = qw_jp2_packet_array_reserve(
        (void **)&state->packet_precinct_indices,
        sizeof(*state->packet_precinct_indices),
        &state->packet_precinct_capacity,
        state->packet_count + candidate_count, maximum_packet_count);
    if (status != QW_STATUS_OK) goto cleanup;
    status = qw_jp2_packet_array_reserve(
        (void **)&state->precinct_states, sizeof(*state->precinct_states),
        &state->precinct_state_capacity,
        state->precinct_state_count + candidate_count, maximum_packet_count);
    if (status != QW_STATUS_OK) goto cleanup;

    for (size_t i = 0u; i < candidate_count; ++i) {
        const QwJp2PacketOrderEntry *packet = &new_packets[i];
        uint64_t packet_stored_key = qw_jp2_packet_order_key(packet) + 1u;
        size_t packet_slot = qw_jp2_packet_precinct_hash(
            packet_stored_key - 1u) & (packet_map_capacity - 1u);
        size_t context_index;
        while (packet_keys[packet_slot] != 0u &&
               packet_keys[packet_slot] != packet_stored_key) {
            packet_slot = (packet_slot + 1u) & (packet_map_capacity - 1u);
        }
        if (packet_keys[packet_slot] == packet_stored_key) continue;
        if (packet->component_index >= component_count ||
            *packet_budget_remaining == 0u) {
            status = packet->component_index >= component_count
                         ? QW_STATUS_ERR_FORMAT
                         : QW_STATUS_ERR_RESOURCE_LIMIT;
            goto cleanup;
        }
        packet_keys[packet_slot] = packet_stored_key;

        {
            uint64_t context_stored_key =
                qw_jp2_packet_precinct_key(packet) + 1u;
            size_t context_slot = qw_jp2_packet_precinct_hash(
                context_stored_key - 1u) & (context_map_capacity - 1u);
            while (context_keys[context_slot] != 0u &&
                   context_keys[context_slot] != context_stored_key) {
                context_slot = (context_slot + 1u) &
                               (context_map_capacity - 1u);
            }
            if (context_keys[context_slot] != context_stored_key) {
                size_t context_codeblocks = 0u;
                if (state->precinct_state_count >= maximum_packet_count) {
                    status = QW_STATUS_ERR_RESOURCE_LIMIT;
                    goto cleanup;
                }
                if (!state->subband_max_bitplanes) {
                    status = QW_STATUS_ERR_FORMAT;
                    goto cleanup;
                }
                context_index = state->precinct_state_count;
                status = qw_jp2_packet_precinct_state_init(
                    &state->precinct_states[context_index],
                    &state->tile_geometry,
                    &state->component_geometry[packet->component_index],
                    state->subband_max_bitplanes +
                        (size_t)packet->component_index *
                            (size_t)QW_JP2_MAX_SUBBANDS,
                    packet, *codeblock_budget_remaining,
                    &context_codeblocks);
                if (status != QW_STATUS_OK) goto cleanup;
                ++state->precinct_state_count;
                *codeblock_budget_remaining -= context_codeblocks;
                context_keys[context_slot] = context_stored_key;
                context_values[context_slot] = (uint32_t)(context_index + 1u);
            } else {
                context_index = (size_t)context_values[context_slot] - 1u;
            }
        }
        state->packet_order[state->packet_count] = *packet;
        state->packet_precinct_indices[state->packet_count] = context_index;
        ++state->packet_count;
        --*packet_budget_remaining;
    }
    status = QW_STATUS_OK;

cleanup:
    free(context_values);
    free(context_keys);
    free(packet_keys);
    free(new_packets);
    free(volumes);
    return status;
}

static int qw_jp2_packet_codeblock_body_reserve(
    QwJp2PacketCodeBlockBody *body,
    size_t additional_size,
    size_t *budget_remaining)
{
    size_t required_size;
    size_t new_capacity;
    size_t growth;
    uint8_t *replacement;

    if (!body || !budget_remaining || body->size > body->capacity ||
        body->capacity > (size_t)QW_JP2_PACKET_CODEBLOCK_MAX_BYTES) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (additional_size >
        (size_t)QW_JP2_PACKET_CODEBLOCK_MAX_BYTES - body->size) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    required_size = body->size + additional_size;
    if (required_size <= body->capacity) return QW_STATUS_OK;

    new_capacity = body->capacity ? body->capacity : 64u;
    while (new_capacity < required_size) {
        if (new_capacity >
            (size_t)QW_JP2_PACKET_CODEBLOCK_MAX_BYTES / 2u) {
            new_capacity = (size_t)QW_JP2_PACKET_CODEBLOCK_MAX_BYTES;
            break;
        }
        new_capacity *= 2u;
    }
    if (new_capacity > (size_t)QW_JP2_PACKET_CODEBLOCK_MAX_BYTES) {
        new_capacity = (size_t)QW_JP2_PACKET_CODEBLOCK_MAX_BYTES;
    }
    growth = new_capacity - body->capacity;
    if (growth > *budget_remaining) {
        new_capacity = required_size;
        growth = new_capacity - body->capacity;
    }
    if (growth > *budget_remaining) return QW_STATUS_ERR_RESOURCE_LIMIT;

    replacement = (uint8_t *)realloc(body->data, new_capacity);
    if (!replacement) return QW_STATUS_ERR_OUT_OF_MEMORY;
    body->data = replacement;
    body->capacity = new_capacity;
    *budget_remaining -= growth;
    return QW_STATUS_OK;
}

static int qw_jp2_traverse_tile_part_packets(
    QwJp2TilePacketState *state,
    uint16_t component_count,
    const uint8_t *header_data,
    size_t header_size,
    const uint8_t *body_data,
    size_t body_size,
    int packed_headers,
    size_t *body_budget_remaining,
    size_t *segment_metadata_budget_remaining)
{
    QwJp2PacketBitReader reader;
    size_t body_offset = 0u;
    uint8_t has_sop;
    uint8_t has_eph;
    int packet_rc;

    if (!state || !state->initialized || !body_budget_remaining ||
        !segment_metadata_budget_remaining ||
        component_count == 0u ||
        (!header_data && header_size != 0u) ||
        (!body_data && body_size != 0u) ||
        (!packed_headers && header_size != body_size)) {
        return QW_STATUS_ERR_FORMAT;
    }
    has_sop = (uint8_t)((state->coding_style_flags & 0x02u) != 0u);
    has_eph = (uint8_t)((state->coding_style_flags & 0x04u) != 0u);
    packet_rc = qw_jp2_packet_bit_reader_init(&reader, header_data, header_size);
    if (packet_rc != QW_JP2_PACKET_OK) {
        return qw_jp2_packet_status_to_image(packet_rc);
    }

    while (state->next_packet_index < state->packet_count &&
           reader.byte_offset < reader.size) {
        const QwJp2PacketOrderEntry *packet =
            &state->packet_order[state->next_packet_index];
        size_t context_index =
            state->packet_precinct_indices[state->next_packet_index];
        QwJp2PacketPrecinctState *precinct_state;
        size_t contribution_count = 0u;
        size_t segment_length_count = 0u;
        size_t header_bytes = 0u;
        size_t packet_body_bytes = 0u;
        uint8_t is_empty = 0u;
        size_t subband_count;
        size_t sop_offset;
        size_t maximum_contributions = 0u;
        size_t packet_body_offset;
        const uint8_t *packet_body_data = NULL;
        QwJp2PacketBodyCursor body_cursor;

        if (packet->component_index >= component_count ||
            context_index >= state->precinct_state_count) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (has_sop) {
            sop_offset = packed_headers ? body_offset : reader.byte_offset;
            if (sop_offset > body_size || body_size - sop_offset < 6u ||
                body_data[sop_offset] != 0xffu ||
                body_data[sop_offset + 1u] != QW_J2K_MARKER_SOP ||
                body_data[sop_offset + 2u] != 0u ||
                body_data[sop_offset + 3u] != 4u ||
                body_data[sop_offset + 4u] !=
                    (uint8_t)(state->next_packet_index >> 8u) ||
                body_data[sop_offset + 5u] !=
                    (uint8_t)state->next_packet_index) {
                return QW_STATUS_ERR_FORMAT;
            }
            if (packed_headers) {
                body_offset += 6u;
            } else {
                reader.byte_offset += 6u;
            }
        }
        precinct_state = &state->precinct_states[context_index];
        subband_count = packet->resolution_level == 0u ? 1u : 3u;
        for (size_t band = 0u; band < subband_count; ++band) {
            size_t band_count =
                precinct_state->subbands[band].codeblock_state_capacity;
            if (band_count >
                (size_t)QW_JP2_MAX_TRACKED_PACKET_CODEBLOCKS -
                    maximum_contributions) {
                return QW_STATUS_ERR_RESOURCE_LIMIT;
            }
            maximum_contributions += band_count;
        }
        if (maximum_contributions > state->contribution_capacity) {
            size_t allocation_size;
            QwJp2PacketCodeBlockContribution *replacement;
            if (maximum_contributions >
                    SIZE_MAX / sizeof(*state->contributions)) {
                return QW_STATUS_ERR_RESOURCE_LIMIT;
            }
            allocation_size = maximum_contributions *
                              sizeof(*state->contributions);
            replacement = (QwJp2PacketCodeBlockContribution *)realloc(
                state->contributions, allocation_size);
            if (!replacement) return QW_STATUS_ERR_OUT_OF_MEMORY;
            state->contributions = replacement;
            state->contribution_capacity = maximum_contributions;
        }
        {
            uint8_t component_codeblock_style =
                state->component_codeblock_styles[packet->component_index];
            if ((component_codeblock_style & 0x04u) != 0u) {
                size_t segment_capacity =
                    maximum_contributions >
                            (size_t)QW_JP2_MAX_PACKET_SEGMENT_LENGTHS /
                                QW_JP2_PACKET_MAX_PASSES
                        ? (size_t)QW_JP2_MAX_PACKET_SEGMENT_LENGTHS
                        : maximum_contributions *
                              QW_JP2_PACKET_MAX_PASSES;
                uint32_t *replacement;
                if (segment_capacity == 0u) segment_capacity = 1u;
                if (segment_capacity > state->segment_length_capacity) {
                    size_t growth_bytes =
                        (segment_capacity - state->segment_length_capacity) *
                        sizeof(*state->segment_lengths);
                    if (segment_capacity > SIZE_MAX /
                                               sizeof(*state->segment_lengths) ||
                        growth_bytes > *segment_metadata_budget_remaining) {
                        return QW_STATUS_ERR_RESOURCE_LIMIT;
                    }
                    replacement = (uint32_t *)realloc(
                        state->segment_lengths,
                        segment_capacity * sizeof(*state->segment_lengths));
                    if (!replacement) return QW_STATUS_ERR_OUT_OF_MEMORY;
                    state->segment_lengths = replacement;
                    state->segment_length_capacity = segment_capacity;
                    *segment_metadata_budget_remaining -= growth_bytes;
                }
                packet_rc = qw_jp2_packet_read_header_with_segments(
                    &reader, precinct_state->subbands, subband_count,
                    packet->layer_index, component_codeblock_style,
                    state->contributions, state->contribution_capacity,
                    &contribution_count, &is_empty, &header_bytes,
                    &packet_body_bytes, state->segment_lengths,
                    state->segment_length_capacity, &segment_length_count);
            } else {
                packet_rc = qw_jp2_packet_read_header(
                    &reader, precinct_state->subbands, subband_count,
                    packet->layer_index, component_codeblock_style,
                    state->contributions, state->contribution_capacity,
                    &contribution_count, &is_empty, &header_bytes,
                    &packet_body_bytes);
            }
        }
        if (packet_rc != QW_JP2_PACKET_OK) {
            return qw_jp2_packet_status_to_image(packet_rc);
        }
        if (contribution_count != (is_empty ? 0u : maximum_contributions)) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (has_eph) {
            if (reader.byte_offset > reader.size ||
                reader.size - reader.byte_offset < 2u ||
                reader.data[reader.byte_offset] != 0xffu ||
                reader.data[reader.byte_offset + 1u] != QW_J2K_MARKER_EPH) {
                return QW_STATUS_ERR_FORMAT;
            }
            reader.byte_offset += 2u;
        }
        if (header_bytes == 0u) {
            return QW_STATUS_ERR_FORMAT;
        }
        packet_body_offset = packed_headers ? body_offset : reader.byte_offset;
        if (packet_body_offset > body_size ||
            packet_body_bytes > body_size - packet_body_offset ||
            (!packed_headers &&
             (reader.byte_offset > reader.size ||
              packet_body_bytes > reader.size - reader.byte_offset))) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (body_data) packet_body_data = body_data + packet_body_offset;
        packet_rc = qw_jp2_packet_body_cursor_init(
            &body_cursor, packet_body_data, packet_body_bytes);
        if (packet_rc != QW_JP2_PACKET_OK) {
            return qw_jp2_packet_status_to_image(packet_rc);
        }
        if (!is_empty) {
            size_t contribution_index = 0u;
            for (size_t band = 0u; band < subband_count; ++band) {
                size_t band_count =
                    precinct_state->subbands[band].codeblock_state_capacity;
                for (size_t i = 0u; i < band_count; ++i) {
                    QwJp2PacketCodeBlockContribution *contribution;
                    QwJp2PacketCodeBlockBody *body;
                    if (contribution_index >= contribution_count) {
                        return QW_STATUS_ERR_FORMAT;
                    }
                    contribution = &state->contributions[contribution_index++];
                    body = &precinct_state->bodies[band][i];
                    if (contribution->included &&
                        contribution->codeword_segment_length != 0u) {
                        int reserve_status =
                            qw_jp2_packet_codeblock_body_reserve(
                                body,
                                (size_t)contribution->codeword_segment_length,
                                body_budget_remaining);
                        if (reserve_status != QW_STATUS_OK) {
                            return reserve_status;
                        }
                    }
                    if ((state->component_codeblock_styles[
                             packet->component_index] & 0x04u) != 0u) {
                        const uint32_t *codeblock_segment_lengths = NULL;
                        size_t codeblock_segment_count = 0u;
                        if (contribution->segment_count != 0u) {
                            if (contribution->segment_offset >
                                    segment_length_count ||
                                contribution->segment_count >
                                    segment_length_count -
                                        contribution->segment_offset) {
                                return QW_STATUS_ERR_FORMAT;
                            }
                            codeblock_segment_lengths =
                                state->segment_lengths +
                                contribution->segment_offset;
                            codeblock_segment_count =
                                contribution->segment_count;
                        }
                        packet_rc =
                            qw_jp2_packet_body_append_codeblock_with_segments(
                                &body_cursor, contribution,
                                &precinct_state->codeblocks[band][i], body,
                                state->component_codeblock_styles[
                                    packet->component_index],
                                codeblock_segment_lengths,
                                codeblock_segment_count,
                                segment_metadata_budget_remaining);
                    } else {
                        packet_rc = qw_jp2_packet_body_append_codeblock(
                            &body_cursor, contribution,
                            &precinct_state->codeblocks[band][i], body);
                    }
                    if (packet_rc != QW_JP2_PACKET_OK) {
                        return qw_jp2_packet_status_to_image(packet_rc);
                    }
                }
            }
            if (contribution_index != contribution_count) {
                return QW_STATUS_ERR_FORMAT;
            }
        }
        packet_rc = qw_jp2_packet_body_cursor_finish(&body_cursor);
        if (packet_rc != QW_JP2_PACKET_OK) {
            return qw_jp2_packet_status_to_image(packet_rc);
        }
        if (packed_headers) {
            body_offset += packet_body_bytes;
        } else {
            reader.byte_offset += packet_body_bytes;
        }
        reader.current_byte = 0u;
        reader.bits_remaining = 0u;
        reader.previous_was_ff = 0u;
        ++state->next_packet_index;
    }

    if (reader.byte_offset != reader.size ||
        (packed_headers && body_offset != body_size)) {
        return QW_STATUS_ERR_FORMAT;
    }
    return QW_STATUS_OK;
}

static int qw_jp2_validate_tile_precinct_grids(
    const QwJp2PacketTileGeometry *tile,
    const QwJp2PacketComponentGeometry *components,
    uint16_t component_count)
{
    size_t total_precincts = 0u;

    if (!tile || !components || component_count == 0u || component_count > 4u) {
        return QW_STATUS_ERR_FORMAT;
    }
    for (uint16_t component = 0u; component < component_count; ++component) {
        const QwJp2PacketComponentGeometry *component_geometry =
            &components[component];
        for (uint8_t resolution = 0u;
             resolution <= component_geometry->decomposition_levels;
             ++resolution) {
            QwJp2PacketPrecinctGrid grid;
            int rc = qw_jp2_packet_precinct_grid_compute(
                tile, component_geometry, resolution, &grid);
            if (rc == QW_JP2_PACKET_ERR_RESOURCE) {
                return QW_STATUS_ERR_RESOURCE_LIMIT;
            }
            if (rc != QW_JP2_PACKET_OK) return QW_STATUS_ERR_FORMAT;
            if ((size_t)grid.precinct_count >
                (size_t)QW_JP2_PACKET_PROGRESSION_MAX_PACKETS -
                    total_precincts) {
                return QW_STATUS_ERR_RESOURCE_LIMIT;
            }
            total_precincts += grid.precinct_count;
        }
    }
    return QW_STATUS_OK;
}

static int qw_jp2_read_marker(const uint8_t *data,
                              size_t size,
                              size_t limit,
                              size_t *position,
                              size_t *marker_offset,
                              uint8_t *marker)
{
    size_t cursor;

    if (!data || !position || !marker_offset || !marker || limit > size ||
        *position >= limit || data[*position] != 0xffu) {
        return QW_STATUS_ERR_FORMAT;
    }
    cursor = *position;
    *marker_offset = cursor;
    while (cursor < limit && data[cursor] == 0xffu) ++cursor;
    if (cursor >= limit || data[cursor] == 0x00u) {
        return QW_STATUS_ERR_FORMAT;
    }
    *marker = data[cursor++];
    *position = cursor;
    return QW_STATUS_OK;
}

static int qw_jp2_tier1_orientation_from_subband(
    uint8_t subband_orientation,
    QwJp2Tier1Orientation *out_orientation)
{
    if (!out_orientation) return QW_STATUS_ERR_INVALID_ARGUMENT;
    switch (subband_orientation) {
    case QW_JP2_SUBBAND_LL:
    case QW_JP2_SUBBAND_LH:
        *out_orientation = QW_JP2_T1_ORIENTATION_LL_LH;
        return QW_STATUS_OK;
    case QW_JP2_SUBBAND_HL:
        *out_orientation = QW_JP2_T1_ORIENTATION_HL;
        return QW_STATUS_OK;
    case QW_JP2_SUBBAND_HH:
        *out_orientation = QW_JP2_T1_ORIENTATION_HH;
        return QW_STATUS_OK;
    default:
        return QW_STATUS_ERR_FORMAT;
    }
}

static size_t qw_jp2_internal_resolution_extent_at_origin(
    uint32_t origin,
    uint32_t extent,
    unsigned downshift)
{
    uint64_t scale;
    uint64_t start;
    uint64_t end;

    if (downshift >= 63u) return 0u;
    scale = UINT64_C(1) << downshift;
    start = origin;
    end = start + extent;
    start = start / scale + (start % scale != 0u ? 1u : 0u);
    end = end / scale + (end % scale != 0u ? 1u : 0u);
    return (size_t)(end - start);
}

static uint8_t qw_jp2_internal_resolution_first_is_high(
    uint32_t origin,
    unsigned downshift)
{
    uint64_t scale;
    uint64_t first;

    if (downshift >= 63u) return 0u;
    scale = UINT64_C(1) << downshift;
    first = origin / scale + (origin % scale != 0u ? 1u : 0u);
    return (uint8_t)(first & 1u);
}

static uint64_t qw_jp2_internal_subband_coordinate_origin(
    uint32_t component_origin,
    uint8_t source_decomposition_levels,
    uint8_t resolution_level,
    int high_pass)
{
    uint64_t scale;
    uint64_t phase = 0u;
    uint64_t coordinate = component_origin;

    if (resolution_level == 0u) {
        scale = UINT64_C(1) << source_decomposition_levels;
    } else {
        unsigned decomposition_level =
            (unsigned)source_decomposition_levels - resolution_level + 1u;
        scale = UINT64_C(1) << decomposition_level;
        if (high_pass) phase = scale >> 1u;
    }
    if (coordinate <= phase) return 0u;
    coordinate -= phase;
    return coordinate / scale + (coordinate % scale != 0u ? 1u : 0u);
}

static int qw_jp2_internal_codeblock_reconstruction_shift(
    const QwJp2PacketCodeBlockBody *body,
    uint8_t *out_shift)
{
    unsigned available_bitplanes;
    unsigned decoded_bitplanes;
    unsigned shift;

    if (!body || !out_shift) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_shift = 0u;
    if (!body->initialized) return QW_STATUS_OK;
    if (body->max_bitplanes <= body->zero_bitplanes ||
        body->coding_passes == 0u) {
        return QW_STATUS_ERR_FORMAT;
    }
    available_bitplanes =
        (unsigned)body->max_bitplanes - (unsigned)body->zero_bitplanes;
    decoded_bitplanes = ((unsigned)body->coding_passes + 2u) / 3u;
    if (decoded_bitplanes == 0u || decoded_bitplanes > available_bitplanes) {
        return QW_STATUS_ERR_FORMAT;
    }
    shift = available_bitplanes - decoded_bitplanes;
    if (shift > 63u) return QW_STATUS_ERR_FORMAT;
    *out_shift = (uint8_t)shift;
    return QW_STATUS_OK;
}

static int qw_jp2_internal_scatter_codeblock(
    QwJp2InternalDecodeContext *context,
    uint8_t resolution_level,
    uint8_t subband_orientation,
    uint64_t x0,
    uint64_t y0,
    size_t width,
    size_t height,
    const QwJp2Tier1Coefficient *coefficients,
    uint8_t reconstruction_shift,
    int use_per_coefficient_reconstruction)
{
    size_t current_width;
    size_t current_height;
    size_t low_width;
    size_t low_height;
    size_t band_width;
    size_t band_height;
    size_t x_offset;
    size_t y_offset;
    uint64_t subband_origin_x;
    uint64_t subband_origin_y;
    int high_x;
    int high_y;

    if (!context || !context->initialized || !coefficients ||
        resolution_level > context->decomposition_levels || width == 0u ||
        height == 0u || subband_orientation > QW_JP2_SUBBAND_HH) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (context->roi_shift >= 64u) return QW_STATUS_ERR_UNSUPPORTED;
    high_x = (subband_orientation & 1u) != 0u;
    high_y = (subband_orientation & 2u) != 0u;
    if (resolution_level == 0u && (high_x || high_y)) {
        return QW_STATUS_ERR_FORMAT;
    }
    current_width = qw_jp2_internal_resolution_extent_at_origin(
        context->component_x_origin, context->width,
        context->decomposition_levels - resolution_level);
    current_height = qw_jp2_internal_resolution_extent_at_origin(
        context->component_y_origin, context->height,
        context->decomposition_levels - resolution_level);
    if (resolution_level == 0u) {
        low_width = current_width;
        low_height = current_height;
    } else {
        low_width = qw_jp2_internal_resolution_extent_at_origin(
            context->component_x_origin, context->width,
            context->decomposition_levels - resolution_level + 1u);
        low_height = qw_jp2_internal_resolution_extent_at_origin(
            context->component_y_origin, context->height,
            context->decomposition_levels - resolution_level + 1u);
    }
    band_width = high_x ? current_width - low_width : low_width;
    band_height = high_y ? current_height - low_height : low_height;
    x_offset = high_x ? low_width : 0u;
    y_offset = high_y ? low_height : 0u;
    subband_origin_x = qw_jp2_internal_subband_coordinate_origin(
        context->component_x_origin, context->source_decomposition_levels,
        resolution_level, high_x);
    subband_origin_y = qw_jp2_internal_subband_coordinate_origin(
        context->component_y_origin, context->source_decomposition_levels,
        resolution_level, high_y);
    if (x0 < subband_origin_x || y0 < subband_origin_y) {
        return QW_STATUS_ERR_FORMAT;
    }
    x0 -= subband_origin_x;
    y0 -= subband_origin_y;
    if (x0 > band_width || width > band_width - (size_t)x0 ||
        y0 > band_height || height > band_height - (size_t)y0) {
        return QW_STATUS_ERR_FORMAT;
    }

    for (size_t y = 0u; y < height; ++y) {
        size_t packed_y = y_offset + (size_t)y0 + y;
        for (size_t x = 0u; x < width; ++x) {
            size_t packed_x = x_offset + (size_t)x0 + x;
            size_t index = packed_y * (size_t)context->width + packed_x;
            size_t coverage_byte = index >> 3u;
            uint8_t coverage_mask = (uint8_t)(1u << (index & 7u));
            const QwJp2Tier1Coefficient *coefficient =
                &coefficients[y * width + x];
            uint64_t magnitude = coefficient->magnitude;
            uint8_t coefficient_reconstruction_shift =
                use_per_coefficient_reconstruction
                    ? coefficient->reconstruction_shift
                    : reconstruction_shift;
            int64_t value;

            if (packed_x >= current_width || packed_y >= current_height ||
                index >= context->pixel_count ||
                (context->coverage[coverage_byte] & coverage_mask) != 0u) {
                return QW_STATUS_ERR_FORMAT;
            }
            if (context->roi_shift != 0u) {
                uint64_t threshold = UINT64_C(1) << context->roi_shift;
                if (magnitude >= threshold) {
                    magnitude >>= context->roi_shift;
                    if (coefficient_reconstruction_shift !=
                            QW_JP2_T1_NO_RECONSTRUCTION_MIDPOINT &&
                        coefficient_reconstruction_shift > context->roi_shift) {
                        coefficient_reconstruction_shift =
                            (uint8_t)(coefficient_reconstruction_shift -
                                      context->roi_shift);
                    } else if (coefficient_reconstruction_shift !=
                               QW_JP2_T1_NO_RECONSTRUCTION_MIDPOINT) {
                        coefficient_reconstruction_shift = 0u;
                    }
                }
            }
            if (magnitude > (uint64_t)(INT64_MAX / INT64_C(128))) {
                return QW_STATUS_ERR_FORMAT;
            }
            value = (int64_t)magnitude;
            if (coefficient->negative && value != 0) value = -value;
            if (context->wavelet_transform == 0u) {
                uint64_t encoded;
                if (coefficient_reconstruction_shift > 63u) {
                    return QW_STATUS_ERR_FORMAT;
                }
                encoded = magnitude |
                    ((uint64_t)coefficient_reconstruction_shift << 56u);
                if (coefficient->negative && magnitude != 0u) {
                    encoded |= UINT64_C(1) << 63u;
                }
                memcpy((uint8_t *)context->coefficient_plane +
                           index * sizeof(encoded),
                       &encoded, sizeof(encoded));
            } else {
                ((int64_t *)context->coefficient_plane)[index] = value;
            }
            context->coverage[coverage_byte] |= coverage_mask;
            ++context->covered_count;
        }
    }
    return QW_STATUS_OK;
}

static int qw_jp2_tile_packet_state_decode_bodies(
    const QwJp2TilePacketState *state,
    QwJp2Tier1Coefficient *coefficients,
    size_t coefficient_capacity,
    QwJp2InternalDecodeContext *internal_decode)
{
    if (!state || !state->initialized || !coefficients ||
        coefficient_capacity < (size_t)QW_JP2_MAX_CODEBLOCK_COEFFICIENTS) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }

    for (size_t precinct_index = 0u;
         precinct_index < state->precinct_state_count; ++precinct_index) {
        const QwJp2PacketPrecinctState *precinct =
            &state->precinct_states[precinct_index];
        QwJp2InternalDecodeContext *component_decode = internal_decode
            ? qw_jp2_internal_component_context(
                  internal_decode, precinct->component_index)
            : NULL;
        QwJp2PacketOrderEntry packet = {0};
        size_t band_count;

        if (precinct->component_index >= 4u ||
            precinct->resolution_level >= QW_JP2_PACKET_MAX_RESOLUTION_LEVELS) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (internal_decode && !component_decode) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if (component_decode && precinct->resolution_level >
                                    component_decode->decomposition_levels) {
            continue;
        }
        packet.component_index = precinct->component_index;
        packet.resolution_level = precinct->resolution_level;
        packet.precinct_index = precinct->precinct_index;
        packet.reference_x = precinct->reference_x;
        packet.reference_y = precinct->reference_y;
        band_count = precinct->resolution_level == 0u ? 1u : 3u;

        for (size_t band = 0u; band < band_count; ++band) {
            uint8_t subband_orientation = precinct->resolution_level == 0u
                ? QW_JP2_SUBBAND_LL
                : (uint8_t)(QW_JP2_SUBBAND_HL + band);
            QwJp2Tier1Orientation tier1_orientation;
            QwJp2PacketPrecinctCodeBlockGrid grid;
            int packet_rc;

            packet_rc = qw_jp2_tier1_orientation_from_subband(
                subband_orientation, &tier1_orientation);
            if (packet_rc != QW_STATUS_OK) return packet_rc;
            packet_rc = qw_jp2_packet_precinct_codeblock_grid_compute(
                &state->tile_geometry,
                &state->component_geometry[precinct->component_index],
                &packet, subband_orientation, &grid);
            if (packet_rc != QW_JP2_PACKET_OK) {
                return qw_jp2_packet_status_to_image(packet_rc);
            }
            if (grid.codeblock_count !=
                    precinct->subbands[band].codeblock_state_capacity ||
                (grid.codeblock_count != 0u &&
                 (!precinct->bodies[band] || !precinct->codeblocks[band]))) {
                return QW_STATUS_ERR_FORMAT;
            }

            uint64_t codeblock_width =
                UINT64_C(1) << grid.codeblock_width_exponent;
            uint64_t codeblock_height =
                UINT64_C(1) << grid.codeblock_height_exponent;
            for (uint32_t codeblock_index = 0u;
                 codeblock_index < grid.codeblock_count; ++codeblock_index) {
                const QwJp2PacketCodeBlockBody *body =
                    &precinct->bodies[band][codeblock_index];
                uint64_t codeblock_x = (uint64_t)grid.codeblock_x0 +
                    (uint64_t)(codeblock_index % grid.codeblocks_wide);
                uint64_t codeblock_y = (uint64_t)grid.codeblock_y0 +
                    (uint64_t)(codeblock_index / grid.codeblocks_wide);
                uint64_t x0 = codeblock_x * codeblock_width;
                uint64_t y0 = codeblock_y * codeblock_height;
                uint64_t x1 = x0 + codeblock_width;
                uint64_t y1 = y0 + codeblock_height;
                uint64_t clipped_width;
                uint64_t clipped_height;
                size_t coefficient_count;
                uint8_t reconstruction_shift = 0u;

                if (!body->initialized && !internal_decode) continue;
                if (x0 < grid.subband_x0) x0 = grid.subband_x0;
                if (y0 < grid.subband_y0) y0 = grid.subband_y0;
                if (x1 > grid.subband_x1) x1 = grid.subband_x1;
                if (y1 > grid.subband_y1) y1 = grid.subband_y1;
                if (x1 <= x0 || y1 <= y0) return QW_STATUS_ERR_FORMAT;
                clipped_width = x1 - x0;
                clipped_height = y1 - y0;
                if (clipped_width > QW_JP2_MAX_CODEBLOCK_DIMENSION ||
                    clipped_height > QW_JP2_MAX_CODEBLOCK_DIMENSION ||
                    clipped_width > SIZE_MAX / clipped_height) {
                    return QW_STATUS_ERR_RESOURCE_LIMIT;
                }
                coefficient_count =
                    (size_t)(clipped_width * clipped_height);
                if (coefficient_count > coefficient_capacity) {
                    return QW_STATUS_ERR_RESOURCE_LIMIT;
                }
                if (component_decode &&
                    component_decode->wavelet_transform == 0u &&
                    body->initialized &&
                    !internal_decode->use_per_coefficient_reconstruction) {
                    int shift_status =
                        qw_jp2_internal_codeblock_reconstruction_shift(
                            body, &reconstruction_shift);
                    if (shift_status != QW_STATUS_OK) return shift_status;
                }
                if (body->initialized) {
                    packet_rc = qw_jp2_packet_codeblock_decode(
                        body, (uint32_t)clipped_width,
                        (uint32_t)clipped_height, tier1_orientation,
                        state->component_codeblock_styles[
                            precinct->component_index],
                        coefficients, coefficient_count);
                    if (packet_rc != QW_JP2_PACKET_OK) {
                        if (!internal_decode &&
                            packet_rc == QW_JP2_PACKET_ERR_UNSUPPORTED) {
                            /* Structural validation can defer unsupported
                             * Tier-1 modes to the dispatched decoder. */
                            continue;
                        }
                        return qw_jp2_packet_status_to_image(packet_rc);
                    }
                } else {
                    memset(coefficients, 0,
                           coefficient_count * sizeof(*coefficients));
                }
                if (component_decode) {
                    packet_rc = qw_jp2_internal_scatter_codeblock(
                        component_decode, precinct->resolution_level,
                        subband_orientation, x0, y0,
                        (size_t)clipped_width, (size_t)clipped_height,
                        coefficients, reconstruction_shift,
                        internal_decode->use_per_coefficient_reconstruction);
                    if (packet_rc != QW_STATUS_OK) return packet_rc;
                }
            }
        }
    }
    return QW_STATUS_OK;
}

static int qw_jp2_internal_dequantize_97_band(
    const QwJp2InternalDecodeContext *context,
    double *coefficients,
    size_t x_origin,
    size_t y_origin,
    size_t band_width,
    size_t band_height,
    size_t subband_index,
    unsigned subband_gain,
    unsigned decomposition_level)
{
    int epsilon;
    uint16_t mantissa;
    int step_exponent;
    double step_size;
    size_t stride;

    if (!context || !coefficients || subband_gain > 2u ||
        x_origin > (size_t)context->width ||
        band_width > (size_t)context->width - x_origin ||
        y_origin > (size_t)context->height ||
        band_height > (size_t)context->height - y_origin) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (context->quantization_style == 1u) {
        if (context->quantization_subband_count != 1u) {
            return QW_STATUS_ERR_FORMAT;
        }
        epsilon = (int)context->quantization_exponents[0] -
                  (int)context->source_decomposition_levels +
                  (int)decomposition_level;
        mantissa = context->quantization_mantissas[0];
    } else if (context->quantization_style == 2u) {
        if (subband_index >=
            (size_t)context->quantization_subband_count) {
            return QW_STATUS_ERR_FORMAT;
        }
        epsilon = (int)context->quantization_exponents[subband_index];
        mantissa = context->quantization_mantissas[subband_index];
    } else {
        return QW_STATUS_ERR_FORMAT;
    }
    step_exponent = (int)context->precision + (int)subband_gain - epsilon;
    step_size = ldexp(1.0 + (double)mantissa / 2048.0, step_exponent);
    if (!isfinite(step_size) || step_size <= 0.0) {
        return QW_STATUS_ERR_FORMAT;
    }

    stride = (size_t)context->width;
    for (size_t y = 0u; y < band_height; ++y) {
        size_t row_offset = (y_origin + y) * stride + x_origin;
        for (size_t x = 0u; x < band_width; ++x) {
            size_t index = row_offset + x;
            double reconstructed = coefficients[index] * step_size;
            if (!isfinite(reconstructed)) return QW_STATUS_ERR_FORMAT;
            coefficients[index] = reconstructed;
        }
    }
    return QW_STATUS_OK;
}

static int qw_jp2_internal_dequantize_97(
    QwJp2InternalDecodeContext *context)
{
    uint8_t *plane_bytes;
    double *coefficients;
    size_t width;
    size_t height;
    int rc;

    if (!context || context->wavelet_transform != 0u ||
        (context->quantization_style != 1u &&
         context->quantization_style != 2u) ||
        sizeof(double) != sizeof(int64_t)) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    plane_bytes = (uint8_t *)context->coefficient_plane;
    coefficients = (double *)context->coefficient_plane;
    for (size_t i = 0u; i < context->pixel_count; ++i) {
        uint64_t encoded;
        uint64_t magnitude;
        uint8_t reconstruction_shift;
        int negative;
        double coefficient;
        memcpy(&encoded, plane_bytes + i * sizeof(encoded), sizeof(encoded));
        magnitude = encoded & ((UINT64_C(1) << 56u) - 1u);
        reconstruction_shift = (uint8_t)((encoded >> 56u) & 0x3fu);
        negative = (encoded & (UINT64_C(1) << 63u)) != 0u;
        if (magnitude == 0u) {
            coefficient = 0.0;
        } else if (reconstruction_shift ==
                   QW_JP2_T1_NO_RECONSTRUCTION_MIDPOINT) {
            coefficient = (double)magnitude;
            if (negative) coefficient = -coefficient;
        } else {
            double reconstructed = (double)magnitude +
                ldexp(0.5, (int)reconstruction_shift);
            coefficient = negative ? -reconstructed : reconstructed;
        }
        if (!isfinite(coefficient)) return QW_STATUS_ERR_FORMAT;
        memcpy(plane_bytes + i * sizeof(coefficient), &coefficient,
               sizeof(coefficient));
    }

    width = (size_t)context->width;
    height = (size_t)context->height;
    {
        size_t low_width = qw_jp2_internal_resolution_extent_at_origin(
            context->component_x_origin, context->width,
            context->decomposition_levels);
        size_t low_height = qw_jp2_internal_resolution_extent_at_origin(
            context->component_y_origin, context->height,
            context->decomposition_levels);
        rc = qw_jp2_internal_dequantize_97_band(
            context, coefficients, 0u, 0u, low_width, low_height, 0u, 0u,
            context->source_decomposition_levels);
        if (rc != QW_STATUS_OK) return rc;
    }
    for (unsigned resolution = 1u;
         resolution <= context->decomposition_levels; ++resolution) {
        unsigned downshift =
            (unsigned)context->decomposition_levels - resolution;
        size_t level_width = qw_jp2_internal_resolution_extent_at_origin(
            context->component_x_origin, context->width, downshift);
        size_t level_height = qw_jp2_internal_resolution_extent_at_origin(
            context->component_y_origin, context->height, downshift);
        size_t low_width = qw_jp2_internal_resolution_extent_at_origin(
            context->component_x_origin, context->width, downshift + 1u);
        size_t low_height = qw_jp2_internal_resolution_extent_at_origin(
            context->component_y_origin, context->height, downshift + 1u);
        size_t subband_index = 1u + 3u * ((size_t)resolution - 1u);
        unsigned band_decomposition_level =
            (unsigned)context->source_decomposition_levels - resolution + 1u;

        rc = qw_jp2_internal_dequantize_97_band(
            context, coefficients, low_width, 0u, level_width - low_width,
            low_height, subband_index, 1u, band_decomposition_level);
        if (rc != QW_STATUS_OK) return rc;
        rc = qw_jp2_internal_dequantize_97_band(
            context, coefficients, 0u, low_height, low_width,
            level_height - low_height, subband_index + 1u, 1u,
            band_decomposition_level);
        if (rc != QW_STATUS_OK) return rc;
        rc = qw_jp2_internal_dequantize_97_band(
            context, coefficients, low_width, low_height,
            level_width - low_width, level_height - low_height,
            subband_index + 2u, 2u, band_decomposition_level);
        if (rc != QW_STATUS_OK) return rc;
    }
    return QW_STATUS_OK;
}

static int qw_jp2_internal_decode_finish_component(
    QwJp2InternalDecodeContext *context)
{
    size_t width;

    if (!context || !context->initialized || !context->coefficient_plane ||
        !context->coverage || !context->scratch ||
        context->covered_count != context->pixel_count ||
        context->mosaic_output_mode) {
        return QW_STATUS_ERR_FORMAT;
    }
    width = (size_t)context->width;
    if (context->wavelet_transform == 1u) {
        for (unsigned resolution = 1u;
             resolution <= context->decomposition_levels; ++resolution) {
            unsigned downshift =
                (unsigned)context->decomposition_levels - resolution;
            size_t level_width = qw_jp2_internal_resolution_extent_at_origin(
                context->component_x_origin, context->width, downshift);
            size_t level_height = qw_jp2_internal_resolution_extent_at_origin(
                context->component_y_origin, context->height, downshift);
            int dwt_status = qw_jp2_dwt53_inverse_level(
                (int64_t *)context->coefficient_plane, width,
                level_width, level_height,
                qw_jp2_internal_resolution_first_is_high(
                    context->component_x_origin, downshift),
                qw_jp2_internal_resolution_first_is_high(
                    context->component_y_origin, downshift),
                (int64_t *)context->scratch, context->scratch_capacity);
            if (dwt_status != QW_JP2_DWT_OK) {
                return QW_STATUS_ERR_FORMAT;
            }
        }
    } else if (context->wavelet_transform == 0u) {
        int rc = qw_jp2_internal_dequantize_97(context);
        if (rc != QW_STATUS_OK) return rc;
        for (unsigned resolution = 1u;
             resolution <= context->decomposition_levels; ++resolution) {
            unsigned downshift =
                (unsigned)context->decomposition_levels - resolution;
            size_t level_width = qw_jp2_internal_resolution_extent_at_origin(
                context->component_x_origin, context->width, downshift);
            size_t level_height = qw_jp2_internal_resolution_extent_at_origin(
                context->component_y_origin, context->height, downshift);
            int dwt_status = qw_jp2_dwt97_inverse_level(
                (double *)context->coefficient_plane, width,
                level_width, level_height,
                qw_jp2_internal_resolution_first_is_high(
                    context->component_x_origin, downshift),
                qw_jp2_internal_resolution_first_is_high(
                    context->component_y_origin, downshift),
                (double *)context->scratch, context->scratch_capacity);
            if (dwt_status != QW_JP2_DWT_OK) {
                return QW_STATUS_ERR_FORMAT;
            }
        }
    } else {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    return QW_STATUS_OK;
}

static int qw_jp2_internal_i64_add(int64_t left, int64_t right,
                                   int64_t *out_value)
{
    if (!out_value || (right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right)) {
        return 0;
    }
    *out_value = left + right;
    return 1;
}

static int qw_jp2_internal_i64_subtract(int64_t left, int64_t right,
                                        int64_t *out_value)
{
    if (!out_value || (right < 0 && left > INT64_MAX + right) ||
        (right > 0 && left < INT64_MIN + right)) {
        return 0;
    }
    *out_value = left - right;
    return 1;
}

static uint8_t qw_jp2_internal_signed_sample_to_u8(int64_t sample,
                                                   uint8_t precision)
{
    int64_t center;
    uint64_t maximum;
    uint64_t value;

    if (precision == 0u || precision > 38u) return 0u;
    if (precision == 8u) {
        if (sample <= -128) return 0u;
        if (sample >= 127) return 255u;
        return (uint8_t)(sample + 128);
    }
    center = (int64_t)(UINT64_C(1) << (precision - 1u));
    if (sample <= -center) return 0u;
    if (sample >= center - 1) return 255u;
    maximum = (UINT64_C(1) << precision) - 1u;
    value = (uint64_t)(sample + center);
    return (uint8_t)((value * 255u + maximum / 2u) / maximum);
}

static uint8_t qw_jp2_internal_unsigned_sample_to_u8(uint64_t sample,
                                                     uint8_t precision)
{
    uint64_t maximum;

    if (precision == 0u || precision > 38u) return 0u;
    maximum = (UINT64_C(1) << precision) - 1u;
    if (sample > maximum) sample = maximum;
    return (uint8_t)((sample * 255u + maximum / 2u) / maximum);
}

static uint8_t qw_jp2_internal_float_sample_to_u8(double sample,
                                                  uint8_t precision)
{
    double center;
    double maximum;

    if (precision == 0u || precision > 32u) return 0u;
    if (precision == 8u) {
        sample += 128.0;
        if (sample <= 0.0) return 0u;
        if (sample >= 255.0) return 255u;
        return (uint8_t)(sample + 0.5);
    }
    center = ldexp(1.0, (int)precision - 1);
    if (sample <= -center) return 0u;
    if (sample >= center - 1.0) return 255u;
    maximum = ldexp(1.0, (int)precision) - 1.0;
    sample = (sample + center) * 255.0 / maximum;
    return (uint8_t)(sample + 0.5);
}

static int qw_jp2_internal_sycc_to_rgb8(double y_centered,
                                        double cb_centered,
                                        double cr_centered,
                                        uint8_t *out_red,
                                        uint8_t *out_green,
                                        uint8_t *out_blue)
{
    double cb_term;
    double cr_term;
    int y;
    int red_term;
    int green_term;
    int blue_term;
    int64_t red;
    int64_t green;
    int64_t blue;

    if (!out_red || !out_green || !out_blue ||
        !isfinite(y_centered) || !isfinite(cb_centered) ||
        !isfinite(cr_centered)) {
        return 0;
    }
    if (y_centered + 128.0 <= (double)INT_MIN) {
        y = INT_MIN;
    } else if (y_centered + 128.0 >= (double)INT_MAX) {
        y = INT_MAX;
    } else {
        y = (int)(y_centered + 128.0);
    }
    cb_term = (double)(float)cb_centered;
    cr_term = (double)(float)cr_centered;
    if (!isfinite(cb_term) || !isfinite(cr_term)) return 0;
    cb_term *= 0.344;
    cr_term *= 0.714;
    if (cr_centered * 1.402 >= (double)INT_MAX) {
        red_term = INT_MAX;
    } else if (cr_centered * 1.402 <= (double)INT_MIN) {
        red_term = INT_MIN;
    } else {
        red_term = (int)(1.402 * (double)(float)cr_centered);
    }
    if (cb_term + cr_term >= (double)INT_MAX) {
        green_term = INT_MAX;
    } else if (cb_term + cr_term <= (double)INT_MIN) {
        green_term = INT_MIN;
    } else {
        green_term = (int)(cb_term + cr_term);
    }
    if (cb_centered * 1.772 >= (double)INT_MAX) {
        blue_term = INT_MAX;
    } else if (cb_centered * 1.772 <= (double)INT_MIN) {
        blue_term = INT_MIN;
    } else {
        blue_term = (int)(1.772 * (double)(float)cb_centered);
    }

    red = (int64_t)y + (int64_t)red_term;
    green = (int64_t)y - (int64_t)green_term;
    blue = (int64_t)y + (int64_t)blue_term;
    *out_red = red <= 0 ? 0u : red >= 255 ? 255u : (uint8_t)red;
    *out_green = green <= 0 ? 0u : green >= 255 ? 255u : (uint8_t)green;
    *out_blue = blue <= 0 ? 0u : blue >= 255 ? 255u : (uint8_t)blue;
    return 1;
}

static uint8_t qw_jp2_internal_composite_alpha(uint8_t color,
                                               uint8_t alpha,
                                               uint8_t alpha_channel_type)
{
    if (alpha_channel_type == 1u) {
        return (uint8_t)(((uint32_t)color * alpha +
                          255u * (255u - alpha) + 127u) / 255u);
    }
    if (alpha_channel_type == 2u) {
        unsigned composite = (unsigned)color + (255u - alpha);
        return (uint8_t)(composite > 255u ? 255u : composite);
    }
    return color;
}

static int qw_jp2_internal_decode_finish_rgb(
    QwJp2InternalDecodeContext *context)
{
    const QwJp2InternalDecodeContext *components[3];
    const QwJp2InternalDecodeContext *alpha_component = NULL;
    size_t component_count;
    size_t output_size;
    uint8_t *pixels;

    if (context && context->rgb_mosaic_output_mode) {
        if (!context->initialized || !context->component_contexts ||
            (context->component_context_count != 3u &&
             context->component_context_count != 4u) ||
            context->output_channels != 3u || !context->pixels ||
            !context->coverage ||
            context->covered_count != context->pixel_count) {
            return QW_STATUS_ERR_FORMAT;
        }
        return QW_STATUS_OK;
    }
    if (!context || (context->component_context_count != 3u &&
                     context->component_context_count != 4u) ||
        !context->component_contexts || context->output_channels != 3u ||
        (context->component_context_count == 3u &&
         context->alpha_channel_type != 0u) ||
        (context->component_context_count == 4u &&
         context->alpha_channel_type != 1u &&
         context->alpha_channel_type != 2u) ||
        !qw_checked_size_mul(context->pixel_count, 3u, &output_size)) {
        return QW_STATUS_ERR_FORMAT;
    }
    component_count = context->component_context_count;
    if (context->sycc_color_space &&
        (component_count != 3u || context->alpha_channel_type != 0u ||
         context->multiple_component_transform != 0u ||
         context->rgb_component_indices[0] != 0u ||
         context->rgb_component_indices[1] != 1u ||
         context->rgb_component_indices[2] != 2u)) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    for (size_t component = 0u; component < component_count; ++component) {
        const QwJp2InternalDecodeContext *component_context =
            &context->component_contexts[component];
        uint8_t x_sampling = context->component_x_sampling[component];
        uint8_t y_sampling = context->component_y_sampling[component];
        uint32_t expected_width;
        uint32_t expected_height;
        if (!x_sampling || !y_sampling) return QW_STATUS_ERR_FORMAT;
        expected_width = context->width / x_sampling +
            (context->width % x_sampling != 0u ? 1u : 0u);
        expected_height = context->height / y_sampling +
            (context->height % y_sampling != 0u ? 1u : 0u);
        if (component_context->width != expected_width ||
            component_context->height != expected_height) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        int rc = qw_jp2_internal_decode_finish_component(
            context->component_contexts + component);
        if (rc != QW_STATUS_OK) return rc;
    }
    {
        uint8_t seen_components = 0u;
        for (size_t channel = 0u; channel < 3u; ++channel) {
            uint8_t component_index = context->rgb_component_indices[channel];
            uint8_t component_bit;
            if (component_index >= component_count) return QW_STATUS_ERR_FORMAT;
            component_bit = (uint8_t)(1u << component_index);
            if (seen_components & component_bit) return QW_STATUS_ERR_FORMAT;
            seen_components |= component_bit;
            components[channel] =
                &context->component_contexts[component_index];
        }
        if (context->alpha_channel_type != 0u) {
            uint8_t component_index = context->alpha_component_index;
            uint8_t component_bit;
            if (component_index >= component_count) return QW_STATUS_ERR_FORMAT;
            component_bit = (uint8_t)(1u << component_index);
            if (seen_components & component_bit) return QW_STATUS_ERR_FORMAT;
            seen_components |= component_bit;
            alpha_component = &context->component_contexts[component_index];
        }
        if ((context->alpha_channel_type != 0u) !=
                (alpha_component != NULL) ||
            seen_components != (uint8_t)((1u << component_count) - 1u)) {
            return QW_STATUS_ERR_FORMAT;
        }
    }
    if (context->multiple_component_transform == 1u) {
        for (size_t channel = 0u; channel < 3u; ++channel) {
            uint8_t component_index = context->rgb_component_indices[channel];
            if (context->component_x_sampling[component_index] != 1u ||
                context->component_y_sampling[component_index] != 1u) {
                return QW_STATUS_ERR_UNSUPPORTED;
            }
        }
    }
    pixels = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!pixels) return QW_STATUS_ERR_OUT_OF_MEMORY;

    if (context->wavelet_transform == 1u) {
        const int64_t *first =
            (const int64_t *)components[0]->coefficient_plane;
        const int64_t *second =
            (const int64_t *)components[1]->coefficient_plane;
        const int64_t *third =
            (const int64_t *)components[2]->coefficient_plane;
        const int64_t *alpha = alpha_component
            ? (const int64_t *)alpha_component->coefficient_plane : NULL;
        for (size_t i = 0u; i < context->pixel_count; ++i) {
            size_t output_x = i % context->width;
            size_t output_y = i / context->width;
            uint8_t first_component = context->rgb_component_indices[0];
            uint8_t second_component = context->rgb_component_indices[1];
            uint8_t third_component = context->rgb_component_indices[2];
            size_t first_index =
                (output_y / context->component_y_sampling[first_component]) *
                    components[0]->width +
                output_x / context->component_x_sampling[first_component];
            size_t second_index =
                (output_y / context->component_y_sampling[second_component]) *
                    components[1]->width +
                output_x / context->component_x_sampling[second_component];
            size_t third_index =
                (output_y / context->component_y_sampling[third_component]) *
                    components[2]->width +
                output_x / context->component_x_sampling[third_component];
            size_t alpha_index = alpha
                ? (output_y /
                       context->component_y_sampling[
                           context->alpha_component_index]) *
                      alpha_component->width +
                  output_x /
                      context->component_x_sampling[
                          context->alpha_component_index]
                : 0u;
            int64_t red = first[first_index];
            int64_t green = second[second_index];
            int64_t blue = third[third_index];
            uint8_t red8;
            uint8_t green8;
            uint8_t blue8;
            uint8_t alpha8 = alpha
                ? qw_jp2_internal_signed_sample_to_u8(
                      alpha[alpha_index], alpha_component->precision) : 255u;
            if (context->multiple_component_transform == 1u) {
                int64_t chroma_sum;
                int64_t correction;
                if (!qw_jp2_internal_i64_add(second[second_index],
                                             third[third_index],
                                             &chroma_sum)) {
                    free(pixels);
                    return QW_STATUS_ERR_FORMAT;
                }
                correction = chroma_sum / 4;
                if (chroma_sum % 4 < 0) --correction;
                if (!qw_jp2_internal_i64_subtract(first[first_index],
                                                  correction,
                                                  &green) ||
                    !qw_jp2_internal_i64_add(third[third_index], green,
                                             &red) ||
                    !qw_jp2_internal_i64_add(second[second_index], green,
                                             &blue)) {
                    free(pixels);
                    return QW_STATUS_ERR_FORMAT;
                }
            }
            red8 = qw_jp2_internal_signed_sample_to_u8(
                red, components[0]->precision);
            if (context->sycc_color_space) {
                if (!qw_jp2_internal_sycc_to_rgb8(
                        (double)first[first_index],
                        (double)second[second_index],
                        (double)third[third_index],
                        &red8, &green8, &blue8)) {
                    free(pixels);
                    return QW_STATUS_ERR_FORMAT;
                }
            } else {
                green8 = qw_jp2_internal_signed_sample_to_u8(
                    green, components[1]->precision);
                blue8 = qw_jp2_internal_signed_sample_to_u8(
                    blue, components[2]->precision);
            }
            pixels[3u * i] = qw_jp2_internal_composite_alpha(
                red8, alpha8, context->alpha_channel_type);
            pixels[3u * i + 1u] = qw_jp2_internal_composite_alpha(
                green8, alpha8, context->alpha_channel_type);
            pixels[3u * i + 2u] = qw_jp2_internal_composite_alpha(
                blue8, alpha8, context->alpha_channel_type);
        }
    } else if (context->wavelet_transform == 0u) {
        const double *first =
            (const double *)components[0]->coefficient_plane;
        const double *second =
            (const double *)components[1]->coefficient_plane;
        const double *third =
            (const double *)components[2]->coefficient_plane;
        const double *alpha = alpha_component
            ? (const double *)alpha_component->coefficient_plane : NULL;
        for (size_t i = 0u; i < context->pixel_count; ++i) {
            size_t output_x = i % context->width;
            size_t output_y = i / context->width;
            uint8_t first_component = context->rgb_component_indices[0];
            uint8_t second_component = context->rgb_component_indices[1];
            uint8_t third_component = context->rgb_component_indices[2];
            size_t first_index =
                (output_y / context->component_y_sampling[first_component]) *
                    components[0]->width +
                output_x / context->component_x_sampling[first_component];
            size_t second_index =
                (output_y / context->component_y_sampling[second_component]) *
                    components[1]->width +
                output_x / context->component_x_sampling[second_component];
            size_t third_index =
                (output_y / context->component_y_sampling[third_component]) *
                    components[2]->width +
                output_x / context->component_x_sampling[third_component];
            size_t alpha_index = alpha
                ? (output_y /
                       context->component_y_sampling[
                           context->alpha_component_index]) *
                      alpha_component->width +
                  output_x /
                      context->component_x_sampling[
                          context->alpha_component_index]
                : 0u;
            double red = first[first_index];
            double green = second[second_index];
            double blue = third[third_index];
            uint8_t red8;
            uint8_t green8;
            uint8_t blue8;
            uint8_t alpha8;
            if (context->multiple_component_transform == 1u) {
                red = first[first_index] + 1.402 * third[third_index];
                green = first[first_index] - 0.34413 * second[second_index] -
                        0.71414 * third[third_index];
                blue = first[first_index] + 1.772 * second[second_index];
            }
            if (!isfinite(red) || !isfinite(green) || !isfinite(blue) ||
                (alpha && !isfinite(alpha[alpha_index]))) {
                free(pixels);
                return QW_STATUS_ERR_FORMAT;
            }
            if (context->sycc_color_space) {
                if (!qw_jp2_internal_sycc_to_rgb8(
                        first[first_index], second[second_index],
                        third[third_index], &red8, &green8, &blue8)) {
                    free(pixels);
                    return QW_STATUS_ERR_FORMAT;
                }
            } else {
                red8 = qw_jp2_internal_float_sample_to_u8(
                    red, components[0]->precision);
                green8 = qw_jp2_internal_float_sample_to_u8(
                    green, components[1]->precision);
                blue8 = qw_jp2_internal_float_sample_to_u8(
                    blue, components[2]->precision);
            }
            alpha8 = alpha
                ? qw_jp2_internal_float_sample_to_u8(
                      alpha[alpha_index], alpha_component->precision) : 255u;
            pixels[3u * i] = qw_jp2_internal_composite_alpha(
                red8, alpha8, context->alpha_channel_type);
            pixels[3u * i + 1u] = qw_jp2_internal_composite_alpha(
                green8, alpha8, context->alpha_channel_type);
            pixels[3u * i + 2u] = qw_jp2_internal_composite_alpha(
                blue8, alpha8, context->alpha_channel_type);
        }
    } else {
        free(pixels);
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    context->pixels = pixels;
    return QW_STATUS_OK;
}

static int qw_jp2_internal_decode_finish(
    QwJp2InternalDecodeContext *context)
{
    uint8_t *pixels;
    size_t output_bytes;
    uint8_t output_channels;
    int rc;

    if (!context || !context->initialized) return QW_STATUS_ERR_FORMAT;
    if (context->component_context_count != 0u) {
        return qw_jp2_internal_decode_finish_rgb(context);
    }
    if (!context->coverage) return QW_STATUS_ERR_FORMAT;
    if (context->mosaic_output_mode) {
        return context->pixels && context->covered_count == context->pixel_count
            ? QW_STATUS_OK : QW_STATUS_ERR_FORMAT;
    }
    rc = qw_jp2_internal_decode_finish_component(context);
    if (rc != QW_STATUS_OK) return rc;

    output_channels = context->output_channels == 3u ? 3u : 1u;
    if (!qw_checked_size_mul(context->pixel_count,
                             (size_t)output_channels, &output_bytes)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    pixels = (uint8_t *)malloc(output_bytes ? output_bytes : 1u);
    if (!pixels) return QW_STATUS_ERR_OUT_OF_MEMORY;
    if (context->wavelet_transform == 1u) {
        const int64_t *coefficients =
            (const int64_t *)context->coefficient_plane;
        for (size_t i = 0u; i < context->pixel_count; ++i) {
            pixels[i] = qw_jp2_internal_signed_sample_to_u8(
                coefficients[i], context->precision);
        }
    } else {
        const double *coefficients =
            (const double *)context->coefficient_plane;
        for (size_t i = 0u; i < context->pixel_count; ++i) {
            if (!isfinite(coefficients[i])) {
                free(pixels);
                return QW_STATUS_ERR_FORMAT;
            }
            pixels[i] = qw_jp2_internal_float_sample_to_u8(
                coefficients[i], context->precision);
        }
    }
    context->output_channels = output_channels;
    context->pixels = pixels;
    return QW_STATUS_OK;
}

static uint32_t qw_jp2_internal_component_coordinate(
    uint32_t reference_coordinate,
    uint8_t sampling)
{
    if (!sampling) return 0u;
    return reference_coordinate / sampling +
        (reference_coordinate % sampling != 0u ? 1u : 0u);
}

static int qw_jp2_internal_mosaic_copy_tile(
    QwJp2InternalDecodeContext *mosaic,
    const QwJp2InternalDecodeContext *tile,
    size_t destination_x,
    size_t destination_y)
{
    size_t channels;

    if (!mosaic || !tile || !mosaic->initialized ||
        (!mosaic->mosaic_output_mode && !mosaic->rgb_mosaic_output_mode) ||
        (mosaic->mosaic_output_mode && mosaic->rgb_mosaic_output_mode) ||
        !mosaic->pixels ||
        !mosaic->coverage || !tile->pixels || !tile->initialized ||
        (mosaic->output_channels != 1u && mosaic->output_channels != 3u) ||
        tile->output_channels != mosaic->output_channels ||
        destination_x > (size_t)mosaic->width ||
        destination_y > (size_t)mosaic->height ||
        (size_t)tile->width > (size_t)mosaic->width - destination_x ||
        (size_t)tile->height > (size_t)mosaic->height - destination_y) {
        return QW_STATUS_ERR_FORMAT;
    }
    channels = (size_t)mosaic->output_channels;
    for (size_t y = 0u; y < (size_t)tile->height; ++y) {
        size_t destination_row = (destination_y + y) * (size_t)mosaic->width;
        size_t source_row = y * (size_t)tile->width;
        for (size_t x = 0u; x < (size_t)tile->width; ++x) {
            size_t destination_index = destination_row + destination_x + x;
            size_t coverage_byte = destination_index >> 3u;
            uint8_t coverage_mask = (uint8_t)(1u << (destination_index & 7u));
            if (destination_index >= mosaic->pixel_count ||
                (mosaic->coverage[coverage_byte] & coverage_mask) != 0u) {
                return QW_STATUS_ERR_FORMAT;
            }
            memcpy(mosaic->pixels + destination_index * channels,
                   tile->pixels + (source_row + x) * channels, channels);
            mosaic->coverage[coverage_byte] |= coverage_mask;
            ++mosaic->covered_count;
        }
    }
    return QW_STATUS_OK;
}

static int qw_jp2_internal_decode_tile_into_mosaic(
    QwJp2TileCodingState *tile_state,
    QwJp2Tier1Coefficient *tier1_coefficients,
    size_t tier1_coefficient_capacity,
    QwJp2InternalDecodeContext *mosaic)
{
    const QwJp2PacketTileGeometry *geometry;
    const QwJp2PacketComponentGeometry *component;
    uint64_t tiles_x;
    uint64_t tiles_y;
    uint64_t tile_start_x;
    uint64_t tile_start_y;
    uint64_t tile_end_x;
    uint64_t tile_end_y;
    uint64_t clipped_start_x;
    uint64_t clipped_start_y;
    uint64_t clipped_end_x;
    uint64_t clipped_end_y;
    uint32_t component_image_start_x;
    uint32_t component_image_start_y;
    uint32_t component_start_x;
    uint32_t component_start_y;
    uint32_t component_end_x;
    uint32_t component_end_y;
    size_t coverage_bytes;
    size_t reserved_bytes;
    QwJp2InternalDecodeContext tile_decode = {0};
    int rc;

    if (!tile_state || !tile_state->packet_state.initialized ||
        !tier1_coefficients || !mosaic || !mosaic->mosaic_output_mode) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    geometry = &tile_state->packet_state.tile_geometry;
    component = &tile_state->packet_state.component_geometry[0];
    if (component->x_sampling == 0u || component->y_sampling == 0u ||
        component->decomposition_levels !=
            mosaic->source_decomposition_levels ||
        geometry->x_size <= geometry->x_origin ||
        geometry->y_size <= geometry->y_origin ||
        geometry->tile_x_origin > geometry->x_origin ||
        geometry->tile_y_origin > geometry->y_origin ||
        geometry->tile_width == 0u || geometry->tile_height == 0u ||
        component->decomposition_levels >= 63u) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    tiles_x = ((uint64_t)geometry->x_size - geometry->tile_x_origin +
               geometry->tile_width - 1u) / geometry->tile_width;
    tiles_y = ((uint64_t)geometry->y_size - geometry->tile_y_origin +
               geometry->tile_height - 1u) / geometry->tile_height;
    if (tiles_x == 0u || tiles_y == 0u ||
        (uint64_t)geometry->tile_index >= tiles_x * tiles_y) {
        return QW_STATUS_ERR_FORMAT;
    }
    tile_start_x = (uint64_t)geometry->tile_x_origin +
        ((uint64_t)geometry->tile_index % tiles_x) * geometry->tile_width;
    tile_start_y = (uint64_t)geometry->tile_y_origin +
        ((uint64_t)geometry->tile_index / tiles_x) * geometry->tile_height;
    tile_end_x = tile_start_x + geometry->tile_width;
    tile_end_y = tile_start_y + geometry->tile_height;
    clipped_start_x = tile_start_x > geometry->x_origin
        ? tile_start_x : geometry->x_origin;
    clipped_start_y = tile_start_y > geometry->y_origin
        ? tile_start_y : geometry->y_origin;
    clipped_end_x = tile_end_x < geometry->x_size
        ? tile_end_x : geometry->x_size;
    clipped_end_y = tile_end_y < geometry->y_size
        ? tile_end_y : geometry->y_size;
    if (clipped_end_x <= clipped_start_x ||
        clipped_end_y <= clipped_start_y) {
        return QW_STATUS_OK;
    }
    if (clipped_end_x - clipped_start_x > UINT32_MAX ||
        clipped_end_y - clipped_start_y > UINT32_MAX ||
        tile_start_x > UINT32_MAX || tile_start_y > UINT32_MAX ||
        clipped_start_x > UINT32_MAX || clipped_start_y > UINT32_MAX ||
        mosaic->pixel_count > SIZE_MAX - 7u) {
        return QW_STATUS_ERR_FORMAT;
    }
    component_image_start_x = qw_jp2_internal_component_coordinate(
        geometry->x_origin, component->x_sampling);
    component_image_start_y = qw_jp2_internal_component_coordinate(
        geometry->y_origin, component->y_sampling);
    component_start_x = qw_jp2_internal_component_coordinate(
        (uint32_t)clipped_start_x, component->x_sampling);
    component_start_y = qw_jp2_internal_component_coordinate(
        (uint32_t)clipped_start_y, component->y_sampling);
    component_end_x = qw_jp2_internal_component_coordinate(
        (uint32_t)clipped_end_x, component->x_sampling);
    component_end_y = qw_jp2_internal_component_coordinate(
        (uint32_t)clipped_end_y, component->y_sampling);
    if (component_end_x <= component_start_x ||
        component_end_y <= component_start_y) {
        return QW_STATUS_OK;
    }
    coverage_bytes = (mosaic->pixel_count + 7u) / 8u;
    if (mosaic->pixel_count > SIZE_MAX - coverage_bytes) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    reserved_bytes = mosaic->pixel_count + coverage_bytes;
    rc = qw_jp2_internal_decode_context_init(
        &tile_decode, component_end_x - component_start_x,
        component_end_y - component_start_y,
        component->decomposition_levels, reserved_bytes);
    if (rc != QW_STATUS_OK) goto cleanup;
    tile_decode.source_decomposition_levels =
        mosaic->source_decomposition_levels;
    tile_decode.component_x_origin = component_start_x;
    tile_decode.component_y_origin = component_start_y;
    tile_decode.roi_shift = tile_state->component_roi_shifts[0];
    tile_decode.wavelet_transform = component->wavelet_transform;
    tile_decode.precision = mosaic->precision;
    if (!tile_state->internal_quantization.present ||
        !qw_jp2_internal_quantization_supported(
            tile_decode.wavelet_transform,
            &tile_state->internal_quantization)) {
        rc = QW_STATUS_ERR_UNSUPPORTED;
        goto cleanup;
    }
    qw_jp2_internal_decode_set_quantization(
        &tile_decode, &tile_state->internal_quantization);
    rc = qw_jp2_tile_packet_state_decode_bodies(
        &tile_state->packet_state, tier1_coefficients,
        tier1_coefficient_capacity, &tile_decode);
    if (rc != QW_STATUS_OK) goto cleanup;
    rc = qw_jp2_internal_decode_finish(&tile_decode);
    if (rc != QW_STATUS_OK) goto cleanup;
    rc = qw_jp2_internal_mosaic_copy_tile(
        mosaic, &tile_decode,
        (size_t)(component_start_x - component_image_start_x),
        (size_t)(component_start_y - component_image_start_y));

cleanup:
    qw_jp2_internal_decode_context_clear(&tile_decode);
    return rc;
}

static int qw_jp2_internal_decode_tile_into_rgb_mosaic(
    QwJp2TileCodingState *tile_state,
    QwJp2Tier1Coefficient *tier1_coefficients,
    size_t tier1_coefficient_capacity,
    QwJp2InternalDecodeContext *mosaic)
{
    const QwJp2PacketTileGeometry *geometry;
    uint64_t tiles_x;
    uint64_t tiles_y;
    uint64_t tile_count;
    uint64_t tile_start_x;
    uint64_t tile_start_y;
    uint64_t tile_end_x;
    uint64_t tile_end_y;
    uint64_t clipped_start_x;
    uint64_t clipped_start_y;
    uint64_t clipped_end_x;
    uint64_t clipped_end_y;
    size_t output_bytes;
    size_t coverage_bytes;
    size_t component_array_bytes;
    size_t reserved_bytes;
    uint8_t decomposition_levels;
    uint8_t wavelet_transform;
    uint16_t component_count;
    uint8_t precisions[4] = {0u};
    uint8_t roi_shifts[4] = {0u};
    QwJp2QuantizationParameters quantizations[4] = {{0}};
    QwJp2InternalDecodeContext tile_decode = {0};
    int rc;

    if (!tile_state || !tile_state->packet_state.initialized ||
        !tier1_coefficients || !mosaic ||
        !mosaic->rgb_mosaic_output_mode) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    geometry = &tile_state->packet_state.tile_geometry;
    component_count = mosaic->component_context_count;
    decomposition_levels =
        tile_state->packet_state.component_geometry[0].decomposition_levels;
    wavelet_transform =
        tile_state->packet_state.component_geometry[0].wavelet_transform;
    if (!mosaic->component_contexts ||
        (component_count != 3u && component_count != 4u) ||
        geometry->x_size <= geometry->x_origin ||
        geometry->y_size <= geometry->y_origin ||
        geometry->tile_x_origin > geometry->x_origin ||
        geometry->tile_y_origin > geometry->y_origin ||
        !geometry->tile_width || !geometry->tile_height ||
        decomposition_levels >= 63u || wavelet_transform > 1u) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    tiles_x = ((uint64_t)geometry->x_size - geometry->tile_x_origin +
               geometry->tile_width - 1u) / geometry->tile_width;
    tiles_y = ((uint64_t)geometry->y_size - geometry->tile_y_origin +
               geometry->tile_height - 1u) / geometry->tile_height;
    if (!tiles_x || !tiles_y || tiles_x > UINT64_MAX / tiles_y) {
        return QW_STATUS_ERR_FORMAT;
    }
    tile_count = tiles_x * tiles_y;
    if ((uint64_t)geometry->tile_index >= tile_count) {
        return QW_STATUS_ERR_FORMAT;
    }
    tile_start_x = (uint64_t)geometry->tile_x_origin +
        ((uint64_t)geometry->tile_index % tiles_x) * geometry->tile_width;
    tile_start_y = (uint64_t)geometry->tile_y_origin +
        ((uint64_t)geometry->tile_index / tiles_x) * geometry->tile_height;
    tile_end_x = tile_start_x + geometry->tile_width;
    tile_end_y = tile_start_y + geometry->tile_height;
    clipped_start_x = tile_start_x > geometry->x_origin
        ? tile_start_x : geometry->x_origin;
    clipped_start_y = tile_start_y > geometry->y_origin
        ? tile_start_y : geometry->y_origin;
    clipped_end_x = tile_end_x < geometry->x_size
        ? tile_end_x : geometry->x_size;
    clipped_end_y = tile_end_y < geometry->y_size
        ? tile_end_y : geometry->y_size;
    if (clipped_end_x <= clipped_start_x ||
        clipped_end_y <= clipped_start_y) {
        return QW_STATUS_OK;
    }
    if (clipped_start_x > UINT32_MAX || clipped_start_y > UINT32_MAX ||
        clipped_end_x > UINT32_MAX || clipped_end_y > UINT32_MAX ||
        clipped_end_x - clipped_start_x > UINT32_MAX ||
        clipped_end_y - clipped_start_y > UINT32_MAX ||
        mosaic->pixel_count > SIZE_MAX - 7u ||
        !qw_checked_size_mul(mosaic->pixel_count, 3u, &output_bytes)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    for (uint16_t component = 0u; component < component_count; ++component) {
        const QwJp2PacketComponentGeometry *geometry_component =
            &tile_state->packet_state.component_geometry[component];
        if (geometry_component->x_sampling != 1u ||
            geometry_component->y_sampling != 1u ||
            geometry_component->decomposition_levels !=
                decomposition_levels ||
            geometry_component->wavelet_transform != wavelet_transform ||
            !mosaic->component_contexts[component].precision ||
            !tile_state->internal_quantizations[component].present ||
            tile_state->component_roi_shifts[component] >= 64u) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        precisions[component] =
            mosaic->component_contexts[component].precision;
        roi_shifts[component] = tile_state->component_roi_shifts[component];
        quantizations[component] = tile_state->internal_quantizations[component];
    }
    coverage_bytes = (mosaic->pixel_count + 7u) / 8u;
    if ((size_t)component_count >
        SIZE_MAX / sizeof(*mosaic->component_contexts)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    component_array_bytes = (size_t)component_count *
                            sizeof(*mosaic->component_contexts);
    reserved_bytes = output_bytes;
    if (coverage_bytes > SIZE_MAX - reserved_bytes) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    reserved_bytes += coverage_bytes;
    if (component_array_bytes > SIZE_MAX - reserved_bytes) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    reserved_bytes += component_array_bytes;
    if (reserved_bytes > (size_t)QW_JP2_MAX_INTERNAL_DECODE_BYTES) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }

    rc = qw_jp2_internal_rgb_context_init(
        &tile_decode, (uint32_t)(clipped_end_x - clipped_start_x),
        (uint32_t)(clipped_end_y - clipped_start_y), decomposition_levels,
        NULL, wavelet_transform, mosaic->multiple_component_transform,
        component_count, mosaic->alpha_channel_type,
        mosaic->rgb_component_indices, mosaic->alpha_component_index,
        NULL, NULL,
        precisions, quantizations, roi_shifts, reserved_bytes, 0);
    if (rc != QW_STATUS_OK) goto cleanup;
    tile_decode.sycc_color_space = mosaic->sycc_color_space;
    tile_decode.use_per_coefficient_reconstruction = component_count == 3u;
    for (uint16_t component = 0u; component < component_count; ++component) {
        tile_decode.component_contexts[component].component_x_origin =
            (uint32_t)clipped_start_x;
        tile_decode.component_contexts[component].component_y_origin =
            (uint32_t)clipped_start_y;
    }
    rc = qw_jp2_tile_packet_state_decode_bodies(
        &tile_state->packet_state, tier1_coefficients,
        tier1_coefficient_capacity, &tile_decode);
    if (rc != QW_STATUS_OK) goto cleanup;
    rc = qw_jp2_internal_decode_finish(&tile_decode);
    if (rc != QW_STATUS_OK) goto cleanup;
    rc = qw_jp2_internal_mosaic_copy_tile(
        mosaic, &tile_decode,
        (size_t)(clipped_start_x - geometry->x_origin),
        (size_t)(clipped_start_y - geometry->y_origin));

cleanup:
    qw_jp2_internal_decode_context_clear(&tile_decode);
    return rc;
}

static int qw_jp2_validate_tile_parts_impl(
    const uint8_t *data,
    size_t size,
    size_t first_sot_offset,
    uint32_t tile_count,
    uint16_t component_count,
    uint16_t main_layer_count,
    const uint8_t *main_component_resolution_counts,
    const QwJp2PacketTileGeometry *siz_tile_geometry,
    const QwJp2PacketComponentGeometry *main_component_geometry,
    const QwJp2CodParameters *main_cod_parameters,
    const QwJp2QuantizationParameters *main_qcd_parameters,
    const QwJp2QuantizationParameters *main_qcc_parameters,
    const uint8_t *main_roi_shifts,
    const uint8_t *main_poc_payload,
    size_t main_poc_payload_size,
    const uint8_t *main_ppm_data,
    size_t main_ppm_size,
    size_t main_ppm_segment_count,
    QwJp2TileCodingState *tile_states,
    QwJp2InternalDecodeContext *internal_decode)
{
    size_t position = first_sot_offset;
    size_t tile_part_ordinal = 0u;
    size_t packet_budget_remaining =
        (size_t)QW_JP2_MAX_TRACKED_PACKET_KEYS;
    size_t codeblock_budget_remaining =
        (size_t)QW_JP2_MAX_TRACKED_PACKET_CODEBLOCKS;
    size_t body_budget_remaining =
        (size_t)QW_JP2_MAX_TRACKED_PACKET_BODY_BYTES;
    size_t segment_metadata_budget_remaining =
        (size_t)QW_JP2_MAX_TRACKED_PACKET_SEGMENT_METADATA_BYTES;
    size_t quantization_budget_remaining =
        (size_t)QW_JP2_MAX_TRACKED_QUANTIZATION_BYTES;

    for (;;) {
        size_t sot_start;
        size_t marker_offset;
        size_t tile_part_end = 0u;
        size_t header_limit = size;
        uint8_t marker;
        uint16_t marker_length;
        uint16_t tile_index;
        uint32_t tile_part_size;
        uint8_t tile_part_index;
        uint8_t tile_part_count;
        QwJp2TileCodingState *tile_state;
        QwJp2CodParameters tile_cod_parameters;
        QwJp2QuantizationMarker tile_qcd_marker = {0};
        QwJp2QuantizationMarker tile_qcc_markers[4] = {{0}};
        QwJp2QuantizationParameters tile_qcd_parameters = {0};
        QwJp2QuantizationParameters tile_qcc_parameters[4] = {{0}};
        QwJp2QuantizationParameters effective_quantizations[4] = {{0}};
        QwJp2PacketComponentGeometry effective_geometry[4] = {{0}};
        QwJp2PacketTileGeometry effective_tile_geometry = {0};
        QwJp2ComponentCodingParameters tile_coc_parameters[4] = {{0}};
        QwJp2PackedHeaderSegment tile_ppt_segments[
            QW_JP2_PACKED_HEADER_MAX_SEGMENTS] = {{0}};
        size_t tile_ppt_segment_count = 0u;
        size_t tile_ppt_payload_size = 0u;
        uint8_t tile_coc_local_mask = 0u;
        uint8_t tile_cod_local_seen = 0u;
        const uint8_t *tile_poc_payload = NULL;
        size_t tile_poc_payload_size = 0u;
        const uint8_t *packed_header_data = NULL;
        size_t packed_header_size = 0u;
        uint8_t *packed_tile_headers = NULL;
        int seen_poc = 0;
        int has_packed_headers = 0;

        if (qw_jp2_read_marker(data, size, size, &position, &marker_offset,
                               &marker) != QW_STATUS_OK) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (marker == QW_J2K_MARKER_EOC) {
            QwJp2Tier1Coefficient *tier1_coefficients;
            if (position != size) return QW_STATUS_ERR_FORMAT;
            for (uint32_t tile = 0u; tile < tile_count; ++tile) {
                const QwJp2TileCodingState *tile_state = &tile_states[tile];
                if (tile_state->initialized &&
                    (!tile_state->packet_state.initialized ||
                     tile_state->packet_state.next_packet_index !=
                         tile_state->packet_state.packet_count)) {
                    return QW_STATUS_ERR_FORMAT;
                }
            }
            if (main_ppm_segment_count != 0u) {
                int ppm_rc = qw_jp2_ppm_validate_tile_part_count(
                    main_ppm_data, main_ppm_size, tile_part_ordinal);
                if (ppm_rc == QW_JP2_PACKED_HEADER_ERR_RESOURCE) {
                    return QW_STATUS_ERR_RESOURCE_LIMIT;
                }
                if (ppm_rc != QW_JP2_PACKED_HEADER_OK) {
                    return QW_STATUS_ERR_FORMAT;
                }
            }
            tier1_coefficients = (QwJp2Tier1Coefficient *)malloc(
                (size_t)QW_JP2_MAX_CODEBLOCK_COEFFICIENTS *
                sizeof(*tier1_coefficients));
            if (!tier1_coefficients) return QW_STATUS_ERR_OUT_OF_MEMORY;
            for (uint32_t tile = 0u; tile < tile_count; ++tile) {
                QwJp2TileCodingState *tile_state = &tile_states[tile];
                if (!tile_state->initialized ||
                    !tile_state->packet_state.initialized) {
                    continue;
                }
                int tier1_status;
                if (internal_decode &&
                    !internal_decode->mosaic_output_mode &&
                    !internal_decode->rgb_mosaic_output_mode) {
                    for (uint16_t component = 0u;
                         component < component_count; ++component) {
                        QwJp2InternalDecodeContext *component_decode =
                            qw_jp2_internal_component_context(
                                internal_decode, component);
                        if (!component_decode) {
                            free(tier1_coefficients);
                            return QW_STATUS_ERR_UNSUPPORTED;
                        }
                        component_decode->roi_shift =
                            tile_state->component_roi_shifts[component];
                    }
                }
                if (internal_decode &&
                    internal_decode->rgb_mosaic_output_mode) {
                    tier1_status =
                        qw_jp2_internal_decode_tile_into_rgb_mosaic(
                            tile_state, tier1_coefficients,
                            (size_t)QW_JP2_MAX_CODEBLOCK_COEFFICIENTS,
                            internal_decode);
                } else if (internal_decode &&
                    internal_decode->mosaic_output_mode) {
                    tier1_status = qw_jp2_internal_decode_tile_into_mosaic(
                        tile_state, tier1_coefficients,
                        (size_t)QW_JP2_MAX_CODEBLOCK_COEFFICIENTS,
                        internal_decode);
                } else {
                    tier1_status = qw_jp2_tile_packet_state_decode_bodies(
                        &tile_state->packet_state, tier1_coefficients,
                        (size_t)QW_JP2_MAX_CODEBLOCK_COEFFICIENTS,
                        internal_decode);
                }
                if (tier1_status != QW_STATUS_OK) {
                    free(tier1_coefficients);
                    return tier1_status;
                }
            }
            free(tier1_coefficients);
            if (internal_decode) {
                int decode_status =
                    qw_jp2_internal_decode_finish(internal_decode);
                if (decode_status != QW_STATUS_OK) return decode_status;
            }
            return QW_STATUS_OK;
        }
        if (marker != QW_J2K_MARKER_SOT) return QW_STATUS_ERR_FORMAT;

        sot_start = marker_offset;
        if (size - position < 10u) return QW_STATUS_ERR_FORMAT;
        marker_length = qw_jp2_read_u16(data + position);
        if (marker_length != 10u) return QW_STATUS_ERR_FORMAT;
        tile_index = qw_jp2_read_u16(data + position + 2u);
        tile_part_size = qw_jp2_read_u32(data + position + 4u);
        tile_part_index = data[position + 8u];
        tile_part_count = data[position + 9u];
        if (tile_index >= tile_count ||
            (tile_part_count && tile_part_index >= tile_part_count)) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (!siz_tile_geometry || !main_component_geometry ||
            !main_roi_shifts ||
            !main_cod_parameters) {
            return QW_STATUS_ERR_FORMAT;
        }
        tile_cod_parameters = *main_cod_parameters;
        tile_state = &tile_states[tile_index];
        if (!tile_state->initialized) {
            tile_state->layer_count = main_layer_count;
            memcpy(tile_state->component_resolution_counts,
                   main_component_resolution_counts, component_count);
            memcpy(tile_state->component_roi_shifts, main_roi_shifts,
                   component_count);
            tile_state->initialized = 1u;
        }
        if (tile_part_index == 0u) {
            if (tile_state->first_tile_part_seen) {
                return QW_STATUS_ERR_FORMAT;
            }
            tile_state->first_tile_part_seen = 1u;
        } else if (!tile_state->first_tile_part_seen) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (tile_part_size) {
            if (tile_part_size < 14u ||
                (uint64_t)tile_part_size > (uint64_t)(size - sot_start)) {
                return QW_STATUS_ERR_FORMAT;
            }
            tile_part_end = sot_start + (size_t)tile_part_size;
            header_limit = tile_part_end;
        } else {
            if (size < 2u || data[size - 2u] != 0xffu ||
                data[size - 1u] != QW_J2K_MARKER_EOC) {
                return QW_STATUS_ERR_FORMAT;
            }
            tile_part_end = size - 2u;
            header_limit = tile_part_end;
        }
        position += (size_t)marker_length;

        for (;;) {
            size_t tile_header_marker_offset;
            if (qw_jp2_read_marker(data, size, header_limit, &position,
                                   &tile_header_marker_offset,
                                   &marker) != QW_STATUS_OK) {
                return QW_STATUS_ERR_FORMAT;
            }
            if (marker == QW_J2K_MARKER_SOD) break;
            if (marker == QW_J2K_MARKER_SOC ||
                marker == QW_J2K_MARKER_SOT ||
                marker == QW_J2K_MARKER_EOC || marker == 0x91u ||
                marker == 0x92u || marker == 0x01u ||
                (marker >= 0xd0u && marker <= 0xd7u)) {
                return QW_STATUS_ERR_FORMAT;
            }
            if (qw_jp2_marker_has_no_segment(marker)) continue;
            if (header_limit - position < 2u) return QW_STATUS_ERR_FORMAT;
            marker_length = qw_jp2_read_u16(data + position);
            if (marker_length < 2u ||
                (size_t)marker_length > header_limit - position) {
                return QW_STATUS_ERR_FORMAT;
            }
            const uint8_t *tile_marker_payload = data + position + 2u;
            size_t tile_marker_payload_size = (size_t)marker_length - 2u;
            if (marker == QW_J2K_MARKER_PPM) {
                return QW_STATUS_ERR_FORMAT;
            } else if (marker == QW_J2K_MARKER_RGN) {
                uint16_t component_index;
                uint8_t roi_shift;
                uint8_t component_mask;
                int rc = qw_jp2_parse_rgn_payload(
                    tile_marker_payload, tile_marker_payload_size,
                    component_count, &component_index, &roi_shift);
                if (rc != QW_STATUS_OK) return rc;
                if (tile_part_index != 0u) return QW_STATUS_ERR_FORMAT;
                component_mask = (uint8_t)(1u << component_index);
                if (tile_state->tile_rgn_mask & component_mask) {
                    return QW_STATUS_ERR_FORMAT;
                }
                tile_state->tile_rgn_mask |= component_mask;
                tile_state->component_roi_shifts[component_index] = roi_shift;
            } else if (marker == QW_J2K_MARKER_PPT) {
                size_t packed_payload_size;
                if (main_ppm_segment_count != 0u ||
                    tile_marker_payload_size < 2u) {
                    return QW_STATUS_ERR_FORMAT;
                }
                if (tile_ppt_segment_count >=
                    (size_t)QW_JP2_PACKED_HEADER_MAX_SEGMENTS) {
                    return QW_STATUS_ERR_RESOURCE_LIMIT;
                }
                if (tile_marker_payload[0] !=
                    (uint8_t)tile_ppt_segment_count) {
                    return QW_STATUS_ERR_FORMAT;
                }
                packed_payload_size = tile_marker_payload_size - 1u;
                if (packed_payload_size >
                    (size_t)QW_JP2_PACKED_HEADER_MAX_BYTES -
                        tile_ppt_payload_size) {
                    return QW_STATUS_ERR_RESOURCE_LIMIT;
                }
                tile_ppt_segments[tile_ppt_segment_count].sequence_index =
                    tile_marker_payload[0];
                tile_ppt_segments[tile_ppt_segment_count].payload =
                    tile_marker_payload + 1u;
                tile_ppt_segments[tile_ppt_segment_count].payload_size =
                    packed_payload_size;
                tile_ppt_payload_size += packed_payload_size;
                ++tile_ppt_segment_count;
            } else if (marker == QW_J2K_MARKER_QCD) {
                if (tile_part_index != 0u || tile_qcd_marker.present) {
                    return QW_STATUS_ERR_FORMAT;
                }
                tile_qcd_marker.payload = tile_marker_payload;
                tile_qcd_marker.payload_size = tile_marker_payload_size;
                tile_qcd_marker.present = 1u;
            } else if (marker == QW_J2K_MARKER_QCC) {
                size_t component_width = component_count < 257u ? 1u : 2u;
                uint16_t component_index;
                if (tile_part_index != 0u ||
                    tile_marker_payload_size < component_width + 1u) {
                    return QW_STATUS_ERR_FORMAT;
                }
                component_index = component_width == 1u
                    ? tile_marker_payload[0]
                    : qw_jp2_read_u16(tile_marker_payload);
                if (component_index >= component_count ||
                    tile_qcc_markers[component_index].present) {
                    return QW_STATUS_ERR_FORMAT;
                }
                tile_qcc_markers[component_index].payload =
                    tile_marker_payload;
                tile_qcc_markers[component_index].payload_size =
                    tile_marker_payload_size;
                tile_qcc_markers[component_index].present = 1u;
            } else if (marker == QW_J2K_MARKER_POC) {
                if (seen_poc) return QW_STATUS_ERR_FORMAT;
                tile_poc_payload = data + position + 2u;
                tile_poc_payload_size = (size_t)marker_length - 2u;
                seen_poc = 1;
            } else if (marker == QW_J2K_MARKER_COC) {
                uint16_t component_index;
                uint8_t component_mask;
                QwJp2ComponentCodingParameters component_coding = {0};
                int rc = qw_jp2_parse_coc_payload(
                    data + position + 2u,
                    (size_t)marker_length - 2u,
                    component_count, &component_index, &component_coding);
                if (rc != QW_STATUS_OK) return rc;
                if (tile_part_index != 0u) return QW_STATUS_ERR_FORMAT;
                component_mask = (uint8_t)(1u << component_index);
                if (tile_state->tile_coc_mask & component_mask) {
                    return QW_STATUS_ERR_FORMAT;
                }
                tile_state->tile_coc_mask |= component_mask;
                tile_coc_local_mask |= component_mask;
                tile_coc_parameters[component_index] = component_coding;
                tile_state->tile_coc_resolution_counts[component_index] =
                    (uint8_t)(component_coding.decomposition_levels + 1u);
                tile_state->component_resolution_counts[component_index] =
                    (uint8_t)(component_coding.decomposition_levels + 1u);
            } else if (marker == QW_J2K_MARKER_COD) {
                QwJp2CodParameters cod;
                int rc;
                if (tile_part_index != 0u || tile_state->tile_cod_seen) {
                    return QW_STATUS_ERR_FORMAT;
                }
                rc = qw_jp2_parse_cod_payload(
                    data + position + 2u,
                    (size_t)marker_length - 2u,
                    component_count, &cod);
                if (rc != QW_STATUS_OK) return rc;
                tile_cod_parameters = cod;
                tile_cod_local_seen = 1u;
                tile_state->tile_cod_seen = 1u;
                tile_state->layer_count = cod.layer_count;
                for (uint16_t component = 0u; component < component_count;
                     ++component) {
                    tile_state->component_resolution_counts[component] =
                        (uint8_t)(cod.decomposition_levels + 1u);
                    if (tile_state->tile_coc_mask &
                        (uint8_t)(1u << component)) {
                        tile_state->component_resolution_counts[component] =
                            tile_state->tile_coc_resolution_counts[component];
                    }
                }
            }
            position += (size_t)marker_length;
            (void)tile_header_marker_offset;
        }

        if (main_ppm_segment_count != 0u) {
            QwJp2PacketHeaderSpan packet_header_span = {NULL, 0u};
            int ppm_rc = qw_jp2_ppm_tile_part_header(
                main_ppm_data, main_ppm_size, tile_part_ordinal,
                &packet_header_span);
            if (ppm_rc == QW_JP2_PACKED_HEADER_ERR_RESOURCE) {
                return QW_STATUS_ERR_RESOURCE_LIMIT;
            }
            if (ppm_rc != QW_JP2_PACKED_HEADER_OK) {
                return QW_STATUS_ERR_FORMAT;
            }
            packed_header_data = packet_header_span.data;
            packed_header_size = packet_header_span.size;
            has_packed_headers = 1;
        }

        if (tile_part_index == 0u) {
            if (internal_decode) {
                for (uint16_t component = 0u;
                     component < component_count; ++component) {
                    if (tile_state->component_roi_shifts[component] >= 64u) {
                        return QW_STATUS_ERR_UNSUPPORTED;
                    }
                }
            }
            effective_tile_geometry = *siz_tile_geometry;
            memcpy(effective_geometry, main_component_geometry,
                   (size_t)component_count * sizeof(*effective_geometry));
            effective_tile_geometry.tile_index = tile_index;
            for (uint16_t component = 0u; component < component_count;
                 ++component) {
                /* T.800 A.6.1: tile COD overrides main COD/COC; tile COC wins. */
                if (tile_cod_local_seen) {
                    qw_jp2_apply_component_coding(
                        &effective_geometry[component],
                        &tile_cod_parameters.component);
                }
                if (tile_coc_local_mask & (uint8_t)(1u << component)) {
                    qw_jp2_apply_component_coding(
                        &effective_geometry[component],
                        &tile_coc_parameters[component]);
                }
            }
            {
                if (!main_qcd_parameters || !main_qcd_parameters->present ||
                    !main_qcc_parameters) {
                    return QW_STATUS_ERR_FORMAT;
                }
                if (tile_qcd_marker.present) {
                    int quantization_rc = qw_jp2_parse_quantization_marker(
                        tile_qcd_marker.payload,
                        tile_qcd_marker.payload_size, component_count,
                        0,
                        &tile_qcd_parameters, NULL);
                    if (quantization_rc != QW_STATUS_OK) {
                        return quantization_rc;
                    }
                }
                for (uint16_t component = 0u; component < component_count;
                     ++component) {
                    uint16_t parsed_component = UINT16_MAX;
                    const QwJp2QuantizationParameters *selected;
                    if (tile_qcc_markers[component].present) {
                        int quantization_rc = qw_jp2_parse_quantization_marker(
                            tile_qcc_markers[component].payload,
                            tile_qcc_markers[component].payload_size,
                            component_count, 1,
                            &tile_qcc_parameters[component],
                            &parsed_component);
                        if (quantization_rc != QW_STATUS_OK) {
                            return quantization_rc;
                        }
                        if (parsed_component != component) {
                            return QW_STATUS_ERR_FORMAT;
                        }
                    }
                    if (tile_qcc_parameters[component].present) {
                        selected = &tile_qcc_parameters[component];
                    } else if (tile_qcd_parameters.present) {
                        selected = &tile_qcd_parameters;
                    } else if (main_qcc_parameters[component].present) {
                        selected = &main_qcc_parameters[component];
                    } else {
                        selected = main_qcd_parameters;
                    }
                    effective_quantizations[component] = *selected;
                }
                if (internal_decode) {
                    for (uint16_t component = 0u;
                         component < component_count; ++component) {
                        QwJp2InternalDecodeContext *component_decode =
                            qw_jp2_internal_component_context(
                                internal_decode, component);
                        const QwJp2QuantizationParameters *selected =
                            &effective_quantizations[component];
                        if (!component_decode ||
                            component_decode->source_decomposition_levels !=
                                effective_geometry[component].decomposition_levels ||
                            component_decode->wavelet_transform !=
                                effective_geometry[component].wavelet_transform ||
                            !qw_jp2_internal_quantization_supported(
                                component_decode->wavelet_transform, selected)) {
                            return QW_STATUS_ERR_UNSUPPORTED;
                        }
                        if (!internal_decode->mosaic_output_mode &&
                            !internal_decode->rgb_mosaic_output_mode) {
                            qw_jp2_internal_decode_set_quantization(
                                component_decode, selected);
                        }
                    }
                    memcpy(tile_state->internal_quantizations,
                           effective_quantizations,
                           (size_t)component_count *
                               sizeof(*effective_quantizations));
                    tile_state->internal_quantization =
                        effective_quantizations[0];
                }
            }
            {
                int geometry_rc = qw_jp2_validate_tile_precinct_grids(
                    &effective_tile_geometry, effective_geometry,
                    component_count);
                if (geometry_rc != QW_STATUS_OK) return geometry_rc;
            }
        }

        if (tile_poc_payload) {
            size_t progression_count = 0u;
            int poc_rc;
            if (tile_part_index == 0u) {
                tile_state->tile_poc_seen = 1u;
            } else if (!tile_state->tile_poc_seen) {
                return QW_STATUS_ERR_FORMAT;
            }
            poc_rc = qw_jp2_packet_progression_parse_poc(
                tile_poc_payload, tile_poc_payload_size, component_count,
                tile_state->layer_count,
                tile_state->component_resolution_counts, NULL, 0u,
                &progression_count);
            if (poc_rc != QW_JP2_PACKET_OK) {
                return qw_jp2_packet_status_to_image(poc_rc);
            }
        } else if (tile_part_index == 0u && main_poc_payload) {
            size_t progression_count = 0u;
            int poc_rc = qw_jp2_packet_progression_parse_poc(
                main_poc_payload, main_poc_payload_size, component_count,
                tile_state->layer_count,
                tile_state->component_resolution_counts, NULL, 0u,
                &progression_count);
            if (poc_rc != QW_JP2_PACKET_OK) {
                return qw_jp2_packet_status_to_image(poc_rc);
            }
        }

        if (tile_part_index == 0u) {
            const uint8_t *effective_poc_payload =
                tile_poc_payload ? tile_poc_payload : main_poc_payload;
            size_t effective_poc_payload_size = tile_poc_payload
                ? tile_poc_payload_size : main_poc_payload_size;
            int packet_state_rc = qw_jp2_tile_packet_state_init(
                tile_state, &effective_tile_geometry, effective_geometry,
                effective_quantizations, tile_state->component_roi_shifts,
                component_count, tile_cod_parameters.progression_order,
                tile_cod_parameters.coding_style_flags,
                effective_poc_payload, effective_poc_payload_size,
                &packet_budget_remaining, &codeblock_budget_remaining,
                &quantization_budget_remaining);
            if (packet_state_rc != QW_STATUS_OK) return packet_state_rc;
        } else if (!tile_state->packet_state.initialized) {
            return QW_STATUS_ERR_FORMAT;
        } else if (tile_poc_payload) {
            int packet_state_rc = qw_jp2_tile_packet_state_append_poc(
                &tile_state->packet_state, component_count,
                tile_state->layer_count, tile_poc_payload,
                tile_poc_payload_size, &packet_budget_remaining,
                &codeblock_budget_remaining);
            if (packet_state_rc != QW_STATUS_OK) return packet_state_rc;
        }

        if (tile_ppt_segment_count != 0u) {
            int collect_rc = qw_jp2_collect_packed_headers(
                tile_ppt_segments, tile_ppt_segment_count,
                &packed_tile_headers, &packed_header_size);
            if (collect_rc != QW_STATUS_OK) return collect_rc;
            packed_header_data = packed_tile_headers;
            has_packed_headers = 1;
        }
        if (position > tile_part_end) {
            free(packed_tile_headers);
            return QW_STATUS_ERR_FORMAT;
        }
        {
            const uint8_t *header_data = has_packed_headers
                ? packed_header_data : data + position;
            size_t header_size = has_packed_headers
                ? packed_header_size : tile_part_end - position;
            int traversal_rc = qw_jp2_traverse_tile_part_packets(
                &tile_state->packet_state, component_count, header_data,
                header_size, data + position, tile_part_end - position,
                has_packed_headers, &body_budget_remaining,
                &segment_metadata_budget_remaining);
            free(packed_tile_headers);
            if (traversal_rc != QW_STATUS_OK) return traversal_rc;
        }
        if (tile_part_ordinal == SIZE_MAX) return QW_STATUS_ERR_RESOURCE_LIMIT;
        ++tile_part_ordinal;
        position = tile_part_end;
    }
}

static int qw_jp2_validate_tile_parts(
    const uint8_t *data,
    size_t size,
    size_t first_sot_offset,
    uint32_t tile_count,
    uint16_t component_count,
    uint16_t main_layer_count,
    const uint8_t *main_component_resolution_counts,
    const QwJp2PacketTileGeometry *siz_tile_geometry,
    const QwJp2PacketComponentGeometry *main_component_geometry,
    const QwJp2CodParameters *main_cod_parameters,
    const QwJp2QuantizationParameters *main_qcd_parameters,
    const QwJp2QuantizationParameters *main_qcc_parameters,
    const uint8_t *main_roi_shifts,
    const uint8_t *main_poc_payload,
    size_t main_poc_payload_size,
    const QwJp2PackedHeaderSegment *main_ppm_segments,
    size_t main_ppm_segment_count,
    QwJp2InternalDecodeContext *internal_decode)
{
    QwJp2TileCodingState *tile_states;
    uint8_t *main_ppm_data = NULL;
    size_t main_ppm_size = 0u;
    int rc;

    if (tile_count == 0u ||
        tile_count > (uint32_t)QW_JP2_MAX_TRACKED_TILE_STATES ||
        (size_t)tile_count > SIZE_MAX / sizeof(*tile_states)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    tile_states = (QwJp2TileCodingState *)calloc(
        (size_t)tile_count, sizeof(*tile_states));
    if (!tile_states) return QW_STATUS_ERR_OUT_OF_MEMORY;
    rc = qw_jp2_collect_packed_headers(main_ppm_segments,
                                       main_ppm_segment_count,
                                       &main_ppm_data, &main_ppm_size);
    if (rc != QW_STATUS_OK) {
        free(tile_states);
        return rc;
    }
    rc = qw_jp2_validate_tile_parts_impl(
        data, size, first_sot_offset, tile_count, component_count,
        main_layer_count, main_component_resolution_counts,
        siz_tile_geometry, main_component_geometry, main_cod_parameters,
        main_qcd_parameters, main_qcc_parameters, main_roi_shifts,
        main_poc_payload, main_poc_payload_size, main_ppm_data,
        main_ppm_size, main_ppm_segment_count, tile_states,
        internal_decode);
    free(main_ppm_data);
    qw_jp2_tile_coding_states_clear(tile_states, tile_count);
    free(tile_states);
    return rc;
}

static int qw_jp2_validate_codestream(const uint8_t *data,
                                     size_t size,
                                     const QwJp2ContainerHeader *container,
                                     QwJp2CodestreamHeader *out_header,
                                     QwJp2InternalDecodeContext *internal_decode)
{
    uint32_t x_size;
    uint32_t y_size;
    uint32_t x_origin;
    uint32_t y_origin;
    uint32_t tile_width;
    uint32_t tile_height;
    uint32_t tile_x_origin;
    uint32_t tile_y_origin;
    uint32_t tiles_x;
    uint32_t tiles_y;
    uint32_t expected_siz_length;
    uint16_t siz_length;
    uint16_t component_count;
    size_t position;
    uint8_t component_transform = 0u;
    QwJp2QuantizationMarker main_qcd_marker = {0};
    QwJp2QuantizationMarker main_qcc_markers[4] = {{0}};
    QwJp2QuantizationParameters main_qcd_parameters = {0};
    QwJp2QuantizationParameters main_qcc_parameters[4] = {{0}};
    const uint8_t *main_poc_payload = NULL;
    size_t main_poc_payload_size = 0u;
    QwJp2PackedHeaderSegment main_ppm_segments[
        QW_JP2_PACKED_HEADER_MAX_SEGMENTS] = {{0}};
    size_t main_ppm_segment_count = 0u;
    uint8_t component_resolution_counts[4] = {0u};
    uint8_t seen_main_coc[4] = {0u};
    uint8_t main_roi_shifts[4] = {0u};
    uint8_t seen_main_rgn_mask = 0u;
    QwJp2ComponentCodingParameters main_coc_parameters[4] = {{0}};
    QwJp2PacketComponentGeometry main_component_geometry[4] = {{0}};
    QwJp2PacketTileGeometry siz_tile_geometry = {0};
    QwJp2CodParameters main_cod_parameters = {0};
    int seen_cod = 0;
    int seen_poc = 0;

    if (!data || !out_header || size < 4u || data[0] != 0xffu ||
        data[1] != QW_J2K_MARKER_SOC) {
        return QW_STATUS_ERR_FORMAT;
    }
    memset(out_header, 0, sizeof(*out_header));
    if (data[2] != 0xffu || data[3] != QW_J2K_MARKER_SIZ || size < 6u) {
        return QW_STATUS_ERR_FORMAT;
    }
    siz_length = qw_jp2_read_u16(data + 4u);
    if (siz_length < 38u || (size_t)siz_length > size - 4u) {
        return QW_STATUS_ERR_FORMAT;
    }
    component_count = qw_jp2_read_u16(data + 40u);
    if (!component_count) return QW_STATUS_ERR_FORMAT;
    expected_siz_length = 38u + 3u * (uint32_t)component_count;
    if ((uint32_t)siz_length != expected_siz_length)
        return QW_STATUS_ERR_FORMAT;
    if (component_count > 4u) return QW_STATUS_ERR_UNSUPPORTED;

    x_size = qw_jp2_read_u32(data + 8u);
    y_size = qw_jp2_read_u32(data + 12u);
    x_origin = qw_jp2_read_u32(data + 16u);
    y_origin = qw_jp2_read_u32(data + 20u);
    tile_width = qw_jp2_read_u32(data + 24u);
    tile_height = qw_jp2_read_u32(data + 28u);
    tile_x_origin = qw_jp2_read_u32(data + 32u);
    tile_y_origin = qw_jp2_read_u32(data + 36u);
    if (x_size <= x_origin || y_size <= y_origin || !tile_width ||
        !tile_height || tile_x_origin > x_origin ||
        tile_y_origin > y_origin) {
        return QW_STATUS_ERR_FORMAT;
    }
    siz_tile_geometry.x_size = x_size;
    siz_tile_geometry.y_size = y_size;
    siz_tile_geometry.x_origin = x_origin;
    siz_tile_geometry.y_origin = y_origin;
    siz_tile_geometry.tile_width = tile_width;
    siz_tile_geometry.tile_height = tile_height;
    siz_tile_geometry.tile_x_origin = tile_x_origin;
    siz_tile_geometry.tile_y_origin = tile_y_origin;
    out_header->width = x_size - x_origin;
    out_header->height = y_size - y_origin;
    out_header->component_count = component_count;
    if (!qw_image_dimensions_within_limits(out_header->width,
                                           out_header->height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    {
        uint64_t tiles_x_64 =
            ((uint64_t)x_size - tile_x_origin + tile_width - 1u) / tile_width;
        uint64_t tiles_y_64 =
            ((uint64_t)y_size - tile_y_origin + tile_height - 1u) / tile_height;
        if (!tiles_x_64 || !tiles_y_64 ||
            tiles_x_64 > UINT32_MAX || tiles_y_64 > UINT32_MAX ||
            tiles_x_64 > UINT32_MAX / tiles_y_64) {
            return QW_STATUS_ERR_FORMAT;
        }
        tiles_x = (uint32_t)tiles_x_64;
        tiles_y = (uint32_t)tiles_y_64;
    }
    if (!tiles_x || !tiles_y) {
        return QW_STATUS_ERR_FORMAT;
    }
    out_header->tile_count = tiles_x * tiles_y;

    if (container && (container->width != out_header->width ||
                      container->height != out_header->height ||
                      container->component_count != component_count)) {
        return QW_STATUS_ERR_FORMAT;
    }
    for (uint16_t i = 0u; i < component_count; ++i) {
        const uint8_t *component = data + 42u + 3u * (size_t)i;
        uint8_t component_bits = component[0];
        unsigned precision = (unsigned)(component_bits & 0x7fu) + 1u;
        if (!component[1] || !component[2]) return QW_STATUS_ERR_FORMAT;
        main_component_geometry[i].x_sampling = component[1];
        main_component_geometry[i].y_sampling = component[2];
        if (precision > 32u) return QW_STATUS_ERR_UNSUPPORTED;
        if (!qw_jp2_check_component_bits(container, i, component_bits)) {
            return QW_STATUS_ERR_FORMAT;
        }
    }

    position = 4u + (size_t)siz_length;
    while (position < size) {
        size_t marker_offset = position;
        size_t segment_end;
        uint8_t marker;
        uint16_t marker_length;
        const uint8_t *payload;
        size_t payload_size;

        if (data[position] != 0xffu) return QW_STATUS_ERR_FORMAT;
        while (position < size && data[position] == 0xffu) ++position;
        if (position >= size) return QW_STATUS_ERR_FORMAT;
        marker = data[position++];
        if (marker == 0xffu || marker == 0x00u) return QW_STATUS_ERR_FORMAT;
        if (marker == QW_J2K_MARKER_SOT) {
            size_t progression_count = 0u;
            if (!seen_cod || !main_qcd_marker.present) {
                return QW_STATUS_ERR_FORMAT;
            }
            for (uint16_t component = 0u; component < component_count;
                 ++component) {
                const QwJp2ComponentCodingParameters *coding =
                    seen_main_coc[component]
                        ? &main_coc_parameters[component]
                        : &main_cod_parameters.component;
                if (!seen_main_coc[component]) {
                    component_resolution_counts[component] =
                        (uint8_t)(out_header->decomposition_levels + 1u);
                }
                qw_jp2_apply_component_coding(
                    &main_component_geometry[component], coding);
            }
            {
                int quantization_rc = qw_jp2_parse_quantization_marker(
                    main_qcd_marker.payload, main_qcd_marker.payload_size,
                    component_count, 0,
                    &main_qcd_parameters, NULL);
                if (quantization_rc != QW_STATUS_OK) return quantization_rc;
            }
            for (uint16_t component = 0u; component < component_count;
                 ++component) {
                if (main_qcc_markers[component].present) {
                    uint16_t parsed_component = UINT16_MAX;
                    int quantization_rc = qw_jp2_parse_quantization_marker(
                        main_qcc_markers[component].payload,
                        main_qcc_markers[component].payload_size,
                        component_count, 1,
                        &main_qcc_parameters[component], &parsed_component);
                    if (quantization_rc != QW_STATUS_OK) {
                        return quantization_rc;
                    }
                    if (parsed_component != component) {
                        return QW_STATUS_ERR_FORMAT;
                    }
                }
            }
            if (seen_poc &&
                qw_jp2_packet_progression_parse_poc(
                    main_poc_payload, main_poc_payload_size,
                    component_count, 0u, NULL, NULL, 0u,
                    &progression_count) != QW_JP2_PACKET_OK) {
                return QW_STATUS_ERR_FORMAT;
            }
            out_header->multiple_component_transform = component_transform;
            if (internal_decode) {
                if (component_count == 3u || component_count == 4u) {
                    QwJp2QuantizationParameters internal_quantizations[4] =
                        {{0}};
                    uint8_t internal_roi_shifts[4] = {0u};
                    uint8_t internal_precisions[4] = {0u};
                    uint8_t internal_decomposition_levels[4] = {0u};
                    uint8_t internal_component_x_sampling[4] = {0u};
                    uint8_t internal_component_y_sampling[4] = {0u};
                    uint8_t alpha_channel_type = 0u;
                    uint8_t rgb_component_indices[3] = {0u, 1u, 2u};
                    uint8_t alpha_component_index = 3u;
                    uint8_t wavelet_transform =
                        main_component_geometry[0].wavelet_transform;
                    int sycc_color_space = container &&
                        container->has_enumerated_color_space &&
                        container->color_method == 1u &&
                        container->enumerated_color_space == 18u;
                    int rgb_tile_mosaic_mode = out_header->tile_count > 1u;
                    uint8_t resolution_reduction =
                        internal_decode->resolution_reduction;
                    int raw_independent_rgb_mode =
                        !container && component_count == 3u &&
                        main_cod_parameters.multiple_component_transform ==
                            0u && wavelet_transform == 1u;

                    /* Nonzero tile origins are supported only when aligned
                     * with the image origin for this single-tile RGB path. */
                    if ((tile_x_origin != 0u &&
                         tile_x_origin != x_origin) ||
                        (tile_y_origin != 0u &&
                         tile_y_origin != y_origin) ||
                        (rgb_tile_mosaic_mode &&
                         (resolution_reduction != 0u || x_origin != 0u ||
                          y_origin != 0u)) ||
                        main_cod_parameters.multiple_component_transform > 1u ||
                        (wavelet_transform != 0u && wavelet_transform != 1u)) {
                        return QW_STATUS_ERR_UNSUPPORTED;
                    }
                    if (resolution_reduction > 32u) {
                        return QW_STATUS_ERR_UNSUPPORTED;
                    }
                    for (uint16_t component_index = 0u;
                         component_index < component_count; ++component_index) {
                        uint8_t x_sampling =
                            main_component_geometry[component_index].x_sampling;
                        uint8_t y_sampling =
                            main_component_geometry[component_index].y_sampling;
                        uint8_t source_levels = main_component_geometry[
                            component_index].decomposition_levels;
                        uint64_t reduction_scale =
                            UINT64_C(1) << resolution_reduction;
                        uint64_t source_scale;
                        uint64_t x_alignment;
                        uint64_t y_alignment;
                        if (!x_sampling || !y_sampling || source_levels > 32u) {
                            return QW_STATUS_ERR_UNSUPPORTED;
                        }
                        source_scale = UINT64_C(1) << source_levels;
                        x_alignment = reduction_scale * x_sampling;
                        y_alignment = reduction_scale * y_sampling;
                        if ((uint64_t)x_origin % x_alignment != 0u ||
                            (uint64_t)y_origin % y_alignment != 0u ||
                            (uint64_t)qw_jp2_internal_component_coordinate(
                                x_origin, x_sampling) % source_scale != 0u ||
                            (uint64_t)qw_jp2_internal_component_coordinate(
                                y_origin, y_sampling) % source_scale != 0u) {
                            return QW_STATUS_ERR_UNSUPPORTED;
                        }
                    }
                    if (container) {
                        if (container->color_box_count != 1u ||
                            container->color_method != 1u ||
                            !container->has_enumerated_color_space ||
                            (container->enumerated_color_space != 16u &&
                             container->enumerated_color_space != 18u) ||
                            container->has_channel_mapping ||
                            !qw_jp2_channel_definition_rgb_alpha_type(
                                container, component_count,
                                &alpha_channel_type,
                                rgb_component_indices,
                                &alpha_component_index)) {
                            return QW_STATUS_ERR_UNSUPPORTED;
                        }
                        if (sycc_color_space &&
                            (component_count != 3u ||
                             main_cod_parameters.multiple_component_transform !=
                                 0u ||
                             alpha_channel_type != 0u ||
                             container->has_channel_definition ||
                             rgb_component_indices[0] != 0u ||
                             rgb_component_indices[1] != 1u ||
                             rgb_component_indices[2] != 2u)) {
                            return QW_STATUS_ERR_UNSUPPORTED;
                        }
                    } else {
                        uint8_t mct =
                            main_cod_parameters.multiple_component_transform;
                        if (component_count != 3u ||
                            (mct == 0u && wavelet_transform != 1u)) {
                            return QW_STATUS_ERR_UNSUPPORTED;
                        }
                    }
                    if (main_cod_parameters.multiple_component_transform == 1u &&
                        (rgb_component_indices[0] != 0u ||
                         rgb_component_indices[1] != 1u ||
                         rgb_component_indices[2] != 2u ||
                         (component_count == 4u &&
                          alpha_component_index != 3u))) {
                        return QW_STATUS_ERR_UNSUPPORTED;
                    }
                    for (uint16_t component_index = 0u;
                         component_index < component_count; ++component_index) {
                        const uint8_t *component =
                            data + 42u + 3u * (size_t)component_index;
                        unsigned precision =
                            (unsigned)(component[0] & 0x7fu) + 1u;
                        internal_component_x_sampling[component_index] =
                            main_component_geometry[component_index].x_sampling;
                        internal_component_y_sampling[component_index] =
                            main_component_geometry[component_index].y_sampling;
                        const QwJp2QuantizationParameters *quantization =
                            main_qcc_parameters[component_index].present
                                ? &main_qcc_parameters[component_index]
                                : &main_qcd_parameters;
                        uint8_t source_decomposition_levels =
                            main_component_geometry[component_index]
                                .decomposition_levels;
                        if ((!raw_independent_rgb_mode &&
                             (component[0] & 0x80u) != 0u) ||
                            (sycc_color_space &&
                             (precision != 8u || component[1] != 1u ||
                              component[2] != 1u)) ||
                            (rgb_tile_mosaic_mode &&
                             (component[1] != 1u || component[2] != 1u)) ||
                            (main_cod_parameters.multiple_component_transform ==
                                 1u &&
                             (component[1] != 1u || component[2] != 1u)) ||
                            resolution_reduction >
                                source_decomposition_levels ||
                            (main_cod_parameters.multiple_component_transform ==
                                 1u && component_index < 3u &&
                             precision != 8u) ||
                            ((!raw_independent_rgb_mode ||
                              rgb_tile_mosaic_mode) &&
                             main_component_geometry[component_index]
                                     .decomposition_levels !=
                                 main_component_geometry[0]
                                     .decomposition_levels) ||
                            main_component_geometry[component_index]
                                    .wavelet_transform != wavelet_transform ||
                            !qw_jp2_internal_quantization_supported(
                                wavelet_transform, quantization)) {
                            return QW_STATUS_ERR_UNSUPPORTED;
                        }
                        internal_quantizations[component_index] = *quantization;
                        internal_precisions[component_index] =
                            (uint8_t)precision;
                        internal_decomposition_levels[component_index] =
                            (uint8_t)(source_decomposition_levels -
                                      resolution_reduction);
                        internal_roi_shifts[component_index] =
                            main_roi_shifts[component_index];
                    }
                    uint32_t internal_width = (uint32_t)
                        qw_jp2_internal_resolution_extent_at_origin(
                            x_origin, out_header->width, resolution_reduction);
                    uint32_t internal_height = (uint32_t)
                        qw_jp2_internal_resolution_extent_at_origin(
                            y_origin, out_header->height, resolution_reduction);
                    int init_status = qw_jp2_internal_rgb_context_init(
                        internal_decode, internal_width, internal_height,
                        internal_decomposition_levels[0],
                        internal_decomposition_levels, wavelet_transform,
                        main_cod_parameters.multiple_component_transform,
                        component_count, alpha_channel_type,
                        rgb_component_indices, alpha_component_index,
                        internal_component_x_sampling,
                        internal_component_y_sampling,
                        internal_precisions,
                        internal_quantizations, internal_roi_shifts,
                        0u, rgb_tile_mosaic_mode);
                    if (init_status != QW_STATUS_OK) return init_status;
                    internal_decode->sycc_color_space =
                        (uint8_t)sycc_color_space;
                    internal_decode->source_decomposition_levels =
                        main_component_geometry[0].decomposition_levels;
                    for (uint16_t component_index = 0u;
                         component_index < component_count; ++component_index) {
                        internal_decode->component_contexts[component_index]
                            .component_x_origin =
                                qw_jp2_internal_component_coordinate(
                                    x_origin,
                                    internal_component_x_sampling[
                                        component_index]);
                        internal_decode->component_contexts[component_index]
                            .component_y_origin =
                                qw_jp2_internal_component_coordinate(
                                    y_origin,
                                    internal_component_y_sampling[
                                        component_index]);
                        internal_decode->component_contexts[component_index]
                            .source_decomposition_levels =
                                main_component_geometry[component_index]
                                    .decomposition_levels;
                    }
                    if (!container && component_count == 3u) {
                        internal_decode->use_per_coefficient_reconstruction =
                            1u;
                    }
                } else if (component_count == 1u) {
                    const uint8_t *component = data + 42u;
                    unsigned component_precision =
                        (unsigned)(component[0] & 0x7fu) + 1u;
                    int palette_rgb_mode = container &&
                        container->has_rgb_palette_mapping;
                    int palette_gray_mode = container &&
                        container->has_gray_palette_mapping;
                    int palette_mapping_mode = palette_rgb_mode ||
                        palette_gray_mode;
                    const QwJp2ComponentCodingParameters *internal_coding =
                        seen_main_coc[0] ? &main_coc_parameters[0]
                                         : &main_cod_parameters.component;
                    const QwJp2QuantizationParameters *internal_quantization =
                        main_qcc_parameters[0].present
                            ? &main_qcc_parameters[0]
                            : &main_qcd_parameters;
                    int tile_mosaic_mode = out_header->tile_count > 1u ||
                        tile_x_origin != x_origin || tile_y_origin != y_origin;
                    if ((tile_mosaic_mode &&
                         internal_decode->resolution_reduction != 0u) ||
                        !qw_jp2_internal_quantization_supported(
                            internal_coding->wavelet_transform,
                            internal_quantization) ||
                        main_cod_parameters.multiple_component_transform != 0u ||
                        (container &&
                         (container->color_box_count != 1u ||
                          container->color_method != 1u ||
                          !container->has_enumerated_color_space ||
                          (palette_rgb_mode
                               ? (container->enumerated_color_space != 16u ||
                                  !container->has_channel_mapping)
                               : palette_gray_mode
                                   ? (container->enumerated_color_space != 17u ||
                                      !container->has_channel_mapping)
                                   : (container->enumerated_color_space != 17u ||
                                      container->has_channel_mapping))))) {
                        return QW_STATUS_ERR_UNSUPPORTED;
                    }
                    if (palette_mapping_mode &&
                        (tile_mosaic_mode || component_precision != 8u ||
                         (component[0] & 0x80u) != 0u ||
                         component[1] != 1u || component[2] != 1u)) {
                        return QW_STATUS_ERR_UNSUPPORTED;
                    }
                    {
                    uint64_t origin_alignment;
                    uint32_t component_origin_x =
                        qw_jp2_internal_component_coordinate(
                            x_origin, component[1]);
                    uint32_t component_origin_y =
                        qw_jp2_internal_component_coordinate(
                            y_origin, component[2]);
                    uint32_t component_end_x =
                        qw_jp2_internal_component_coordinate(
                            x_size, component[1]);
                    uint32_t component_end_y =
                        qw_jp2_internal_component_coordinate(
                            y_size, component[2]);
                    uint32_t component_width;
                    uint32_t component_height;
                    unsigned resolution_reduction =
                        internal_decode->resolution_reduction;
                    uint32_t reduced_component_width;
                    uint32_t reduced_component_height;
                    if (component_end_x <= component_origin_x ||
                        component_end_y <= component_origin_y) {
                        return QW_STATUS_ERR_FORMAT;
                    }
                    component_width = component_end_x - component_origin_x;
                    component_height = component_end_y - component_origin_y;
                    if (resolution_reduction >
                        internal_coding->decomposition_levels) {
                        return QW_STATUS_ERR_UNSUPPORTED;
                    }
                    origin_alignment = UINT64_C(1) <<
                        internal_coding->decomposition_levels;
                    if (!tile_mosaic_mode &&
                        (x_origin != 0u || y_origin != 0u) &&
                        (component[1] != 1u || component[2] != 1u ||
                         (uint64_t)x_origin % origin_alignment != 0u ||
                         (uint64_t)y_origin % origin_alignment != 0u)) {
                        return QW_STATUS_ERR_UNSUPPORTED;
                    }
                    reduced_component_width = (uint32_t)
                        qw_jp2_internal_resolution_extent_at_origin(
                            component_origin_x, component_width,
                            resolution_reduction);
                    reduced_component_height = (uint32_t)
                        qw_jp2_internal_resolution_extent_at_origin(
                            component_origin_y, component_height,
                            resolution_reduction);
                    internal_decode->source_decomposition_levels =
                        internal_coding->decomposition_levels;
                    if (palette_rgb_mode) {
                        internal_decode->output_channels = 3u;
                    }
                    int init_status = tile_mosaic_mode
                        ? qw_jp2_internal_mosaic_context_init(
                              internal_decode, component_width,
                              component_height,
                              internal_coding->decomposition_levels)
                        : qw_jp2_internal_decode_context_init(
                              internal_decode, reduced_component_width,
                              reduced_component_height,
                              (uint8_t)(internal_coding->decomposition_levels -
                                        resolution_reduction),
                              0u);
                    if (init_status != QW_STATUS_OK) return init_status;
                    internal_decode->component_x_origin = component_origin_x;
                    internal_decode->component_y_origin = component_origin_y;
                    internal_decode->wavelet_transform =
                        internal_coding->wavelet_transform;
                    internal_decode->precision =
                        (uint8_t)component_precision;
                    qw_jp2_internal_decode_set_quantization(
                        internal_decode, internal_quantization);
                    }
                } else {
                    return QW_STATUS_ERR_UNSUPPORTED;
                }
            }
            {
                int tile_parts_status = qw_jp2_validate_tile_parts(
                    data, size, marker_offset, out_header->tile_count,
                    component_count, out_header->layer_count,
                    component_resolution_counts, &siz_tile_geometry,
                    main_component_geometry, &main_cod_parameters,
                    &main_qcd_parameters, main_qcc_parameters,
                    main_roi_shifts,
                    main_poc_payload, main_poc_payload_size,
                    main_ppm_segments, main_ppm_segment_count,
                    internal_decode);
                return tile_parts_status;
            }
        }
        if (qw_jp2_marker_has_no_segment(marker)) {
            continue;
        }
        if (marker == QW_J2K_MARKER_EOC || marker == QW_J2K_MARKER_SOD ||
            marker == QW_J2K_MARKER_SOC || marker == 0x92u ||
            marker == 0x01u || (marker >= 0xd0u && marker <= 0xd7u)) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (size - position < 2u) return QW_STATUS_ERR_FORMAT;
        marker_length = qw_jp2_read_u16(data + position);
        if (marker_length < 2u ||
            (size_t)marker_length > size - position) {
            return QW_STATUS_ERR_FORMAT;
        }
        payload = data + position + 2u;
        payload_size = (size_t)marker_length - 2u;
        segment_end = position + (size_t)marker_length;
        if (marker == QW_J2K_MARKER_SIZ) return QW_STATUS_ERR_FORMAT;
        if (marker == QW_J2K_MARKER_RGN) {
            uint16_t component_index;
            uint8_t roi_shift;
            uint8_t component_mask;
            int rc = qw_jp2_parse_rgn_payload(
                payload, payload_size, component_count,
                &component_index, &roi_shift);
            if (rc != QW_STATUS_OK) return rc;
            component_mask = (uint8_t)(1u << component_index);
            if (seen_main_rgn_mask & component_mask) {
                return QW_STATUS_ERR_FORMAT;
            }
            seen_main_rgn_mask |= component_mask;
            main_roi_shifts[component_index] = roi_shift;
        } else if (marker == QW_J2K_MARKER_COC) {
            uint16_t component_index;
            QwJp2ComponentCodingParameters component_coding = {0};
            int rc = qw_jp2_parse_coc_payload(
                payload, payload_size, component_count, &component_index,
                &component_coding);
            if (rc != QW_STATUS_OK) return rc;
            if (seen_main_coc[component_index]) return QW_STATUS_ERR_FORMAT;
            seen_main_coc[component_index] = 1u;
            main_coc_parameters[component_index] = component_coding;
            component_resolution_counts[component_index] =
                (uint8_t)(component_coding.decomposition_levels + 1u);
        } else if (marker == QW_J2K_MARKER_PPM) {
            if (payload_size < 5u) return QW_STATUS_ERR_FORMAT;
            if (main_ppm_segment_count >=
                (size_t)QW_JP2_PACKED_HEADER_MAX_SEGMENTS) {
                return QW_STATUS_ERR_RESOURCE_LIMIT;
            }
            if (payload[0] != (uint8_t)main_ppm_segment_count) {
                return QW_STATUS_ERR_FORMAT;
            }
            main_ppm_segments[main_ppm_segment_count].sequence_index =
                payload[0];
            main_ppm_segments[main_ppm_segment_count].payload = payload + 1u;
            main_ppm_segments[main_ppm_segment_count].payload_size =
                payload_size - 1u;
            ++main_ppm_segment_count;
        } else if (marker == QW_J2K_MARKER_PPT) {
            return QW_STATUS_ERR_FORMAT;
        } else if (marker == QW_J2K_MARKER_POC) {
            if (seen_poc) return QW_STATUS_ERR_FORMAT;
            main_poc_payload = payload;
            main_poc_payload_size = payload_size;
            seen_poc = 1;
        } else if (marker == QW_J2K_MARKER_COD) {
            QwJp2CodParameters cod;
            int rc;
            if (seen_cod) return QW_STATUS_ERR_FORMAT;
            rc = qw_jp2_parse_cod_payload(payload, payload_size,
                                          component_count, &cod);
            if (rc != QW_STATUS_OK) return rc;
            main_cod_parameters = cod;
            out_header->layer_count = cod.layer_count;
            out_header->decomposition_levels = cod.decomposition_levels;
            out_header->progression_order = cod.progression_order;
            out_header->multiple_component_transform =
                cod.multiple_component_transform;
            out_header->wavelet_transform = cod.wavelet_transform;
            component_transform = cod.multiple_component_transform;
            seen_cod = 1;
        } else if (marker == QW_J2K_MARKER_QCD) {
            if (main_qcd_marker.present || !payload_size) {
                return QW_STATUS_ERR_FORMAT;
            }
            main_qcd_marker.payload = payload;
            main_qcd_marker.payload_size = payload_size;
            main_qcd_marker.present = 1u;
        } else if (marker == QW_J2K_MARKER_QCC) {
            size_t component_width = component_count < 257u ? 1u : 2u;
            uint16_t component_index;
            if (payload_size < component_width + 1u) {
                return QW_STATUS_ERR_FORMAT;
            }
            component_index = component_width == 1u
                ? payload[0] : qw_jp2_read_u16(payload);
            if (component_index >= component_count ||
                main_qcc_markers[component_index].present) {
                return QW_STATUS_ERR_FORMAT;
            }
            main_qcc_markers[component_index].payload = payload;
            main_qcc_markers[component_index].payload_size = payload_size;
            main_qcc_markers[component_index].present = 1u;
        }
        position = segment_end;
    }
    return QW_STATUS_ERR_FORMAT;
}

static int qw_jp2_process_input(const void *input,
                                size_t size,
                                QwJp2InternalDecodeContext *internal_decode)
{
    static const uint8_t signature[12] = {
        0x00u, 0x00u, 0x00u, 0x0cu, 0x6au, 0x50u, 0x20u, 0x20u,
        0x0du, 0x0au, 0x87u, 0x0au
    };
    const uint8_t *data = (const uint8_t *)input;
    const uint8_t *codestream = NULL;
    size_t codestream_size = 0u;
    QwJp2ContainerHeader container = {0};
    QwJp2CodestreamHeader header = {0};
    int has_file_type = 0;
    int has_jp2_brand = 0;
    int has_header = 0;
    int has_codestream = 0;
    int is_container;
    int rc;
    size_t offset = 0u;

    if (!data || size < 4u) return QW_STATUS_ERR_FORMAT;
    if (size < sizeof(signature) &&
        memcmp(data, signature, size) == 0) {
        return QW_STATUS_ERR_FORMAT;
    }
    is_container = size >= sizeof(signature) &&
                   memcmp(data, signature, sizeof(signature)) == 0;
    if (!is_container) {
        if (data[0] != 0xffu || data[1] != QW_J2K_MARKER_SOC) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        return qw_jp2_validate_codestream(data, size, NULL, &header,
                                          internal_decode);
    }

    while (offset < size) {
        QwJp2Box box;
        if (!qw_jp2_read_box(data, size, offset, &box)) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (offset == 0u) {
            if (box.type != QW_JP2_BOX_SIGNATURE || box.end_offset != 12u) {
                return QW_STATUS_ERR_FORMAT;
            }
        } else if (box.type == QW_JP2_BOX_FILE_TYPE) {
            const uint8_t *payload = data + box.payload_offset;
            uint32_t major_brand;
            if (has_file_type || offset != 12u || box.payload_size < 8u ||
                ((box.payload_size - 8u) % 4u) != 0u) {
                return QW_STATUS_ERR_FORMAT;
            }
            major_brand = qw_jp2_read_u32(payload);
            if (qw_jp2_brand_is_jpx(major_brand)) {
                return QW_STATUS_ERR_UNSUPPORTED;
            }
            has_jp2_brand = major_brand == QW_JP2_BRAND_JP2;
            for (size_t i = 8u; i < box.payload_size; i += 4u) {
                uint32_t brand = qw_jp2_read_u32(payload + i);
                if (qw_jp2_brand_is_jpx(brand)) {
                    return QW_STATUS_ERR_UNSUPPORTED;
                }
                if (brand == QW_JP2_BRAND_JP2) has_jp2_brand = 1;
            }
            if (!has_jp2_brand) return QW_STATUS_ERR_UNSUPPORTED;
            has_file_type = 1;
        } else if (box.type == QW_JP2_BOX_HEADER) {
            int rc;
            if (!has_file_type || has_header || has_codestream) {
                return QW_STATUS_ERR_FORMAT;
            }
            rc = qw_jp2_parse_image_header(data + box.payload_offset,
                                           box.payload_size, &container);
            if (rc != QW_STATUS_OK) return rc;
            has_header = 1;
        } else if (box.type == QW_JP2_BOX_CODESTREAM) {
            if (!has_file_type || !has_header || has_codestream) {
                return QW_STATUS_ERR_FORMAT;
            }
            codestream = data + box.payload_offset;
            codestream_size = box.payload_size;
            has_codestream = 1;
        }
        offset = box.end_offset;
    }
    if (!has_file_type || !has_header || !has_codestream) {
        return QW_STATUS_ERR_FORMAT;
    }
    rc = qw_jp2_validate_codestream(codestream, codestream_size,
                                    &container, &header, internal_decode);
    if (rc != QW_STATUS_OK) return rc;
    if (internal_decode && container.has_rgb_palette_mapping) {
        return qw_jp2_apply_rgb_palette_mapping(internal_decode, &container);
    }
    if (internal_decode && container.has_gray_palette_mapping) {
        return qw_jp2_apply_gray_palette_mapping(internal_decode, &container);
    }
    return QW_STATUS_OK;
}

int qw_jp2_validate_structure(const void *input, size_t size)
{
    return qw_jp2_process_input(input, size, NULL);
}

int qw_jp2_decode_internal_gray8_reduced(const void *input,
                                         size_t size,
                                         uint8_t resolution_reduction,
                                         QwPageImage *out_page)
{
    QwJp2InternalDecodeContext context = {0};
    size_t output_size;
    int rc;

    if (!out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    context.resolution_reduction = resolution_reduction;
    rc = qw_jp2_process_input(input, size, &context);
    if (rc == QW_STATUS_OK) {
        if (!context.pixels || !context.initialized ||
            (context.output_channels != 1u && context.output_channels != 3u) ||
            !qw_checked_size_mul(context.pixel_count,
                                 (size_t)context.output_channels,
                                 &output_size)) {
            rc = QW_STATUS_ERR_FORMAT;
        } else {
            out_page->data = context.pixels;
            out_page->size = output_size;
            out_page->kind = context.output_channels == 3u
                ? QW_PAGE_IMAGE_RGB24 : QW_PAGE_IMAGE_GRAY8;
            out_page->width = context.width;
            out_page->height = context.height;
            context.pixels = NULL;
        }
    }
    qw_jp2_internal_decode_context_clear(&context);
    return rc;
}

int qw_jp2_decode_internal_gray8(const void *input,
                                 size_t size,
                                 QwPageImage *out_page)
{
    return qw_jp2_decode_internal_gray8_reduced(input, size, 0u, out_page);
}

