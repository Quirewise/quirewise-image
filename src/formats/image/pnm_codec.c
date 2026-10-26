#include "image_internal.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct QwPnmReader {
    const uint8_t *data;
    size_t size;
    size_t position;
} QwPnmReader;

typedef struct QwPamHeader {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t maxval;
    int has_width;
    int has_height;
    int has_depth;
    int has_maxval;
    int has_tuple;
    char tuple[32];
} QwPamHeader;


static int pnm_magic(const void *data, size_t size, int first, int last)
{
    const uint8_t *bytes = (const uint8_t *)data;
    return bytes && size >= 3u && bytes[0] == 'P' &&
           bytes[1] >= (uint8_t)('0' + first) &&
           bytes[1] <= (uint8_t)('0' + last) && qw_ascii_isspace(bytes[2]);
}

int qw_pnm_probe(const void *data, size_t size)
{
    return pnm_magic(data, size, 1, 6);
}

int qw_pam_probe(const void *data, size_t size)
{
    return pnm_magic(data, size, 7, 7);
}

static void pnm_skip_space_and_comments(QwPnmReader *reader)
{
    while (reader->position < reader->size) {
        uint8_t value = reader->data[reader->position];
        if (qw_ascii_isspace(value)) {
            ++reader->position;
            continue;
        }
        if (value == '#') {
            while (reader->position < reader->size &&
                   reader->data[reader->position] != '\n' &&
                   reader->data[reader->position] != '\r') {
                ++reader->position;
            }
            continue;
        }
        break;
    }
}

static int pnm_read_token(QwPnmReader *reader, char *token, size_t capacity)
{
    size_t start;
    size_t length;
    pnm_skip_space_and_comments(reader);
    start = reader->position;
    while (reader->position < reader->size &&
           !qw_ascii_isspace(reader->data[reader->position]) &&
           reader->data[reader->position] != '#') {
        ++reader->position;
    }
    length = reader->position - start;
    if (!length || length >= capacity) {
        return 0;
    }
    memcpy(token, reader->data + start, length);
    token[length] = '\0';
    return 1;
}

static int pnm_parse_u32(const char *text, uint32_t *out)
{
    uint64_t value = 0;
    const unsigned char *cursor = (const unsigned char *)text;
    if (!cursor || !*cursor || !out) {
        return 0;
    }
    for (; *cursor; ++cursor) {
        unsigned digit;
        if (*cursor < '0' || *cursor > '9') {
            return 0;
        }
        digit = (unsigned)(*cursor - '0');
        if (value > (UINT32_MAX - digit) / 10u) {
            return 0;
        }
        value = value * 10u + digit;
    }
    *out = (uint32_t)value;
    return 1;
}

static int pnm_consume_raster_separator(QwPnmReader *reader)
{
    uint8_t value;
    if (reader->position >= reader->size ||
        !qw_ascii_isspace(reader->data[reader->position])) {
        return 0;
    }
    value = reader->data[reader->position++];
    if (value == '\r' && reader->position < reader->size &&
        reader->data[reader->position] == '\n') {
        ++reader->position;
    }
    return 1;
}

static int pnm_read_line(QwPnmReader *reader, char *line, size_t capacity)
{
    size_t length = 0;
    int found_end = 0;
    while (reader->position < reader->size) {
        uint8_t value = reader->data[reader->position++];
        if (value == '\n' || value == '\r') {
            if (value == '\r' && reader->position < reader->size &&
                reader->data[reader->position] == '\n') {
                ++reader->position;
            }
            found_end = 1;
            break;
        }
        if (!value || length + 1u >= capacity) {
            return 0;
        }
        line[length++] = (char)value;
    }
    if (!found_end) {
        return 0;
    }
    line[length] = '\0';
    return 1;
}

static char *pnm_trim(char *text)
{
    char *end;
    while (*text && qw_ascii_isspace((uint8_t)*text)) {
        ++text;
    }
    end = text + strlen(text);
    while (end > text && qw_ascii_isspace((uint8_t)end[-1])) {
        *--end = '\0';
    }
    return text;
}

static int pnm_parse_pam_header(QwPnmReader *reader, QwPamHeader *header)
{
    char line[256];
    int found_end = 0;
    memset(header, 0, sizeof(*header));
    if (reader->position >= reader->size ||
        (reader->data[reader->position] != '\n' &&
         reader->data[reader->position] != '\r')) {
        return 0;
    }
    if (reader->data[reader->position++] == '\r' &&
        reader->position < reader->size &&
        reader->data[reader->position] == '\n') {
        ++reader->position;
    }
    while (reader->position < reader->size) {
        char *content;
        char *value;
        char *split;
        uint32_t number;
        if (!pnm_read_line(reader, line, sizeof(line))) {
            return 0;
        }
        content = pnm_trim(line);
        if (!*content || *content == '#') {
            continue;
        }
        split = content;
        while (*split && !qw_ascii_isspace((uint8_t)*split)) {
            ++split;
        }
        if (*split) {
            *split++ = '\0';
        }
        value = pnm_trim(split);
        if (strcmp(content, "ENDHDR") == 0) {
            if (*value) {
                return 0;
            }
            found_end = 1;
            break;
        }
        if (strcmp(content, "WIDTH") == 0 ||
            strcmp(content, "HEIGHT") == 0 ||
            strcmp(content, "DEPTH") == 0 ||
            strcmp(content, "MAXVAL") == 0) {
            int *seen;
            uint32_t *destination;
            if (!pnm_parse_u32(value, &number)) {
                return 0;
            }
            if (strcmp(content, "WIDTH") == 0) {
                seen = &header->has_width;
                destination = &header->width;
            } else if (strcmp(content, "HEIGHT") == 0) {
                seen = &header->has_height;
                destination = &header->height;
            } else if (strcmp(content, "DEPTH") == 0) {
                seen = &header->has_depth;
                destination = &header->depth;
            } else {
                seen = &header->has_maxval;
                destination = &header->maxval;
            }
            if (*seen || *value == '\0') {
                return 0;
            }
            *seen = 1;
            *destination = number;
        } else if (strcmp(content, "TUPLTYPE") == 0) {
            size_t length = strlen(value);
            if (header->has_tuple || !length || length >= sizeof(header->tuple)) {
                return 0;
            }
            memcpy(header->tuple, value, length + 1u);
            header->has_tuple = 1;
        }
    }
    return found_end && header->has_width && header->has_height &&
           header->has_depth && header->has_maxval;
}

static uint8_t pnm_scale_sample(uint32_t sample, uint32_t maxval)
{
    return (uint8_t)(((uint64_t)sample * 255u + maxval / 2u) / maxval);
}

static int pnm_read_sample(QwPnmReader *reader, int ascii,
                           uint32_t maxval, uint32_t *sample)
{
    if (ascii) {
        char token[32];
        if (!pnm_read_token(reader, token, sizeof(token)) ||
            !pnm_parse_u32(token, sample)) {
            return 0;
        }
    } else if (maxval < 256u) {
        if (reader->position >= reader->size) {
            return 0;
        }
        *sample = reader->data[reader->position++];
    } else {
        if (reader->size - reader->position < 2u) {
            return 0;
        }
        *sample = ((uint32_t)reader->data[reader->position] << 8) |
                  reader->data[reader->position + 1u];
        reader->position += 2u;
    }
    return *sample <= maxval;
}

static int pnm_decode_page_internal(const void *data, size_t size,
                                    QwPageImage *out_page)
{
    QwPnmReader reader;
    QwPamHeader pam;
    char token[32];
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t maxval = 1;
    uint32_t source_depth = 0;
    size_t pixel_count;
    size_t output_channels;
    size_t output_size;
    size_t raw_sample_count;
    size_t expected_raw_size;
    int magic;
    int ascii;
    int pbm;
    int has_alpha = 0;
    int gray = 0;
    int tuple_supported = 1;
    uint8_t *pixels = NULL;
    int rc = QW_STATUS_ERR_FORMAT;

    if (!data || !size || !out_page) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    memset(out_page, 0, sizeof(*out_page));
    reader.data = (const uint8_t *)data;
    reader.size = size;
    reader.position = 0;
    if (!pnm_read_token(&reader, token, sizeof(token)) || token[0] != 'P' ||
        token[1] < '1' || token[1] > '7' || token[2] != '\0') {
        return QW_STATUS_ERR_FORMAT;
    }
    magic = token[1] - '0';
    if (magic == 7) {
        if (!pnm_parse_pam_header(&reader, &pam)) {
            return QW_STATUS_ERR_FORMAT;
        }
        width = pam.width;
        height = pam.height;
        source_depth = pam.depth;
        maxval = pam.maxval;
        if (source_depth == 1u) {
            gray = 1;
        } else if (source_depth == 2u) {
            gray = 1;
            has_alpha = 1;
        } else if (source_depth == 3u) {
            gray = 0;
        } else if (source_depth == 4u) {
            gray = 0;
            has_alpha = 1;
        } else {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if (pam.has_tuple) {
            tuple_supported =
                ((strcmp(pam.tuple, "BLACKANDWHITE") == 0 && source_depth == 1u && maxval == 1u) ||
                 (strcmp(pam.tuple, "GRAYSCALE") == 0 && source_depth == 1u) ||
                 (strcmp(pam.tuple, "BLACKANDWHITE_ALPHA") == 0 && source_depth == 2u && maxval == 1u) ||
                 (strcmp(pam.tuple, "GRAYSCALE_ALPHA") == 0 && source_depth == 2u) ||
                 (strcmp(pam.tuple, "RGB") == 0 && source_depth == 3u) ||
                 (strcmp(pam.tuple, "RGB_ALPHA") == 0 && source_depth == 4u));
        }
    } else {
        if (!pnm_read_token(&reader, token, sizeof(token)) ||
            !pnm_parse_u32(token, &width) ||
            !pnm_read_token(&reader, token, sizeof(token)) ||
            !pnm_parse_u32(token, &height)) {
            return QW_STATUS_ERR_FORMAT;
        }
        pbm = magic == 1 || magic == 4;
        ascii = magic <= 3;
        if (magic == 2 || magic == 3 || magic == 5 || magic == 6) {
            if (!pnm_read_token(&reader, token, sizeof(token)) ||
                !pnm_parse_u32(token, &maxval)) {
                return QW_STATUS_ERR_FORMAT;
            }
        }
        source_depth = (magic == 3 || magic == 6) ? 3u : 1u;
        gray = source_depth == 1u;
        if (!ascii && !pnm_consume_raster_separator(&reader)) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (pbm) {
            maxval = 1u;
        }
    }
    if (!tuple_supported) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (!width || !height || !source_depth || !maxval || maxval > 65535u ||
        source_depth > 4u) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (!qw_image_dimensions_within_limits(width, height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (!qw_checked_size_mul((size_t)width, (size_t)height, &pixel_count)) {
        return QW_STATUS_ERR_FORMAT;
    }
    output_channels = gray ? 1u : 3u;
    if (!qw_checked_size_mul(pixel_count, output_channels, &output_size)) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (magic == 4) {
        size_t row_bytes = (size_t)(width / 8u) + (width % 8u ? 1u : 0u);
        if (!qw_checked_size_mul(row_bytes, (size_t)height, &expected_raw_size) ||
            size - reader.position != expected_raw_size) {
            return QW_STATUS_ERR_FORMAT;
        }
    } else if (magic >= 5) {
        size_t bytes_per_sample = maxval < 256u ? 1u : 2u;
        if (!qw_checked_size_mul(pixel_count, source_depth, &raw_sample_count) ||
            !qw_checked_size_mul(raw_sample_count, bytes_per_sample, &expected_raw_size) ||
            size - reader.position != expected_raw_size) {
            return QW_STATUS_ERR_FORMAT;
        }
    }
    pixels = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!pixels) {
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    if (magic == 4) {
        size_t row_bytes = (size_t)(width / 8u) + (width % 8u ? 1u : 0u);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                size_t byte_index = reader.position + (size_t)y * row_bytes + x / 8u;
                unsigned bit = (reader.data[byte_index] >> (7u - (x & 7u))) & 1u;
                pixels[(size_t)y * width + x] = bit ? 0u : 255u;
            }
        }
        reader.position = size;
    } else {
        for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
            uint32_t color[3] = {0, 0, 0};
            uint32_t alpha = maxval;
            for (uint32_t channel = 0; channel < source_depth; ++channel) {
                uint32_t sample;
                int sample_ascii = magic == 1 || magic == 2 || magic == 3;
                if (!pnm_read_sample(&reader, sample_ascii, maxval, &sample)) {
                    free(pixels);
                    return QW_STATUS_ERR_FORMAT;
                }
                if (magic == 1) {
                    if (sample > 1u) {
                        free(pixels);
                        return QW_STATUS_ERR_FORMAT;
                    }
                    color[0] = sample ? 0u : 1u;
                } else if (has_alpha && channel + 1u == source_depth) {
                    alpha = sample;
                } else if (gray) {
                    color[0] = sample;
                } else {
                    color[channel] = sample;
                }
            }
            if (magic == 1) {
                pixels[pixel] = color[0] ? 255u : 0u;
            } else if (gray) {
                uint32_t sample = color[0];
                if (has_alpha) {
                    sample = (uint32_t)(((uint64_t)sample * alpha +
                                         (uint64_t)maxval * (maxval - alpha) +
                                         maxval / 2u) / maxval);
                }
                pixels[pixel] = pnm_scale_sample(sample, maxval);
            } else {
                for (size_t channel = 0; channel < 3u; ++channel) {
                    uint32_t sample = color[channel];
                    if (has_alpha) {
                        sample = (uint32_t)(((uint64_t)sample * alpha +
                                             (uint64_t)maxval * (maxval - alpha) +
                                             maxval / 2u) / maxval);
                    }
                    pixels[pixel * 3u + channel] = pnm_scale_sample(sample, maxval);
                }
            }
        }
        if (magic <= 3) {
            pnm_skip_space_and_comments(&reader);
            if (reader.position != size) {
                free(pixels);
                return QW_STATUS_ERR_FORMAT;
            }
        }
    }
    out_page->data = pixels;
    out_page->size = output_size;
    out_page->kind = output_channels == 1u
        ? QW_PAGE_IMAGE_GRAY8 : QW_PAGE_IMAGE_RGB24;
    out_page->width = width;
    out_page->height = height;
    rc = QW_STATUS_OK;
    return rc;
}

int qw_pnm_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    if (qw_pnm_probe(data, size) || qw_pam_probe(data, size)) {
        return pnm_decode_page_internal(data, size, out_page);
    }
    return QW_STATUS_ERR_UNSUPPORTED;
}

int qw_pnm_encode_page(const QwPageImage *page, int pam,
                       uint8_t **out_data, size_t *out_size)
{
    size_t channels;
    size_t pixel_count;
    size_t source_size;
    size_t raster_size;
    size_t header_capacity;
    size_t header_size;
    size_t total_size;
    uint8_t *encoded;
    int gray;
    if (!page || !out_data || !out_size || !page->data ||
        !page->width || !page->height ||
        (page->kind != QW_PAGE_IMAGE_GRAY8 && page->kind != QW_PAGE_IMAGE_RGB24)) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_data = NULL;
    *out_size = 0;
    gray = page->kind == QW_PAGE_IMAGE_GRAY8;
    if (!qw_checked_size_mul(page->width, page->height, &pixel_count) ||
        !qw_checked_size_mul(pixel_count, gray ? 1u : 3u, &source_size) ||
        page->size != source_size) {
        return QW_STATUS_ERR_FORMAT;
    }
    channels = pam ? (gray ? 1u : 3u) : 3u;
    if (
        !qw_checked_size_mul(pixel_count, channels, &raster_size) ||
        raster_size > SIZE_MAX - 160u) {
        return QW_STATUS_ERR_FORMAT;
    }
    header_capacity = 160u;
    total_size = header_capacity + raster_size;
    encoded = (uint8_t *)malloc(total_size);
    if (!encoded) {
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    if (pam) {
        int length = snprintf((char *)encoded, header_capacity,
                              gray
                                  ? "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 1\nMAXVAL 255\nTUPLTYPE GRAYSCALE\nENDHDR\n"
                                  : "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 3\nMAXVAL 255\nTUPLTYPE RGB\nENDHDR\n",
                              (unsigned)page->width, (unsigned)page->height);
        if (length < 0 || (size_t)length >= header_capacity) {
            free(encoded);
            return QW_STATUS_ERR_FORMAT;
        }
        header_size = (size_t)length;
    } else {
        int length = snprintf((char *)encoded, header_capacity,
                              "P6\n%u %u\n255\n",
                              (unsigned)page->width, (unsigned)page->height);
        if (length < 0 || (size_t)length >= header_capacity) {
            free(encoded);
            return QW_STATUS_ERR_FORMAT;
        }
        header_size = (size_t)length;
    }
    total_size = header_size + raster_size;
    if (gray && !pam) {
        for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
            uint8_t value = ((const uint8_t *)page->data)[pixel];
            encoded[header_size + pixel * 3u] = value;
            encoded[header_size + pixel * 3u + 1u] = value;
            encoded[header_size + pixel * 3u + 2u] = value;
        }
    } else {
        memcpy(encoded + header_size, page->data, raster_size);
    }
    *out_data = encoded;
    *out_size = total_size;
    return QW_STATUS_OK;
}
