#include "image_internal.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DCM_PREAMBLE_SIZE 132u
#define DCM_MAX_PIXELS 20000000u

typedef struct QwDcmElement {
    uint16_t group;
    uint16_t element;
    uint8_t vr[2];
    uint32_t length;
    size_t value_offset;
} QwDcmElement;

typedef struct QwDcmHeader {
    uint32_t width;
    uint32_t height;
    uint32_t samples_per_pixel;
    uint32_t planar_configuration;
    uint32_t bits_allocated;
    uint32_t bits_stored;
    uint32_t high_bit;
    uint32_t pixel_representation;
    size_t pixel_count;
    size_t pixel_offset;
    size_t pixel_length;
    int photometric;
} QwDcmHeader;

enum {
    DCM_PHOTOMETRIC_MONOCHROME1 = 1,
    DCM_PHOTOMETRIC_MONOCHROME2 = 2,
    DCM_PHOTOMETRIC_RGB = 3
};



static int dcm_vr_is_long(const uint8_t vr[2])
{
    static const char *const long_vrs[] = {
        "OB", "OD", "OF", "OL", "OV", "OW", "SQ", "UC", "UR", "UT", "UN"
    };
    for (size_t i = 0u; i < sizeof(long_vrs) / sizeof(long_vrs[0]); ++i) {
        if (vr[0] == (uint8_t)long_vrs[i][0] &&
            vr[1] == (uint8_t)long_vrs[i][1]) {
            return 1;
        }
    }
    return 0;
}

static int dcm_read_element(const uint8_t *bytes,
                            size_t size,
                            size_t *position,
                            QwDcmElement *element)
{
    size_t value_offset;
    uint32_t length;
    if (!bytes || !position || !element || *position > size ||
        size - *position < 8u) {
        return 0;
    }
    element->group = qw_read_u16_le(bytes + *position);
    element->element = qw_read_u16_le(bytes + *position + 2u);
    element->vr[0] = bytes[*position + 4u];
    element->vr[1] = bytes[*position + 5u];
    if (element->vr[0] < 'A' || element->vr[0] > 'Z' ||
        element->vr[1] < 'A' || element->vr[1] > 'Z') {
        return 0;
    }
    if (dcm_vr_is_long(element->vr)) {
        if (size - *position < 12u || bytes[*position + 6u] != 0u ||
            bytes[*position + 7u] != 0u) {
            return 0;
        }
        length = qw_read_u32_le(bytes + *position + 8u);
        value_offset = *position + 12u;
    } else {
        length = qw_read_u16_le(bytes + *position + 6u);
        value_offset = *position + 8u;
    }
    if (length == UINT32_MAX || value_offset > size ||
        (size_t)length > size - value_offset) {
        return 0;
    }
    element->length = length;
    element->value_offset = value_offset;
    *position = value_offset + length;
    return 1;
}

static int dcm_element_is(const QwDcmElement *element,
                          uint16_t group,
                          uint16_t tag,
                          const char *vr)
{
    return element && element->group == group && element->element == tag &&
           element->vr[0] == (uint8_t)vr[0] &&
           element->vr[1] == (uint8_t)vr[1];
}

static int dcm_read_us(const uint8_t *bytes,
                       const QwDcmElement *element,
                       uint32_t *out_value)
{
    if (!bytes || !element || !out_value || element->length != 2u ||
        (element->vr[0] != 'U' && element->vr[0] != 'S') ||
        element->vr[1] != 'S') {
        return 0;
    }
    *out_value = qw_read_u16_le(bytes + element->value_offset);
    return 1;
}

static int dcm_parse_frame_count(const uint8_t *bytes,
                                 const QwDcmElement *element,
                                 uint32_t *out_count)
{
    uint32_t count = 0u;
    size_t i = 0u;
    if (!bytes || !element || !out_count || element->vr[0] != 'I' ||
        element->vr[1] != 'S' || !element->length || element->length > 16u) {
        return 0;
    }
    while (i < element->length && bytes[element->value_offset + i] == ' ') ++i;
    for (; i < element->length && bytes[element->value_offset + i] >= '0' &&
           bytes[element->value_offset + i] <= '9'; ++i) {
        count = count * 10u + (bytes[element->value_offset + i] - '0');
        if (count > 1u) return 0;
    }
    while (i < element->length &&
           (bytes[element->value_offset + i] == ' ' ||
            bytes[element->value_offset + i] == 0u)) ++i;
    if (i != element->length || count != 1u) return 0;
    *out_count = count;
    return 1;
}

static int dcm_parse_photometric(const uint8_t *bytes,
                                 const QwDcmElement *element,
                                 int *out_photometric)
{
    size_t length;
    const uint8_t *value;
    if (!bytes || !element || !out_photometric ||
        element->vr[0] != 'C' || element->vr[1] != 'S' ||
        !element->length || element->length > 32u) {
        return 0;
    }
    value = bytes + element->value_offset;
    length = element->length;
    while (length && (value[length - 1u] == ' ' || value[length - 1u] == 0u)) {
        --length;
    }
    if (length == 11u && memcmp(value, "MONOCHROME1", 11u) == 0) {
        *out_photometric = DCM_PHOTOMETRIC_MONOCHROME1;
    } else if (length == 11u && memcmp(value, "MONOCHROME2", 11u) == 0) {
        *out_photometric = DCM_PHOTOMETRIC_MONOCHROME2;
    } else if (length == 3u && memcmp(value, "RGB", 3u) == 0) {
        *out_photometric = DCM_PHOTOMETRIC_RGB;
    } else {
        return 0;
    }
    return 1;
}

static int dcm_read_header(const void *data, size_t size, QwDcmHeader *header)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t position = DCM_PREAMBLE_SIZE;
    int have_transfer_syntax = 0;
    int have_rows = 0;
    int have_columns = 0;
    int have_pixel_data = 0;
    uint32_t frame_count = 1u;
    uint32_t transfer_syntax_length = 0u;
    char transfer_syntax[40];

    if (!bytes || !header || size < DCM_PREAMBLE_SIZE ||
        memcmp(bytes + 128u, "DICM", 4u) != 0) {
        return 0;
    }
    memset(header, 0, sizeof(*header));
    transfer_syntax[0] = '\0';
    while (position < size) {
        QwDcmElement element;
        size_t element_start = position;
        if (!dcm_read_element(bytes, size, &position, &element)) return 0;
        if (element.group != 0x0002u) {
            position = element_start;
            break;
        }
        if (dcm_element_is(&element, 0x0002u, 0x0010u, "UI")) {
            size_t length = element.length;
            while (length && (bytes[element.value_offset + length - 1u] == 0u ||
                              bytes[element.value_offset + length - 1u] == ' ')) {
                --length;
            }
            if (!length || length >= sizeof(transfer_syntax)) return 0;
            memcpy(transfer_syntax, bytes + element.value_offset, length);
            transfer_syntax[length] = '\0';
            transfer_syntax_length = (uint32_t)length;
            have_transfer_syntax = 1;
        }
    }
    if (!have_transfer_syntax || transfer_syntax_length != 19u ||
        memcmp(transfer_syntax, "1.2.840.10008.1.2.1", 19u) != 0) {
        return 0;
    }

    while (position < size) {
        QwDcmElement element;
        if (!dcm_read_element(bytes, size, &position, &element)) return 0;
        if (dcm_element_is(&element, 0x0028u, 0x0002u, "US")) {
            if (!dcm_read_us(bytes, &element, &header->samples_per_pixel)) return 0;
        } else if (dcm_element_is(&element, 0x0028u, 0x0004u, "CS")) {
            if (!dcm_parse_photometric(bytes, &element, &header->photometric)) return 0;
        } else if (dcm_element_is(&element, 0x0028u, 0x0006u, "US")) {
            if (!dcm_read_us(bytes, &element, &header->planar_configuration)) return 0;
        } else if (dcm_element_is(&element, 0x0028u, 0x0008u, "IS")) {
            if (!dcm_parse_frame_count(bytes, &element, &frame_count)) return 0;
        } else if (dcm_element_is(&element, 0x0028u, 0x0010u, "US")) {
            if (!dcm_read_us(bytes, &element, &header->height)) return 0;
            have_rows = 1;
        } else if (dcm_element_is(&element, 0x0028u, 0x0011u, "US")) {
            if (!dcm_read_us(bytes, &element, &header->width)) return 0;
            have_columns = 1;
        } else if (dcm_element_is(&element, 0x0028u, 0x0100u, "US")) {
            if (!dcm_read_us(bytes, &element, &header->bits_allocated)) return 0;
        } else if (dcm_element_is(&element, 0x0028u, 0x0101u, "US")) {
            if (!dcm_read_us(bytes, &element, &header->bits_stored)) return 0;
        } else if (dcm_element_is(&element, 0x0028u, 0x0102u, "US")) {
            if (!dcm_read_us(bytes, &element, &header->high_bit)) return 0;
        } else if (dcm_element_is(&element, 0x0028u, 0x0103u, "US")) {
            if (!dcm_read_us(bytes, &element, &header->pixel_representation)) return 0;
        } else if (element.group == 0x7fe0u && element.element == 0x0010u) {
            if ((element.vr[0] != 'O' ||
                 (element.vr[1] != 'B' && element.vr[1] != 'W')) ||
                !element.length) {
                return 0;
            }
            header->pixel_offset = element.value_offset;
            header->pixel_length = element.length;
            have_pixel_data = 1;
            break;
        }
    }
    if (!have_rows || !have_columns || !have_pixel_data || frame_count != 1u ||
        !header->width || !header->height || !header->samples_per_pixel ||
        (header->samples_per_pixel != 1u && header->samples_per_pixel != 3u) ||
        (header->bits_allocated != 8u && header->bits_allocated != 16u) ||
        !header->bits_stored || header->bits_stored > header->bits_allocated ||
        header->high_bit + 1u < header->bits_stored ||
        header->high_bit >= header->bits_allocated ||
        header->pixel_representation != 0u ||
        (header->samples_per_pixel == 1u &&
         header->photometric != DCM_PHOTOMETRIC_MONOCHROME1 &&
         header->photometric != DCM_PHOTOMETRIC_MONOCHROME2) ||
        (header->samples_per_pixel == 3u &&
         (header->photometric != DCM_PHOTOMETRIC_RGB ||
          header->planar_configuration > 1u))) {
        return 0;
    }
    {
        uint64_t pixel_count = (uint64_t)header->width * header->height;
        uint64_t sample_count = pixel_count * header->samples_per_pixel;
        uint64_t expected_bytes = sample_count * (header->bits_allocated / 8u);
        if (!pixel_count || pixel_count > DCM_MAX_PIXELS ||
            pixel_count > SIZE_MAX / 3u || expected_bytes > SIZE_MAX ||
            header->pixel_length != expected_bytes + (expected_bytes & 1u)) {
            return 0;
        }
        header->pixel_count = (size_t)pixel_count;
    }
    return 1;
}

int qw_dcm_probe(const void *data, size_t size)
{
    QwDcmHeader header;
    return dcm_read_header(data, size, &header);
}

int qw_dcm_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwDcmHeader header;
    uint8_t *pixels;
    size_t output_size;
    uint32_t shift;
    uint32_t maximum;
    if (!out_page || !dcm_read_header(data, size, &header)) {
        return QW_STATUS_ERR_FORMAT;
    }
    memset(out_page, 0, sizeof(*out_page));
    output_size = header.pixel_count * 3u;
    pixels = (uint8_t *)malloc(output_size);
    if (!pixels) return QW_STATUS_ERR_OUT_OF_MEMORY;
    shift = header.high_bit + 1u - header.bits_stored;
    maximum = header.bits_stored == 16u ? 65535u :
              ((1u << header.bits_stored) - 1u);
    for (size_t i = 0u; i < header.pixel_count; ++i) {
        if (header.samples_per_pixel == 1u) {
            const uint8_t *sample = bytes + header.pixel_offset +
                                    i * (header.bits_allocated / 8u);
            uint32_t value = header.bits_allocated == 8u
                                 ? sample[0] : qw_read_u16_le(sample);
            uint8_t gray;
            value = (value >> shift) & maximum;
            gray = (uint8_t)((value * 255u + maximum / 2u) / maximum);
            if (header.photometric == DCM_PHOTOMETRIC_MONOCHROME1) {
                gray = (uint8_t)(255u - gray);
            }
            pixels[i * 3u] = pixels[i * 3u + 1u] = pixels[i * 3u + 2u] = gray;
        } else {
            for (size_t channel = 0u; channel < 3u; ++channel) {
                size_t sample_index = header.planar_configuration == 0u
                                          ? i * 3u + channel
                                          : channel * header.pixel_count + i;
                const uint8_t *sample = bytes + header.pixel_offset +
                    sample_index * (header.bits_allocated / 8u);
                uint32_t value = header.bits_allocated == 8u
                                     ? sample[0] : qw_read_u16_le(sample);
                value = (value >> shift) & maximum;
                pixels[i * 3u + channel] = (uint8_t)(
                    (value * 255u + maximum / 2u) / maximum);
            }
        }
    }
    out_page->data = pixels;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = header.width;
    out_page->height = header.height;
    return QW_STATUS_OK;
}
