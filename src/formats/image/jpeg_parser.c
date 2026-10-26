#include "image_internal.h"

#include "../../core/byte_utils.h"

#include <stdint.h>
#include <string.h>

enum {
    JPEG_MARKER_TEM = 0x01,
    JPEG_MARKER_SOF0 = 0xc0,
    JPEG_MARKER_SOF2 = 0xc2,
    JPEG_MARKER_DHT = 0xc4,
    JPEG_MARKER_SOI = 0xd8,
    JPEG_MARKER_EOI = 0xd9,
    JPEG_MARKER_SOS = 0xda,
    JPEG_MARKER_DQT = 0xdb,
    JPEG_MARKER_DRI = 0xdd,
    JPEG_MARKER_COM = 0xfe
};

typedef struct QwJpegFrame {
    uint32_t width;
    uint32_t height;
    uint8_t component_ids[4];
    uint8_t component_count;
    uint8_t marker;
} QwJpegFrame;

static int jpeg_is_sof_marker(uint8_t marker)
{
    return marker >= 0xc0u && marker <= 0xcfu &&
           marker != JPEG_MARKER_DHT && marker != 0xc8u && marker != 0xccu;
}

int qw_jpeg_probe(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    return bytes && size >= 2u && bytes[0] == 0xffu &&
           bytes[1] == JPEG_MARKER_SOI;
}

static int jpeg_parse_quantization_tables(const uint8_t *segment,
                                          size_t segment_size)
{
    size_t position = 0u;
    if (!segment_size) return QW_STATUS_ERR_FORMAT;
    while (position < segment_size) {
        uint8_t spec = segment[position++];
        unsigned precision = (unsigned)(spec >> 4);
        unsigned table = (unsigned)(spec & 0x0fu);
        size_t value_size;
        if (precision > 1u || table > 3u) return QW_STATUS_ERR_UNSUPPORTED;
        value_size = 64u * (precision ? 2u : 1u);
        if (value_size > segment_size - position) return QW_STATUS_ERR_FORMAT;
        for (size_t i = 0u; i < value_size; i += precision ? 2u : 1u) {
            if (precision) {
                if (segment[position + i] == 0u &&
                    segment[position + i + 1u] == 0u) {
                    return QW_STATUS_ERR_FORMAT;
                }
            } else if (!segment[position + i]) {
                return QW_STATUS_ERR_FORMAT;
            }
        }
        position += value_size;
    }
    return position == segment_size ? QW_STATUS_OK : QW_STATUS_ERR_FORMAT;
}

static int jpeg_parse_huffman_tables(const uint8_t *segment,
                                     size_t segment_size)
{
    size_t position = 0u;
    if (!segment_size) return QW_STATUS_ERR_FORMAT;
    while (position < segment_size) {
        uint8_t spec = segment[position++];
        unsigned table_class = (unsigned)(spec >> 4);
        unsigned table = (unsigned)(spec & 0x0fu);
        unsigned symbol_count = 0u;
        int code_space = 1;
        if (table_class > 1u || table > 3u) return QW_STATUS_ERR_UNSUPPORTED;
        if (segment_size - position < 16u) return QW_STATUS_ERR_FORMAT;
        for (unsigned i = 0u; i < 16u; ++i) {
            unsigned count = segment[position + i];
            code_space = code_space * 2 - (int)count;
            if (code_space < 0) return QW_STATUS_ERR_FORMAT;
            symbol_count += count;
        }
        position += 16u;
        if (!symbol_count || symbol_count > 256u ||
            symbol_count > segment_size - position) {
            return QW_STATUS_ERR_FORMAT;
        }
        position += symbol_count;
    }
    return position == segment_size ? QW_STATUS_OK : QW_STATUS_ERR_FORMAT;
}

static int jpeg_parse_frame_header(uint8_t marker,
                                   const uint8_t *segment,
                                   size_t segment_size,
                                   QwJpegFrame *frame)
{
    uint8_t component_count;
    size_t expected_size;

    if (marker != JPEG_MARKER_SOF0 && marker != JPEG_MARKER_SOF2) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (segment_size < 6u) return QW_STATUS_ERR_FORMAT;
    if (segment[0] != 8u) return QW_STATUS_ERR_UNSUPPORTED;
    frame->height = qw_read_u16_be(segment + 1u);
    frame->width = qw_read_u16_be(segment + 3u);
    component_count = segment[5];
    if (component_count != 1u && component_count != 3u &&
        component_count != 4u) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    expected_size = 6u + (size_t)component_count * 3u;
    if (segment_size != expected_size) return QW_STATUS_ERR_FORMAT;
    if (!frame->width || !frame->height) return QW_STATUS_ERR_UNSUPPORTED;
    if (!qw_image_dimensions_within_limits(frame->width, frame->height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    frame->component_count = component_count;
    frame->marker = marker;
    for (unsigned i = 0u; i < component_count; ++i) {
        const uint8_t *component = segment + 6u + i * 3u;
        uint8_t sampling = component[1];
        uint8_t horizontal = (uint8_t)(sampling >> 4);
        uint8_t vertical = (uint8_t)(sampling & 0x0fu);
        if (!horizontal || horizontal > 4u || !vertical || vertical > 4u ||
            component[2] > 3u) {
            return QW_STATUS_ERR_FORMAT;
        }
        for (unsigned previous = 0u; previous < i; ++previous) {
            if (frame->component_ids[previous] == component[0]) {
                return QW_STATUS_ERR_FORMAT;
            }
        }
        frame->component_ids[i] = component[0];
    }
    return QW_STATUS_OK;
}

static int jpeg_parse_scan_header(const uint8_t *segment,
                                  size_t segment_size,
                                  const QwJpegFrame *frame)
{
    uint8_t scan_components;
    size_t expected_size;
    uint8_t spectral_start;
    uint8_t spectral_end;
    uint8_t successive;

    if (segment_size < 4u || !frame->component_count) return QW_STATUS_ERR_FORMAT;
    scan_components = segment[0];
    if (!scan_components || scan_components > frame->component_count) {
        return QW_STATUS_ERR_FORMAT;
    }
    expected_size = 4u + (size_t)scan_components * 2u;
    if (segment_size != expected_size) return QW_STATUS_ERR_FORMAT;
    for (unsigned i = 0u; i < scan_components; ++i) {
        const uint8_t *selector = segment + 1u + i * 2u;
        int known_component = 0;
        for (unsigned j = 0u; j < frame->component_count; ++j) {
            if (selector[0] == frame->component_ids[j]) {
                known_component = 1;
                break;
            }
        }
        if (!known_component || (selector[1] >> 4) > 3u ||
            (selector[1] & 0x0fu) > 3u) {
            return QW_STATUS_ERR_FORMAT;
        }
        for (unsigned previous = 0u; previous < i; ++previous) {
            if (selector[0] == segment[1u + previous * 2u]) {
                return QW_STATUS_ERR_FORMAT;
            }
        }
    }
    spectral_start = segment[1u + (size_t)scan_components * 2u];
    spectral_end = segment[2u + (size_t)scan_components * 2u];
    successive = segment[3u + (size_t)scan_components * 2u];
    if (frame->marker == JPEG_MARKER_SOF0) {
        return spectral_start == 0u && spectral_end == 63u &&
               successive == 0u ? QW_STATUS_OK : QW_STATUS_ERR_FORMAT;
    }
    if (spectral_start > spectral_end || spectral_end > 63u ||
        (spectral_start == 0u && spectral_end != 0u) ||
        (spectral_start > 0u && scan_components != 1u) ||
        (successive >> 4) > 13u || (successive & 0x0fu) > 13u ||
        ((successive >> 4) != 0u &&
         (successive >> 4) != (uint8_t)((successive & 0x0fu) + 1u))) {
        return QW_STATUS_ERR_FORMAT;
    }
    return QW_STATUS_OK;
}

static int jpeg_scan_to_marker(const uint8_t *bytes,
                               size_t size,
                               size_t *position)
{
    while (*position < size) {
        size_t marker_start;
        uint8_t marker;
        if (bytes[(*position)++] != 0xffu) continue;
        marker_start = *position - 1u;
        if (*position >= size) return QW_STATUS_ERR_FORMAT;
        while (*position < size && bytes[*position] == 0xffu) ++*position;
        if (*position >= size) return QW_STATUS_ERR_FORMAT;
        marker = bytes[(*position)++];
        if (marker == 0x00u || (marker >= 0xd0u && marker <= 0xd7u) ||
            marker == JPEG_MARKER_TEM) {
            continue;
        }
        *position = marker_start;
        return QW_STATUS_OK;
    }
    return QW_STATUS_ERR_FORMAT;
}

int qw_jpeg_parse(const void *data,
                  size_t size,
                  uint32_t *out_width,
                  uint32_t *out_height)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwJpegFrame frame;
    size_t position = 2u;
    int has_frame = 0;
    int has_scan = 0;

    if (!data || !out_width || !out_height) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_width = 0u;
    *out_height = 0u;
    if (!qw_jpeg_probe(data, size)) return QW_STATUS_ERR_FORMAT;
    memset(&frame, 0, sizeof(frame));

    while (position < size) {
        uint8_t marker;
        uint16_t segment_length;
        const uint8_t *segment;
        size_t segment_size;
        int rc;

        if (bytes[position++] != 0xffu) return QW_STATUS_ERR_FORMAT;
        while (position < size && bytes[position] == 0xffu) ++position;
        if (position >= size) return QW_STATUS_ERR_FORMAT;
        marker = bytes[position++];
        if (marker == 0x00u) return QW_STATUS_ERR_FORMAT;
        if (marker == JPEG_MARKER_EOI) {
            if (!has_frame || !has_scan) return QW_STATUS_ERR_FORMAT;
            *out_width = frame.width;
            *out_height = frame.height;
            return QW_STATUS_OK;
        }
        if (marker == JPEG_MARKER_SOI ||
            (marker >= 0xd0u && marker <= 0xd7u)) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (marker == JPEG_MARKER_TEM) continue;
        if (size - position < 2u) return QW_STATUS_ERR_FORMAT;
        segment_length = qw_read_u16_be(bytes + position);
        if (segment_length < 2u || segment_length > size - position) {
            return QW_STATUS_ERR_FORMAT;
        }
        segment = bytes + position + 2u;
        segment_size = (size_t)segment_length - 2u;
        position += segment_length;

        if (jpeg_is_sof_marker(marker)) {
            if (has_frame || has_scan) return QW_STATUS_ERR_FORMAT;
            rc = jpeg_parse_frame_header(marker, segment, segment_size,
                                         &frame);
            if (rc != QW_STATUS_OK) return rc;
            has_frame = 1;
        } else if (marker == JPEG_MARKER_DQT) {
            rc = jpeg_parse_quantization_tables(segment, segment_size);
            if (rc != QW_STATUS_OK) return rc;
        } else if (marker == JPEG_MARKER_DHT) {
            rc = jpeg_parse_huffman_tables(segment, segment_size);
            if (rc != QW_STATUS_OK) return rc;
        } else if (marker == JPEG_MARKER_DRI) {
            if (segment_length != 4u) return QW_STATUS_ERR_FORMAT;
        } else if (marker == JPEG_MARKER_SOS) {
            if (!has_frame) return QW_STATUS_ERR_FORMAT;
            rc = jpeg_parse_scan_header(segment, segment_size, &frame);
            if (rc != QW_STATUS_OK) return rc;
            has_scan = 1;
            rc = jpeg_scan_to_marker(bytes, size, &position);
            if (rc != QW_STATUS_OK) return rc;
        } else if ((marker >= 0xe0u && marker <= 0xefu) ||
                   marker == JPEG_MARKER_COM) {
            /* Application and comment payloads are opaque to the raster parser. */
        } else {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
    }
    return QW_STATUS_ERR_FORMAT;
}
