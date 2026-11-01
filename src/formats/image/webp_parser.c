#include "image_internal.h"

#include "../../core/byte_utils.h"

#include <stdlib.h>
#include <string.h>

typedef struct QwWebPChunk {
    const uint8_t *data;
    size_t size;
    size_t next;
    uint32_t type;
} QwWebPChunk;

enum { QW_WEBP_MAX_FRAMES = 4096u };

static uint32_t webp_read_u24_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16);
}

static uint32_t webp_fourcc(const char value[4])
{
    return (uint32_t)(uint8_t)value[0] |
           ((uint32_t)(uint8_t)value[1] << 8) |
           ((uint32_t)(uint8_t)value[2] << 16) |
           ((uint32_t)(uint8_t)value[3] << 24);
}

static int webp_is_known_chunk(uint32_t type)
{
    return type == webp_fourcc("VP8X") || type == webp_fourcc("ICCP") ||
           type == webp_fourcc("EXIF") || type == webp_fourcc("XMP ") ||
           type == webp_fourcc("ALPH") || type == webp_fourcc("VP8 ") ||
           type == webp_fourcc("VP8L") || type == webp_fourcc("ANIM") ||
           type == webp_fourcc("ANMF");
}

static int webp_alpha_chunk_valid(const QwWebPChunk *chunk)
{
    return chunk && chunk->size >= 1u && (chunk->data[0] & 0xc0u) == 0u &&
           (chunk->data[0] & 0x03u) <= 1u;
}

static int webp_read_chunk(const uint8_t *bytes,
                           size_t end,
                           size_t position,
                           QwWebPChunk *chunk)
{
    uint32_t chunk_size;
    size_t payload;
    size_t chunk_end;
    size_t next;
    if (!bytes || !chunk || position > end || end - position < 8u)
        return 0;
    chunk->type = qw_read_u32_le(bytes + position);
    chunk_size = qw_read_u32_le(bytes + position + 4u);
    payload = position + 8u;
    if ((size_t)chunk_size > end - payload) return 0;
    chunk_end = payload + (size_t)chunk_size;
    if (chunk_size & 1u) {
        if (chunk_end >= end) return 0;
        if (bytes[chunk_end] != 0u) return 0;
        next = chunk_end + 1u;
    } else {
        next = chunk_end;
    }
    chunk->data = bytes + payload;
    chunk->size = (size_t)chunk_size;
    chunk->next = next;
    return 1;
}

static int webp_vp8_dimensions(const uint8_t *data,
                              size_t size,
                              uint32_t *width,
                              uint32_t *height)
{
    uint32_t frame_tag;
    if (!data || size < 10u || !width || !height) return 0;
    frame_tag = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                ((uint32_t)data[2] << 16);
    if ((frame_tag & 1u) != 0u || ((frame_tag >> 1) & 7u) > 3u ||
        (frame_tag & 0x10u) == 0u || data[3] != 0x9du ||
        data[4] != 0x01u || data[5] != 0x2au ||
        (frame_tag >> 5) > size - 10u) {
        return 0;
    }
    *width = (uint32_t)(qw_read_u16_le(data + 6u) & 0x3fffu);
    *height = (uint32_t)(qw_read_u16_le(data + 8u) & 0x3fffu);
    return *width != 0u && *height != 0u;
}

static int webp_vp8l_dimensions(const uint8_t *data,
                               size_t size,
                               uint32_t *width,
                               uint32_t *height,
                               int *has_alpha)
{
    uint32_t width_minus_one;
    uint32_t height_minus_one;
    if (!data || size < 5u || !width || !height || !has_alpha ||
        data[0] != 0x2fu || (data[4] & 0xe0u) != 0u) {
        return 0;
    }
    width_minus_one = (uint32_t)data[1] |
                      (((uint32_t)data[2] & 0x3fu) << 8);
    height_minus_one = ((uint32_t)data[2] >> 6) |
                       ((uint32_t)data[3] << 2) |
                       (((uint32_t)data[4] & 0x0fu) << 10);
    *width = width_minus_one + 1u;
    *height = height_minus_one + 1u;
    *has_alpha = (data[4] & 0x10u) != 0u;
    return 1;
}

static int webp_parse_frame(const uint8_t *data,
                            size_t size,
                            uint32_t canvas_width,
                            uint32_t canvas_height,
                            int *out_alpha,
                            QwWebPFrame *out_frame)
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint8_t flags;
    size_t position = 16u;
    int seen_alpha = 0;
    int seen_image = 0;
    int seen_unknown = 0;
    uint32_t image_width = 0u;
    uint32_t image_height = 0u;
    QwWebPFrame parsed_frame = {0};

    if (!data || size < 16u || !out_alpha) return 0;
    x = webp_read_u24_le(data) * 2u;
    y = webp_read_u24_le(data + 3u) * 2u;
    width = webp_read_u24_le(data + 6u) + 1u;
    height = webp_read_u24_le(data + 9u) + 1u;
    flags = data[15];
    if ((flags & 0xfcu) != 0u || x > canvas_width || y > canvas_height ||
        width > canvas_width - x || height > canvas_height - y) {
        return 0;
    }
    parsed_frame.x = x;
    parsed_frame.y = y;
    parsed_frame.width = width;
    parsed_frame.height = height;
    parsed_frame.duration_ms = webp_read_u24_le(data + 12u);
    parsed_frame.blend = (uint8_t)((flags >> 1) & 1u);
    parsed_frame.dispose = (uint8_t)(flags & 1u);
    while (position < size) {
        QwWebPChunk chunk;
        int image_alpha = 0;
        if (!webp_read_chunk(data, size, position, &chunk)) return 0;
        if (seen_unknown &&
            (chunk.type == webp_fourcc("ALPH") ||
             chunk.type == webp_fourcc("VP8 ") ||
             chunk.type == webp_fourcc("VP8L"))) return 0;
        if (chunk.type == webp_fourcc("ALPH")) {
            if (seen_alpha || seen_image || !webp_alpha_chunk_valid(&chunk))
                return 0;
            seen_alpha = 1;
            parsed_frame.alpha_data = chunk.data;
            parsed_frame.alpha_size = chunk.size;
        } else if (chunk.type == webp_fourcc("VP8 ")) {
            if (seen_image ||
                !webp_vp8_dimensions(chunk.data, chunk.size,
                                     &image_width, &image_height)) {
                return 0;
            }
            seen_image = 1;
            parsed_frame.codec = QW_WEBP_CODEC_VP8;
            parsed_frame.image_data = chunk.data;
            parsed_frame.image_size = chunk.size;
        } else if (chunk.type == webp_fourcc("VP8L")) {
            if (seen_image || seen_alpha ||
                !webp_vp8l_dimensions(chunk.data, chunk.size,
                                      &image_width, &image_height,
                                      &image_alpha)) {
                return 0;
            }
            seen_image = 1;
            seen_alpha = image_alpha;
            parsed_frame.codec = QW_WEBP_CODEC_VP8L;
            parsed_frame.image_data = chunk.data;
            parsed_frame.image_size = chunk.size;
        } else if (webp_is_known_chunk(chunk.type)) {
            return 0;
        } else {
            seen_unknown = 1;
        }
        position = chunk.next;
    }
    if (!seen_image || image_width != width || image_height != height)
        return 0;
    parsed_frame.has_alpha = (uint8_t)(seen_alpha != 0);
    *out_alpha = seen_alpha;
    if (out_frame) *out_frame = parsed_frame;
    return 1;
}

int qw_webp_probe(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    return bytes && size >= 12u && memcmp(bytes, "RIFF", 4u) == 0 &&
           memcmp(bytes + 8u, "WEBP", 4u) == 0;
}

int qw_webp_parse(const void *data,
                  size_t size,
                  uint32_t *out_width,
                  uint32_t *out_height,
                  int *out_has_animation)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t riff_size;
    size_t riff_end;
    size_t position = 12u;
    uint32_t canvas_width = 0u;
    uint32_t canvas_height = 0u;
    uint32_t simple_width = 0u;
    uint32_t simple_height = 0u;
    uint8_t extended_flags = 0u;
    int extended = 0;
    int seen_vp8x = 0;
    int seen_image = 0;
    int seen_alpha_chunk = 0;
    int seen_alpha = 0;
    int seen_iccp = 0;
    int seen_exif = 0;
    int seen_xmp = 0;
    int seen_anim = 0;
    int seen_frame = 0;
    int seen_unknown = 0;
    int frame_alpha = 0;
    int image_is_vp8l = 0;
    int image_has_alpha = 0;
    int first_chunk = 1;
    size_t frame_count = 0u;

    if (!qw_webp_probe(data, size) || !out_width || !out_height ||
        !out_has_animation) {
        return QW_STATUS_ERR_FORMAT;
    }
    riff_size = qw_read_u32_le(bytes + 4u);
    if (riff_size < 4u || (riff_size & 1u) != 0u ||
        (uint64_t)riff_size + 8u > (uint64_t)size)
        return QW_STATUS_ERR_FORMAT;
    riff_end = (size_t)riff_size + 8u;

    while (position < riff_end) {
        QwWebPChunk chunk;
        if (!webp_read_chunk(bytes, riff_end, position, &chunk))
            return QW_STATUS_ERR_FORMAT;
        if (seen_unknown && webp_is_known_chunk(chunk.type))
            return QW_STATUS_ERR_FORMAT;
        if (first_chunk) {
            first_chunk = 0;
            if (chunk.type == webp_fourcc("VP8X")) {
                if (chunk.size != 10u || (chunk.data[0] & 0xc1u) != 0u ||
                    chunk.data[1] || chunk.data[2] || chunk.data[3]) {
                    return QW_STATUS_ERR_FORMAT;
                }
                extended = 1;
                seen_vp8x = 1;
                extended_flags = chunk.data[0];
                canvas_width = webp_read_u24_le(chunk.data + 4u) + 1u;
                canvas_height = webp_read_u24_le(chunk.data + 7u) + 1u;
            } else if (chunk.type == webp_fourcc("VP8 ")) {
                if (!webp_vp8_dimensions(chunk.data, chunk.size,
                                        &simple_width, &simple_height)) {
                    return QW_STATUS_ERR_FORMAT;
                }
                seen_image = 1;
            } else if (chunk.type == webp_fourcc("VP8L")) {
                if (!webp_vp8l_dimensions(chunk.data, chunk.size,
                                          &simple_width, &simple_height,
                                          &image_has_alpha)) {
                    return QW_STATUS_ERR_FORMAT;
                }
                image_is_vp8l = 1;
                seen_image = 1;
                seen_alpha = image_has_alpha;
            } else {
                return QW_STATUS_ERR_FORMAT;
            }
        } else if (chunk.type == webp_fourcc("VP8X")) {
            return QW_STATUS_ERR_FORMAT;
        } else if (chunk.type == webp_fourcc("ICCP")) {
            if (seen_iccp || chunk.size == 0u || seen_image || seen_anim ||
                seen_alpha_chunk || seen_frame) return QW_STATUS_ERR_FORMAT;
            seen_iccp = 1;
        } else if (chunk.type == webp_fourcc("EXIF")) {
            if (seen_exif || chunk.size == 0u) return QW_STATUS_ERR_FORMAT;
            seen_exif = 1;
        } else if (chunk.type == webp_fourcc("XMP ")) {
            if (seen_xmp || chunk.size == 0u) return QW_STATUS_ERR_FORMAT;
            seen_xmp = 1;
        } else if (chunk.type == webp_fourcc("ALPH")) {
            if (seen_alpha_chunk || seen_image ||
                !webp_alpha_chunk_valid(&chunk))
                return QW_STATUS_ERR_FORMAT;
            seen_alpha_chunk = 1;
            seen_alpha = 1;
        } else if (chunk.type == webp_fourcc("VP8 ")) {
            uint32_t image_width;
            uint32_t image_height;
            if (seen_image ||
                !webp_vp8_dimensions(chunk.data, chunk.size,
                                     &image_width, &image_height)) {
                return QW_STATUS_ERR_FORMAT;
            }
            simple_width = image_width;
            simple_height = image_height;
            seen_image = 1;
        } else if (chunk.type == webp_fourcc("VP8L")) {
            uint32_t image_width;
            uint32_t image_height;
            int image_alpha;
            if (seen_image || seen_alpha_chunk ||
                !webp_vp8l_dimensions(chunk.data, chunk.size,
                                      &image_width, &image_height,
                                      &image_alpha)) {
                return QW_STATUS_ERR_FORMAT;
            }
            simple_width = image_width;
            simple_height = image_height;
            image_is_vp8l = 1;
            image_has_alpha = image_alpha;
            seen_image = 1;
            seen_alpha |= image_alpha;
        } else if (chunk.type == webp_fourcc("ANIM")) {
            if (seen_anim || chunk.size != 6u) return QW_STATUS_ERR_FORMAT;
            seen_anim = 1;
        } else if (chunk.type == webp_fourcc("ANMF")) {
            int this_frame_alpha;
            if (!seen_anim || frame_count >= QW_WEBP_MAX_FRAMES ||
                !webp_parse_frame(chunk.data, chunk.size,
                                  canvas_width, canvas_height,
                                  &this_frame_alpha, NULL)) {
                if (frame_count >= QW_WEBP_MAX_FRAMES)
                    return QW_STATUS_ERR_RESOURCE_LIMIT;
                return QW_STATUS_ERR_FORMAT;
            }
            ++frame_count;
            seen_frame = 1;
            frame_alpha |= this_frame_alpha;
        }
        if (!webp_is_known_chunk(chunk.type)) seen_unknown = 1;
        position = chunk.next;
    }

    if (position != riff_end) return QW_STATUS_ERR_FORMAT;
    if (!extended) {
        if (!seen_image || seen_anim || seen_frame || seen_alpha_chunk ||
            seen_iccp || seen_exif || seen_xmp || seen_unknown) {
            return QW_STATUS_ERR_FORMAT;
        }
        canvas_width = simple_width;
        canvas_height = simple_height;
    } else {
        int animation_flag = (extended_flags & 0x02u) != 0u;
        int alpha_flag = (extended_flags & 0x10u) != 0u;
        int iccp_flag = (extended_flags & 0x20u) != 0u;
        int exif_flag = (extended_flags & 0x08u) != 0u;
        int xmp_flag = (extended_flags & 0x04u) != 0u;
        if (!seen_vp8x || iccp_flag != seen_iccp || exif_flag != seen_exif ||
            xmp_flag != seen_xmp || alpha_flag != (seen_alpha || frame_alpha)) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (animation_flag) {
            if (!seen_anim || !seen_frame || seen_image) return QW_STATUS_ERR_FORMAT;
        } else if (seen_anim || seen_frame || !seen_image ||
                   simple_width != canvas_width ||
                   simple_height != canvas_height) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (seen_alpha_chunk && (image_is_vp8l || !seen_image))
            return QW_STATUS_ERR_FORMAT;
    }
    if (!qw_image_dimensions_within_limits(canvas_width, canvas_height))
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    *out_width = canvas_width;
    *out_height = canvas_height;
    *out_has_animation = seen_frame != 0;
    return QW_STATUS_OK;
}

void qw_webp_document_clear(QwWebPDocument *document)
{
    if (!document) return;
    free(document->frames);
    memset(document, 0, sizeof(*document));
}

int qw_webp_document_parse(const void *data,
                           size_t size,
                           QwWebPDocument *out_document)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t riff_size;
    uint32_t parsed_width;
    uint32_t parsed_height;
    int parsed_animation;
    size_t riff_end;
    size_t position = 12u;
    size_t frame_count = 0u;
    size_t frame_index = 0u;
    int rc;

    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    rc = qw_webp_parse(data, size, &parsed_width, &parsed_height,
                       &parsed_animation);
    if (rc != QW_STATUS_OK) return rc;
    riff_size = qw_read_u32_le(bytes + 4u);
    riff_end = (size_t)riff_size + 8u;
    out_document->width = parsed_width;
    out_document->height = parsed_height;
    out_document->has_animation = (uint8_t)(parsed_animation != 0);

    if (parsed_animation) {
        while (position < riff_end) {
            QwWebPChunk chunk;
            if (!webp_read_chunk(bytes, riff_end, position, &chunk)) {
                rc = QW_STATUS_ERR_FORMAT;
                goto fail;
            }
            if (chunk.type == webp_fourcc("ANMF")) ++frame_count;
            position = chunk.next;
        }
    } else {
        frame_count = 1u;
    }
    if (frame_count == 0u || frame_count > QW_WEBP_MAX_FRAMES) {
        rc = QW_STATUS_ERR_RESOURCE_LIMIT;
        goto fail;
    }
    out_document->frames = (QwWebPFrame *)calloc(
        frame_count, sizeof(*out_document->frames));
    if (!out_document->frames) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto fail;
    }
    out_document->frame_count = frame_count;

    position = 12u;
    while (position < riff_end) {
        QwWebPChunk chunk;
        if (!webp_read_chunk(bytes, riff_end, position, &chunk)) {
            rc = QW_STATUS_ERR_FORMAT;
            goto fail;
        }
        if (chunk.type == webp_fourcc("ANIM")) {
            out_document->background_argb = qw_read_u32_le(chunk.data);
            out_document->loop_count = qw_read_u16_le(chunk.data + 4u);
        } else if (chunk.type == webp_fourcc("ANMF")) {
            int frame_alpha;
            if (frame_index >= frame_count ||
                !webp_parse_frame(chunk.data, chunk.size,
                                  parsed_width, parsed_height,
                                  &frame_alpha,
                                  &out_document->frames[frame_index])) {
                rc = QW_STATUS_ERR_FORMAT;
                goto fail;
            }
            ++frame_index;
        } else if (!parsed_animation &&
                   chunk.type == webp_fourcc("ALPH")) {
            out_document->frames[0].alpha_data = chunk.data;
            out_document->frames[0].alpha_size = chunk.size;
            out_document->frames[0].has_alpha = 1u;
        } else if (!parsed_animation &&
                   chunk.type == webp_fourcc("VP8 ")) {
            QwWebPFrame *frame = &out_document->frames[0];
            frame->image_data = chunk.data;
            frame->image_size = chunk.size;
            frame->codec = QW_WEBP_CODEC_VP8;
        } else if (!parsed_animation &&
                   chunk.type == webp_fourcc("VP8L")) {
            uint32_t image_width;
            uint32_t image_height;
            int has_alpha;
            QwWebPFrame *frame = &out_document->frames[0];
            if (!webp_vp8l_dimensions(chunk.data, chunk.size,
                                     &image_width, &image_height,
                                     &has_alpha)) {
                rc = QW_STATUS_ERR_FORMAT;
                goto fail;
            }
            frame->image_data = chunk.data;
            frame->image_size = chunk.size;
            frame->codec = QW_WEBP_CODEC_VP8L;
            frame->has_alpha = (uint8_t)(has_alpha != 0);
        }
        position = chunk.next;
    }
    if (parsed_animation && frame_index != frame_count) {
        rc = QW_STATUS_ERR_FORMAT;
        goto fail;
    }
    if (!parsed_animation) {
        QwWebPFrame *frame = &out_document->frames[0];
        frame->width = parsed_width;
        frame->height = parsed_height;
        if (!frame->image_data ||
            (frame->codec != QW_WEBP_CODEC_VP8 &&
             frame->codec != QW_WEBP_CODEC_VP8L)) {
            rc = QW_STATUS_ERR_FORMAT;
            goto fail;
        }
    }
    return QW_STATUS_OK;

fail:
    qw_webp_document_clear(out_document);
    return rc;
}
