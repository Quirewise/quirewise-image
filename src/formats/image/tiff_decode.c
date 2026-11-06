#include "tiff_decode.h"
#include "image_internal.h"

#include "quirewise/status.h"

#include <quirewise/compression.h>

#include "stb_image.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TIFF_TAG_IMAGE_WIDTH = 256,
    TIFF_TAG_IMAGE_LENGTH = 257,
    TIFF_TAG_BITS_PER_SAMPLE = 258,
    TIFF_TAG_COMPRESSION = 259,
    TIFF_TAG_PHOTOMETRIC = 262,
    TIFF_TAG_FILL_ORDER = 266,
    TIFF_TAG_ORIENTATION = 274,
    TIFF_TAG_SAMPLES_PER_PIXEL = 277,
    TIFF_TAG_ROWS_PER_STRIP = 278,
    TIFF_TAG_STRIP_OFFSETS = 273,
    TIFF_TAG_STRIP_BYTE_COUNTS = 279,
    TIFF_TAG_PLANAR_CONFIGURATION = 284,
    TIFF_TAG_PREDICTOR = 317,
    TIFF_TAG_COLOR_MAP = 320,
    TIFF_TAG_T6_OPTIONS = 293,
    TIFF_TAG_EXTRA_SAMPLES = 338,
    TIFF_TAG_SAMPLE_FORMAT = 339,
    TIFF_TAG_JPEG_TABLES = 347
};

enum {
    TIFF_COMPRESSION_NONE = 1,
    TIFF_COMPRESSION_CCITT_T6 = 4,
    TIFF_COMPRESSION_LZW = 5,
    TIFF_COMPRESSION_JPEG_OLD = 6,
    TIFF_COMPRESSION_JPEG = 7,
    TIFF_COMPRESSION_DEFLATE = 8,
    TIFF_COMPRESSION_DEFLATE_OLD = 32946,
    TIFF_COMPRESSION_PACKBITS = 32773
};

enum {
    TIFF_PHOTOMETRIC_WHITE_IS_ZERO = 0,
    TIFF_PHOTOMETRIC_BLACK_IS_ZERO = 1,
    TIFF_PHOTOMETRIC_RGB = 2,
    TIFF_PHOTOMETRIC_PALETTE = 3,
    TIFF_PHOTOMETRIC_SEPARATED = 5,
    TIFF_PHOTOMETRIC_YCBCR = 6
};

typedef struct QwTiffFile {
    const uint8_t *data;
    size_t size;
    uint64_t first_ifd;
    int big_tiff;
    int big_endian;
} QwTiffFile;

typedef struct QwTiffDirectory {
    uint64_t entry_count;
    size_t entries_offset;
    uint64_t next_ifd;
} QwTiffDirectory;

typedef struct QwTiffField {
    uint16_t tag;
    uint16_t type;
    uint64_t count;
    size_t value_offset;
    int inline_value;
} QwTiffField;

typedef struct QwTiffImageInfo {
    uint32_t width;
    uint32_t height;
    uint32_t rows_per_strip;
    uint16_t bits_per_sample;
    uint16_t samples_per_pixel;
    uint16_t compression;
    uint16_t photometric;
    uint16_t orientation;
    uint16_t fill_order;
    uint16_t planar_configuration;
    uint16_t predictor;
    uint16_t extra_sample_type;
    uint16_t sample_format;
    uint16_t base_samples;
    size_t source_row_bytes;
    size_t output_pixel_count;
    size_t output_size;
    int output_channels;
    int output_photometric;
    QwTiffField strip_offsets_field;
    QwTiffField strip_byte_counts_field;
    QwTiffField color_map_field;
    QwTiffField jpeg_tables_field;
    const QwTiffField *strip_offsets;
    const QwTiffField *strip_byte_counts;
    const QwTiffField *color_map;
    const QwTiffField *jpeg_tables;
} QwTiffImageInfo;

static int tiff_range_ok(const QwTiffFile *file, uint64_t offset, uint64_t length)
{
    return offset <= file->size && length <= (uint64_t)file->size - offset;
}

static uint64_t tiff_read_uint(const QwTiffFile *file,
                               size_t offset,
                               unsigned length)
{
    uint64_t value = 0u;
    unsigned i;
    if (length > 8u || !tiff_range_ok(file, offset, length)) return 0u;
    if (file->big_endian) {
        for (i = 0u; i < length; ++i) {
            value = (value << 8) | file->data[offset + i];
        }
    } else {
        for (i = 0u; i < length; ++i) {
            value |= (uint64_t)file->data[offset + i] << (i * 8u);
        }
    }
    return value;
}

static int tiff_parse_header(QwTiffFile *file,
                             const uint8_t *data,
                             size_t size)
{
    uint16_t magic;
    if (!file || !data || size < 8u) return 0;
    memset(file, 0, sizeof(*file));
    file->data = data;
    file->size = size;
    if (data[0] == 'I' && data[1] == 'I') file->big_endian = 0;
    else if (data[0] == 'M' && data[1] == 'M') file->big_endian = 1;
    else return 0;
    magic = (uint16_t)tiff_read_uint(file, 2u, 2u);
    if (magic == 42u) {
        file->first_ifd = tiff_read_uint(file, 4u, 4u);
    } else if (magic == 43u) {
        if (size < 16u || tiff_read_uint(file, 4u, 2u) != 8u ||
            tiff_read_uint(file, 6u, 2u) != 0u) {
            return 0;
        }
        file->big_tiff = 1;
        file->first_ifd = tiff_read_uint(file, 8u, 8u);
    } else {
        return 0;
    }
    return file->first_ifd != 0u &&
           tiff_range_ok(file, file->first_ifd,
                         file->big_tiff ? 8u : 2u);
}

static int tiff_parse_directory(const QwTiffFile *file,
                                uint64_t offset,
                                QwTiffDirectory *directory)
{
    uint64_t count;
    uint64_t entries_size;
    uint64_t total_size;
    unsigned count_size = file->big_tiff ? 8u : 2u;
    unsigned entry_size = file->big_tiff ? 20u : 12u;
    unsigned next_size = file->big_tiff ? 8u : 4u;
    if (!directory || !offset || !tiff_range_ok(file, offset, count_size)) {
        return 0;
    }
    count = tiff_read_uint(file, (size_t)offset, count_size);
    if (count > (UINT64_MAX - count_size - next_size) / entry_size) return 0;
    entries_size = count * entry_size;
    total_size = count_size + entries_size + next_size;
    if (!tiff_range_ok(file, offset, total_size) ||
        offset + count_size > SIZE_MAX) {
        return 0;
    }
    directory->entry_count = count;
    directory->entries_offset = (size_t)(offset + count_size);
    directory->next_ifd = tiff_read_uint(
        file, (size_t)(offset + count_size + entries_size), next_size);
    if (directory->next_ifd &&
        !tiff_range_ok(file, directory->next_ifd, count_size)) {
        return 0;
    }
    return 1;
}

static unsigned tiff_type_size(uint16_t type)
{
    switch (type) {
    case 1: case 2: case 6: case 7: return 1u;
    case 3: case 8: return 2u;
    case 4: case 9: case 11: case 13: return 4u;
    case 5: case 10: case 12: case 16: case 17: case 18: return 8u;
    default: return 0u;
    }
}

static int tiff_find_field(const QwTiffFile *file,
                           const QwTiffDirectory *directory,
                           uint16_t wanted_tag,
                           QwTiffField *field)
{
    uint64_t index;
    unsigned entry_size = file->big_tiff ? 20u : 12u;
    unsigned count_size = file->big_tiff ? 8u : 4u;
    unsigned value_size = file->big_tiff ? 8u : 4u;
    unsigned inline_size = file->big_tiff ? 8u : 4u;
    for (index = 0u; index < directory->entry_count; ++index) {
        size_t position = directory->entries_offset +
                          (size_t)index * entry_size;
        uint16_t tag = (uint16_t)tiff_read_uint(file, position, 2u);
        if (tag == wanted_tag) {
            uint16_t type = (uint16_t)tiff_read_uint(file, position + 2u, 2u);
            uint64_t count = tiff_read_uint(file, position + 4u, count_size);
            unsigned item_size = tiff_type_size(type);
            uint64_t total_bytes;
            size_t value_slot = position + 4u + count_size;
            uint64_t value;
            if (!item_size || count > UINT64_MAX / item_size) return 0;
            total_bytes = count * item_size;
            value = tiff_read_uint(file, value_slot, value_size);
            field->tag = tag;
            field->type = type;
            field->count = count;
            field->inline_value = total_bytes <= inline_size;
            if (field->inline_value) field->value_offset = value_slot;
            else {
                if (value > SIZE_MAX ||
                    !tiff_range_ok(file, value, total_bytes)) return 0;
                field->value_offset = (size_t)value;
            }
            return 1;
        }
    }
    return 0;
}

static int tiff_field_item(const QwTiffFile *file,
                           const QwTiffField *field,
                           uint64_t index,
                           uint64_t *value)
{
    unsigned item_size = tiff_type_size(field->type);
    uint64_t offset;
    if (index >= field->count || !item_size ||
        index > UINT64_MAX / item_size) return 0;
    offset = (uint64_t)field->value_offset + index * item_size;
    if (offset > SIZE_MAX || !tiff_range_ok(file, offset, item_size)) return 0;
    switch (field->type) {
    case 1: case 6: case 7:
        *value = tiff_read_uint(file, (size_t)offset, 1u);
        return 1;
    case 3: case 8:
        *value = tiff_read_uint(file, (size_t)offset, 2u);
        return 1;
    case 4: case 9: case 13:
        *value = tiff_read_uint(file, (size_t)offset, 4u);
        return 1;
    case 16: case 17: case 18:
        *value = tiff_read_uint(file, (size_t)offset, 8u);
        return 1;
    default:
        return 0;
    }
}

static int tiff_tag_scalar(const QwTiffFile *file,
                           const QwTiffDirectory *directory,
                           uint16_t tag,
                           uint64_t fallback,
                           uint64_t *value,
                           QwTiffField *field_out)
{
    QwTiffField field;
    if (!tiff_find_field(file, directory, tag, &field)) {
        *value = fallback;
        return 1;
    }
    if (!field.count || !tiff_field_item(file, &field, 0u, value)) return 0;
    if (field_out) *field_out = field;
    return 1;
}

static int tiff_get_page_directory(const QwTiffFile *file,
                                   size_t page_index,
                                   QwTiffDirectory *out_directory)
{
    uint64_t offset = file->first_ifd;
    size_t page;
    for (page = 0u; page <= page_index; ++page) {
        QwTiffDirectory directory;
        if (!tiff_parse_directory(file, offset, &directory)) return 0;
        if (page == page_index) {
            *out_directory = directory;
            return 1;
        }
        if (!directory.next_ifd) return 0;
        offset = directory.next_ifd;
    }
    return 0;
}

int qw_tiff_probe(const void *data, size_t size)
{
    QwTiffFile file;
    return tiff_parse_header(&file, (const uint8_t *)data, size);
}

int qw_tiff_page_count(const uint8_t *data, size_t size, size_t *out_count)
{
    QwTiffFile file;
    uint64_t offset;
    uint64_t slow;
    uint64_t fast;
    size_t count = 0u;
    if (!data || !size || !out_count) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_count = 0u;
    if (!tiff_parse_header(&file, data, size)) return QW_STATUS_ERR_DECODE;
    offset = file.first_ifd;
    slow = file.first_ifd;
    fast = file.first_ifd;
    while (offset) {
        QwTiffDirectory directory;
        if (!tiff_parse_directory(&file, offset, &directory)) {
            return QW_STATUS_ERR_DECODE;
        }
        if (++count > 1000000u) return QW_STATUS_ERR_FORMAT;
        offset = directory.next_ifd;
        if (!offset) break;
        {
            QwTiffDirectory step;
            if (!tiff_parse_directory(&file, slow, &step)) return QW_STATUS_ERR_DECODE;
            slow = step.next_ifd;
            if (fast) {
                if (!tiff_parse_directory(&file, fast, &step)) return QW_STATUS_ERR_DECODE;
                fast = step.next_ifd;
                if (fast) {
                    if (!tiff_parse_directory(&file, fast, &step)) return QW_STATUS_ERR_DECODE;
                    fast = step.next_ifd;
                }
            }
            if (fast && slow == fast) return QW_STATUS_ERR_FORMAT;
        }
    }
    *out_count = count;
    return count ? QW_STATUS_OK : QW_STATUS_ERR_DECODE;
}

static int tiff_get_field(const QwTiffFile *file,
                          const QwTiffDirectory *directory,
                          uint16_t tag,
                          QwTiffField *field)
{
    return tiff_find_field(file, directory, tag, field);
}

static int tiff_read_repeated_sample(const QwTiffFile *file,
                                     const QwTiffDirectory *directory,
                                     uint16_t tag,
                                     uint16_t samples,
                                     uint64_t fallback,
                                     uint64_t *first_value)
{
    QwTiffField field;
    uint64_t value;
    uint64_t i;
    if (!tiff_find_field(file, directory, tag, &field)) {
        *first_value = fallback;
        return 1;
    }
    if (!field.count || (field.count != 1u && field.count != samples) ||
        !tiff_field_item(file, &field, 0u, &value)) return 0;
    for (i = 1u; i < field.count; ++i) {
        uint64_t next;
        if (!tiff_field_item(file, &field, i, &next) || next != value) return 0;
    }
    *first_value = value;
    return 1;
}

static int tiff_parse_image_info(const QwTiffFile *file,
                                 const QwTiffDirectory *directory,
                                 QwTiffImageInfo *info)
{
    uint64_t value;
    uint64_t width;
    uint64_t height;
    uint64_t rows_per_strip;
    uint64_t bits;
    uint64_t samples;
    uint64_t compression;
    uint64_t photometric;
    uint64_t orientation;
    uint64_t fill_order;
    uint64_t planar;
    uint64_t predictor;
    uint64_t t6_options;
    uint64_t sample_format;
    uint64_t pixels;
    size_t row_bits;
    memset(info, 0, sizeof(*info));
    if (!tiff_tag_scalar(file, directory, TIFF_TAG_IMAGE_WIDTH, 0u, &width, NULL) ||
        !tiff_tag_scalar(file, directory, TIFF_TAG_IMAGE_LENGTH, 0u, &height, NULL) ||
        !tiff_tag_scalar(file, directory, TIFF_TAG_SAMPLES_PER_PIXEL, 1u, &samples, NULL) ||
        !width || !height || width > UINT32_MAX || height > UINT32_MAX ||
        !samples || samples > 5u ||
        !tiff_tag_scalar(file, directory, TIFF_TAG_COMPRESSION, 1u, &compression, NULL) ||
        !tiff_tag_scalar(file, directory, TIFF_TAG_PHOTOMETRIC, 0u, &photometric, NULL) ||
        !tiff_tag_scalar(file, directory, TIFF_TAG_ORIENTATION, 1u, &orientation, NULL) ||
        !tiff_tag_scalar(file, directory, TIFF_TAG_FILL_ORDER, 1u, &fill_order, NULL) ||
        !tiff_tag_scalar(file, directory, TIFF_TAG_PLANAR_CONFIGURATION, 1u, &planar, NULL) ||
        !tiff_tag_scalar(file, directory, TIFF_TAG_PREDICTOR, 1u, &predictor, NULL) ||
        !tiff_tag_scalar(file, directory, TIFF_TAG_T6_OPTIONS, 0u, &t6_options, NULL) ||
        !tiff_read_repeated_sample(file, directory, TIFF_TAG_BITS_PER_SAMPLE,
                                   (uint16_t)samples, 1u, &bits) ||
        !tiff_read_repeated_sample(file, directory, TIFF_TAG_SAMPLE_FORMAT,
                                   (uint16_t)samples, 1u, &sample_format)) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (!tiff_tag_scalar(file, directory, TIFF_TAG_ROWS_PER_STRIP, height,
                         &rows_per_strip, NULL) || !rows_per_strip ||
        rows_per_strip > UINT32_MAX || orientation < 1u || orientation > 8u ||
        fill_order < 1u || fill_order > 2u || planar != 1u ||
        compression > UINT16_MAX || sample_format != 1u ||
        (bits != 1u && bits != 2u && bits != 4u &&
                                bits != 8u && bits != 16u) ||
        (predictor != 1u && predictor != 2u)) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (predictor == 2u && bits != 8u && bits != 16u) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (photometric == TIFF_PHOTOMETRIC_WHITE_IS_ZERO ||
        photometric == TIFF_PHOTOMETRIC_BLACK_IS_ZERO) {
        if (samples == 1u || samples == 2u) info->base_samples = 1u;
        else return QW_STATUS_ERR_UNSUPPORTED;
        info->output_channels = 1;
    } else if (photometric == TIFF_PHOTOMETRIC_RGB ||
               photometric == TIFF_PHOTOMETRIC_YCBCR) {
        if (samples != 3u && samples != 4u) return QW_STATUS_ERR_UNSUPPORTED;
        if (bits != 8u && bits != 16u) return QW_STATUS_ERR_UNSUPPORTED;
        info->base_samples = 3u;
        info->output_channels = 3;
    } else if (photometric == TIFF_PHOTOMETRIC_PALETTE) {
        if (samples != 1u && samples != 2u) return QW_STATUS_ERR_UNSUPPORTED;
        if (bits > 8u) return QW_STATUS_ERR_UNSUPPORTED;
        info->base_samples = 1u;
        info->output_channels = 3;
    } else if (photometric == TIFF_PHOTOMETRIC_SEPARATED) {
        if (samples != 4u && samples != 5u) return QW_STATUS_ERR_UNSUPPORTED;
        if (bits != 8u && bits != 16u) return QW_STATUS_ERR_UNSUPPORTED;
        info->base_samples = 4u;
        info->output_channels = 3;
    } else {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (samples != info->base_samples && samples != info->base_samples + 1u) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (samples == info->base_samples + 1u) {
        QwTiffField extra;
        if (!tiff_find_field(file, directory, TIFF_TAG_EXTRA_SAMPLES, &extra) ||
            extra.count != 1u || !tiff_field_item(file, &extra, 0u, &value) ||
            (value != 1u && value != 2u)) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        info->extra_sample_type = (uint16_t)value;
    }
    if (width > SIZE_MAX / samples || width * samples > SIZE_MAX / bits) {
        return QW_STATUS_ERR_FORMAT;
    }
    row_bits = (size_t)width * samples * (size_t)bits;
    if (row_bits > SIZE_MAX - 7u) return QW_STATUS_ERR_FORMAT;
    info->source_row_bytes = (row_bits + 7u) / 8u;
    if (height > SIZE_MAX / width) return QW_STATUS_ERR_FORMAT;
    if (!qw_image_dimensions_within_limits((uint32_t)width,
                                           (uint32_t)height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    pixels = width * height;
    if (pixels > SIZE_MAX / (size_t)info->output_channels) return QW_STATUS_ERR_FORMAT;
    info->output_pixel_count = (size_t)pixels;
    info->output_size = info->output_pixel_count * (size_t)info->output_channels;
    info->width = (uint32_t)width;
    info->height = (uint32_t)height;
    info->rows_per_strip = (uint32_t)rows_per_strip;
    info->bits_per_sample = (uint16_t)bits;
    info->samples_per_pixel = (uint16_t)samples;
    info->compression = (uint16_t)compression;
    info->photometric = (uint16_t)photometric;
    info->orientation = (uint16_t)orientation;
    info->fill_order = (uint16_t)fill_order;
    info->planar_configuration = (uint16_t)planar;
    info->predictor = (uint16_t)predictor;
    info->sample_format = (uint16_t)sample_format;
    info->output_photometric = (int)photometric;
    if (photometric == TIFF_PHOTOMETRIC_YCBCR) {
        if (compression != TIFF_COMPRESSION_JPEG &&
            compression != TIFF_COMPRESSION_JPEG_OLD) {
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        info->output_photometric = TIFF_PHOTOMETRIC_RGB;
    }
    if (!tiff_get_field(file, directory, TIFF_TAG_STRIP_OFFSETS,
                        &info->strip_offsets_field) ||
        !tiff_get_field(file, directory, TIFF_TAG_STRIP_BYTE_COUNTS,
                        &info->strip_byte_counts_field)) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    info->strip_offsets = &info->strip_offsets_field;
    info->strip_byte_counts = &info->strip_byte_counts_field;
    if (tiff_get_field(file, directory, TIFF_TAG_COLOR_MAP,
                       &info->color_map_field)) {
        info->color_map = &info->color_map_field;
    }
    if (tiff_get_field(file, directory, TIFF_TAG_JPEG_TABLES,
                       &info->jpeg_tables_field)) {
        info->jpeg_tables = &info->jpeg_tables_field;
        /* JPEG tables are optional for self-contained strip streams. */
    }
    if (info->photometric == TIFF_PHOTOMETRIC_PALETTE) {
        uint64_t entries = UINT64_C(1) << bits;
        if (!info->color_map || info->color_map->type != 3u ||
            info->color_map->count != entries * 3u) {
            return QW_STATUS_ERR_FORMAT;
        }
    }
    if ((compression == TIFF_COMPRESSION_CCITT_T6) &&
        (bits != 1u || samples != 1u ||
         (photometric != TIFF_PHOTOMETRIC_WHITE_IS_ZERO &&
          photometric != TIFF_PHOTOMETRIC_BLACK_IS_ZERO) || t6_options != 0u)) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (compression != TIFF_COMPRESSION_NONE &&
        compression != TIFF_COMPRESSION_CCITT_T6 &&
        compression != TIFF_COMPRESSION_LZW &&
        compression != TIFF_COMPRESSION_JPEG &&
        compression != TIFF_COMPRESSION_JPEG_OLD &&
        compression != TIFF_COMPRESSION_DEFLATE &&
        compression != TIFF_COMPRESSION_DEFLATE_OLD &&
        compression != TIFF_COMPRESSION_PACKBITS) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    return QW_STATUS_OK;
}

static int tiff_unpack_packbits(const uint8_t *source,
                                size_t source_size,
                                uint8_t *destination,
                                size_t destination_size)
{
    size_t input = 0u;
    size_t output = 0u;
    while (input < source_size && output < destination_size) {
        int8_t control = (int8_t)source[input++];
        if (control >= 0) {
            size_t count = (size_t)control + 1u;
            if (count > source_size - input || count > destination_size - output) return 0;
            memcpy(destination + output, source + input, count);
            input += count;
            output += count;
        } else if (control != INT8_MIN) {
            size_t count = (size_t)(1 - control);
            if (input >= source_size || count > destination_size - output) return 0;
            memset(destination + output, source[input++], count);
            output += count;
        }
    }
    return output == destination_size;
}

typedef struct QwTiffBitReader {
    const uint8_t *data;
    size_t size;
    size_t bit_position;
} QwTiffBitReader;

static int tiff_read_lzw_code(QwTiffBitReader *reader,
                              unsigned code_width,
                              unsigned *code)
{
    unsigned i;
    unsigned value = 0u;
    if (reader->bit_position > reader->size * 8u ||
        code_width > reader->size * 8u - reader->bit_position) return 0;
    for (i = 0u; i < code_width; ++i) {
        size_t position = reader->bit_position++;
        value = (value << 1) |
            ((reader->data[position >> 3] >> (7u - (position & 7u))) & 1u);
    }
    *code = value;
    return 1;
}

static int tiff_unpack_lzw(const uint8_t *source,
                           size_t source_size,
                           uint8_t *destination,
                           size_t destination_size)
{
    uint16_t prefix[4096];
    uint8_t suffix[4096];
    uint8_t stack[4096];
    QwTiffBitReader reader;
    unsigned code_width = 9u;
    unsigned next_code = 258u;
    unsigned old_code = 0u;
    int have_old = 0;
    size_t output = 0u;
    size_t i;
    reader.data = source;
    reader.size = source_size;
    reader.bit_position = 0u;
    for (i = 0u; i < 256u; ++i) suffix[i] = (uint8_t)i;
    for (;;) {
        unsigned code;
        unsigned walk;
        size_t stack_count = 0u;
        uint8_t first;
        if (!tiff_read_lzw_code(&reader, code_width, &code)) return 0;
        if (code == 256u) {
            code_width = 9u;
            next_code = 258u;
            have_old = 0;
            continue;
        }
        if (code == 257u) return output == destination_size;
        if (code > next_code || (code == next_code && !have_old)) return 0;
        walk = code;
        if (code == next_code) {
            walk = old_code;
        }
        while (walk >= 256u) {
            if (walk >= next_code || stack_count >= sizeof(stack)) return 0;
            stack[stack_count++] = suffix[walk];
            walk = prefix[walk];
        }
        first = suffix[walk];
        if (stack_count >= sizeof(stack)) return 0;
        stack[stack_count++] = first;
        if (code == next_code) {
            if (stack_count >= sizeof(stack)) return 0;
            memmove(stack + 1u, stack, stack_count);
            stack[0] = first;
            ++stack_count;
        }
        while (stack_count) {
            if (output >= destination_size) return 0;
            destination[output++] = stack[--stack_count];
        }
        if (have_old && next_code < 4096u) {
            prefix[next_code] = (uint16_t)old_code;
            suffix[next_code] = first;
            ++next_code;
            if (code_width < 12u && next_code == ((1u << code_width) - 1u)) {
                ++code_width;
            }
        }
        old_code = code;
        have_old = 1;
    }
}

static int tiff_inflate_attempt(const uint8_t *source,
                                size_t source_size,
                                uint8_t *destination,
                                size_t destination_size,
                                int zlib_wrapped)
{
    if (zlib_wrapped) {
        return qw_compression_inflate_zlib(source, source_size, destination,
                                           destination_size);
    }
    return qw_compression_inflate_raw(source, source_size, destination,
                                      destination_size);
}

static int tiff_unpack_deflate(const uint8_t *source,
                               size_t source_size,
                               uint8_t *destination,
                               size_t destination_size)
{
    return tiff_inflate_attempt(source, source_size, destination,
                                destination_size, 1) ||
           tiff_inflate_attempt(source, source_size, destination,
                                destination_size, 0);
}

static int tiff_decode_jpeg_strip(const uint8_t *source,
                                  size_t source_size,
                                  const QwTiffFile *file,
                                  const QwTiffField *tables,
                                  uint32_t width,
                                  uint32_t rows,
                                  int channels,
                                  uint8_t *destination,
                                  size_t destination_size)
{
    uint8_t *joined = NULL;
    const uint8_t *jpeg_data = source;
    size_t jpeg_size = source_size;
    size_t output_stride;
    int image_width = 0;
    int image_height = 0;
    int source_channels = 0;
    stbi_uc *pixels = NULL;
    int requested_channels = channels == 1 ? 1 : 3;

    if (source_size < 2u || (channels != 1 && channels != 3) ||
        width > (uint32_t)INT_MAX || rows > (uint32_t)INT_MAX ||
        !destination) {
        return 0;
    }
    output_stride = (size_t)width * (size_t)channels;
    if (rows && output_stride > SIZE_MAX / rows) return 0;
    if (output_stride * rows != destination_size) return 0;
    if (!(source[0] == 0xffu && source[1] == 0xd8u) ||
        (tables && tables->count)) {
        const uint8_t *table_data = NULL;
        size_t table_size = 0u;
        size_t prefix_start = 0u;
        size_t prefix_end = 0u;
        size_t strip_start = 0u;
        size_t strip_end = source_size;
        size_t capacity;
        size_t cursor = 0u;
        if (tables && (tables->count > SIZE_MAX || tables->type != 7u ||
            (tables->count > 0u &&
             !tiff_range_ok(file, tables->value_offset, tables->count)))) {
            return 0;
        }
        if (tables && tables->count) {
            table_data = file->data + tables->value_offset;
            table_size = (size_t)tables->count;
        }
        if (table_size >= 2u && table_data[0] == 0xffu && table_data[1] == 0xd8u) {
            prefix_start = 2u;
            prefix_end = table_size;
            if (table_size >= 4u && table_data[table_size - 2u] == 0xffu &&
                table_data[table_size - 1u] == 0xd9u) {
                prefix_end -= 2u;
            }
        }
        if (source_size >= 2u && source[source_size - 2u] == 0xffu &&
            source[source_size - 1u] == 0xd9u) strip_end -= 2u;
        if (source_size >= 2u && source[0] == 0xffu && source[1] == 0xd8u) {
            strip_start = 2u;
        }
        if (prefix_end < prefix_start || strip_end < strip_start ||
            prefix_end - prefix_start > SIZE_MAX - (strip_end - strip_start) ||
            prefix_end - prefix_start + (strip_end - strip_start) > SIZE_MAX - 4u) {
            return 0;
        }
        capacity = 4u + (prefix_end - prefix_start) +
                   (strip_end - strip_start);
        joined = (uint8_t *)malloc(capacity);
        if (!joined) return 0;
        joined[cursor++] = 0xffu;
        joined[cursor++] = 0xd8u;
        if (prefix_end > prefix_start) {
            memcpy(joined + cursor, table_data + prefix_start,
                   prefix_end - prefix_start);
            cursor += prefix_end - prefix_start;
        }
        if (strip_end > strip_start) {
            memcpy(joined + cursor, source + strip_start,
                   strip_end - strip_start);
            cursor += strip_end - strip_start;
        }
        joined[cursor++] = 0xffu;
        joined[cursor++] = 0xd9u;
        jpeg_data = joined;
        jpeg_size = cursor;
    }
    if (jpeg_size > (size_t)INT_MAX) goto jpeg_fail;
    if (!stbi_info_from_memory((const stbi_uc *)jpeg_data, (int)jpeg_size,
                               &image_width, &image_height, &source_channels) ||
        image_width != (int)width || image_height != (int)rows) {
        goto jpeg_fail;
    }
    pixels = stbi_load_from_memory((const stbi_uc *)jpeg_data, (int)jpeg_size,
                                   &image_width, &image_height, &source_channels,
                                   requested_channels);
    if (!pixels) goto jpeg_fail;
    if (image_width != (int)width || image_height != (int)rows) goto jpeg_fail;
    memcpy(destination, pixels, destination_size);
    stbi_image_free(pixels);
    free(joined);
    return 1;

jpeg_fail:
    stbi_image_free(pixels);
    free(joined);
    return 0;
}

static uint8_t tiff_reverse_byte(uint8_t value)
{
    value = (uint8_t)(((value & 0x55u) << 1) | ((value >> 1) & 0x55u));
    value = (uint8_t)(((value & 0x33u) << 2) | ((value >> 2) & 0x33u));
    return (uint8_t)((value << 4) | (value >> 4));
}

static int tiff_decode_strip(const QwTiffFile *file,
                             const QwTiffImageInfo *info,
                             uint64_t strip_index,
                             uint32_t rows,
                             uint8_t *destination,
                             size_t destination_size)
{
    uint64_t source_offset;
    uint64_t source_size_value;
    const uint8_t *source;
    size_t source_size;
    size_t expected_size;
    if (!tiff_field_item(file, info->strip_offsets, strip_index, &source_offset) ||
        !tiff_field_item(file, info->strip_byte_counts, strip_index,
                         &source_size_value) ||
        source_offset > SIZE_MAX || source_size_value > SIZE_MAX ||
        !tiff_range_ok(file, source_offset, source_size_value)) {
        return QW_STATUS_ERR_FORMAT;
    }
    source = file->data + (size_t)source_offset;
    source_size = (size_t)source_size_value;
    if (rows && info->source_row_bytes > SIZE_MAX / rows) return QW_STATUS_ERR_FORMAT;
    expected_size = info->source_row_bytes * rows;
    if (expected_size != destination_size) return QW_STATUS_ERR_FORMAT;

    if (info->compression == TIFF_COMPRESSION_NONE) {
        if (source_size < expected_size) return QW_STATUS_ERR_DECODE;
        memcpy(destination, source, expected_size);
    } else if (info->compression == TIFF_COMPRESSION_PACKBITS) {
        if (!tiff_unpack_packbits(source, source_size, destination, expected_size)) {
            return QW_STATUS_ERR_DECODE;
        }
    } else if (info->compression == TIFF_COMPRESSION_LZW) {
        if (!tiff_unpack_lzw(source, source_size, destination, expected_size)) {
            return QW_STATUS_ERR_DECODE;
        }
    } else if (info->compression == TIFF_COMPRESSION_DEFLATE ||
               info->compression == TIFF_COMPRESSION_DEFLATE_OLD) {
        if (!tiff_unpack_deflate(source, source_size, destination, expected_size)) {
            return QW_STATUS_ERR_DECODE;
        }
    } else if (info->compression == TIFF_COMPRESSION_CCITT_T6) {
        return QW_STATUS_ERR_UNSUPPORTED;
    } else if (info->compression == TIFF_COMPRESSION_JPEG ||
               info->compression == TIFF_COMPRESSION_JPEG_OLD) {
        size_t channels = info->output_photometric == TIFF_PHOTOMETRIC_RGB ? 3u : 1u;
        if (info->bits_per_sample != 8u || info->samples_per_pixel != channels ||
            !tiff_decode_jpeg_strip(source, source_size, file, info->jpeg_tables,
                                    info->width, rows, (int)channels,
                                    destination, expected_size)) {
            return QW_STATUS_ERR_DECODE;
        }
    }
    if (info->fill_order == 2u && info->bits_per_sample < 8u &&
        info->compression != TIFF_COMPRESSION_CCITT_T6) {
        size_t i;
        for (i = 0u; i < expected_size; ++i) {
            destination[i] = tiff_reverse_byte(destination[i]);
        }
    }
    if (info->predictor == 2u) {
        uint32_t y;
        size_t samples_per_row = (size_t)info->width * info->samples_per_pixel;
        for (y = 0u; y < rows; ++y) {
            uint8_t *row = destination + (size_t)y * info->source_row_bytes;
            size_t sample;
            if (info->bits_per_sample == 8u) {
                for (sample = info->samples_per_pixel; sample < samples_per_row; ++sample) {
                    row[sample] = (uint8_t)(row[sample] + row[sample - info->samples_per_pixel]);
                }
            } else {
                for (sample = info->samples_per_pixel; sample < samples_per_row; ++sample) {
                    size_t pos = sample * 2u;
                    size_t previous = (sample - info->samples_per_pixel) * 2u;
                    uint16_t current_value = (uint16_t)tiff_read_uint(
                        &(QwTiffFile){row, info->source_row_bytes, 0u, 0, file->big_endian},
                        pos, 2u);
                    uint16_t previous_value = (uint16_t)tiff_read_uint(
                        &(QwTiffFile){row, info->source_row_bytes, 0u, 0, file->big_endian},
                        previous, 2u);
                    uint16_t sum = (uint16_t)(current_value + previous_value);
                    if (file->big_endian) {
                        row[pos] = (uint8_t)(sum >> 8);
                        row[pos + 1u] = (uint8_t)sum;
                    } else {
                        row[pos] = (uint8_t)sum;
                        row[pos + 1u] = (uint8_t)(sum >> 8);
                    }
                }
            }
        }
    }
    return QW_STATUS_OK;
}

static uint32_t tiff_read_sample(const uint8_t *row,
                                 size_t sample_index,
                                 uint16_t bits,
                                 int big_endian)
{
    if (bits == 8u) return row[sample_index];
    if (bits == 16u) {
        size_t offset = sample_index * 2u;
        return big_endian
            ? ((uint32_t)row[offset] << 8) | row[offset + 1u]
            : ((uint32_t)row[offset + 1u] << 8) | row[offset];
    }
    {
        size_t bit_offset = sample_index * bits;
        unsigned shift = 8u - bits - (unsigned)(bit_offset & 7u);
        uint32_t mask = (1u << bits) - 1u;
        return (row[bit_offset >> 3] >> shift) & mask;
    }
}

static uint8_t tiff_scale_sample(uint32_t value, uint16_t bits)
{
    uint32_t maximum = bits == 16u ? 65535u : ((1u << bits) - 1u);
    return (uint8_t)((value * 255u + maximum / 2u) / maximum);
}

static int tiff_color_map(const QwTiffFile *file,
                          const QwTiffImageInfo *info,
                          uint8_t **palette_out,
                          size_t *entries_out)
{
    size_t entries;
    uint8_t *palette;
    size_t index;
    if (info->photometric != TIFF_PHOTOMETRIC_PALETTE) {
        *palette_out = NULL;
        *entries_out = 0u;
        return 1;
    }
    entries = (size_t)1u << info->bits_per_sample;
    if (entries > SIZE_MAX / 3u) return 0;
    palette = (uint8_t *)malloc(entries * 3u);
    if (!palette) return 0;
    for (index = 0u; index < entries; ++index) {
        uint64_t red;
        uint64_t green;
        uint64_t blue;
        if (!tiff_field_item(file, info->color_map, index, &red) ||
            !tiff_field_item(file, info->color_map, entries + index, &green) ||
            !tiff_field_item(file, info->color_map, entries * 2u + index, &blue)) {
            free(palette);
            return 0;
        }
        palette[index * 3u] = (uint8_t)((red + 128u) / 257u);
        palette[index * 3u + 1u] = (uint8_t)((green + 128u) / 257u);
        palette[index * 3u + 2u] = (uint8_t)((blue + 128u) / 257u);
    }
    *palette_out = palette;
    *entries_out = entries;
    return 1;
}

static uint32_t tiff_orient_x(uint16_t orientation,
                              uint32_t x,
                              uint32_t y,
                              uint32_t width,
                              uint32_t height)
{
    switch (orientation) {
    case 2: case 3: return width - 1u - x;
    case 5: case 8: return y;
    case 6: case 7: return height - 1u - y;
    default: return x;
    }
}

static uint32_t tiff_orient_y(uint16_t orientation,
                              uint32_t x,
                              uint32_t y,
                              uint32_t width,
                              uint32_t height)
{
    switch (orientation) {
    case 3: case 4: return height - 1u - y;
    case 5: case 6: return x;
    case 7: case 8: return width - 1u - x;
    default: return y;
    }
}

static uint8_t tiff_composite_alpha(uint8_t color,
                                    uint8_t alpha,
                                    uint16_t extra_sample_type)
{
    uint32_t result;
    if (extra_sample_type == 2u) {
        result = (uint32_t)color + (255u - alpha);
    } else {
        result = ((uint32_t)color * alpha + 255u * (255u - alpha) + 127u) / 255u;
    }
    return (uint8_t)(result > 255u ? 255u : result);
}

int qw_tiff_decode_page(const uint8_t *data,
                        size_t size,
                        size_t page_index,
                        QwPageImage *out)
{
    QwTiffFile file;
    QwTiffDirectory directory;
    QwTiffImageInfo info;
    uint8_t *raw = NULL;
    uint8_t *pixels = NULL;
    uint8_t *palette = NULL;
    size_t palette_entries = 0u;
    size_t strip_count;
    size_t strip_index;
    uint32_t output_width;
    uint32_t output_height;
    int rc;

    if (!data || !size || !out || page_index > UINT32_MAX) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (!tiff_parse_header(&file, data, size) ||
        !tiff_get_page_directory(&file, page_index, &directory)) {
        return QW_STATUS_ERR_DECODE;
    }
    rc = tiff_parse_image_info(&file, &directory, &info);
    if (rc != QW_STATUS_OK) return rc;
    if (info.strip_offsets->count != info.strip_byte_counts->count) {
        return QW_STATUS_ERR_FORMAT;
    }
    strip_count = ((size_t)info.height - 1u) / info.rows_per_strip + 1u;
    if (info.strip_offsets->count < strip_count ||
        info.strip_byte_counts->count < strip_count ||
        info.source_row_bytes > SIZE_MAX / info.height) {
        return QW_STATUS_ERR_FORMAT;
    }
    raw = (uint8_t *)calloc(info.source_row_bytes * info.height, 1u);
    if (!raw) return QW_STATUS_ERR_OUT_OF_MEMORY;
    if (!tiff_color_map(&file, &info, &palette, &palette_entries)) {
        free(raw);
        return info.photometric == TIFF_PHOTOMETRIC_PALETTE
                   ? QW_STATUS_ERR_OUT_OF_MEMORY : QW_STATUS_ERR_FORMAT;
    }
    for (strip_index = 0u; strip_index < strip_count; ++strip_index) {
        uint32_t first_row = (uint32_t)(strip_index * info.rows_per_strip);
        uint32_t rows = info.height - first_row;
        size_t strip_size;
        if (rows > info.rows_per_strip) rows = info.rows_per_strip;
        if (info.source_row_bytes > SIZE_MAX / rows) {
            rc = QW_STATUS_ERR_FORMAT;
            goto cleanup;
        }
        strip_size = info.source_row_bytes * rows;
        rc = tiff_decode_strip(&file, &info, strip_index, rows,
                               raw + (size_t)first_row * info.source_row_bytes,
                               strip_size);
        if (rc != QW_STATUS_OK) goto cleanup;
    }
    output_width = info.orientation >= 5u ? info.height : info.width;
    output_height = info.orientation >= 5u ? info.width : info.height;
    if (output_width > SIZE_MAX / (size_t)info.output_channels ||
        (size_t)output_width * info.output_channels > SIZE_MAX / output_height) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    pixels = (uint8_t *)malloc(info.output_size ? info.output_size : 1u);
    if (!pixels) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (uint32_t y = 0u; y < info.height; ++y) {
        const uint8_t *row = raw + (size_t)y * info.source_row_bytes;
        for (uint32_t x = 0u; x < info.width; ++x) {
            size_t sample_base = (size_t)x * info.samples_per_pixel;
            uint32_t sample[5] = {0u, 0u, 0u, 0u, 0u};
            uint8_t color[3];
            uint8_t alpha = 255u;
            uint32_t dx = tiff_orient_x(info.orientation, x, y,
                                        info.width, info.height);
            uint32_t dy = tiff_orient_y(info.orientation, x, y,
                                        info.width, info.height);
            size_t dst = ((size_t)dy * output_width + dx) *
                         (size_t)info.output_channels;
            uint16_t channel;
            for (channel = 0u; channel < info.samples_per_pixel; ++channel) {
                sample[channel] = tiff_read_sample(
                    row, sample_base + channel, info.bits_per_sample,
                    file.big_endian);
            }
            if (info.extra_sample_type) {
                alpha = tiff_scale_sample(sample[info.base_samples],
                                          info.bits_per_sample);
            }
            if (info.photometric == TIFF_PHOTOMETRIC_WHITE_IS_ZERO ||
                info.photometric == TIFF_PHOTOMETRIC_BLACK_IS_ZERO) {
                uint8_t gray = tiff_scale_sample(sample[0], info.bits_per_sample);
                if (info.photometric == TIFF_PHOTOMETRIC_WHITE_IS_ZERO) {
                    gray = (uint8_t)(255u - gray);
                }
                if (info.extra_sample_type) {
                    gray = tiff_composite_alpha(gray, alpha, info.extra_sample_type);
                }
                pixels[dst] = gray;
            } else if (info.photometric == TIFF_PHOTOMETRIC_PALETTE) {
                size_t index = sample[0];
                if (index >= palette_entries) {
                    rc = QW_STATUS_ERR_FORMAT;
                    goto cleanup;
                }
                color[0] = palette[index * 3u];
                color[1] = palette[index * 3u + 1u];
                color[2] = palette[index * 3u + 2u];
                if (info.extra_sample_type) {
                    color[0] = tiff_composite_alpha(color[0], alpha, info.extra_sample_type);
                    color[1] = tiff_composite_alpha(color[1], alpha, info.extra_sample_type);
                    color[2] = tiff_composite_alpha(color[2], alpha, info.extra_sample_type);
                }
                memcpy(pixels + dst, color, 3u);
            } else if (info.photometric == TIFF_PHOTOMETRIC_SEPARATED) {
                uint8_t cyan = tiff_scale_sample(sample[0], info.bits_per_sample);
                uint8_t magenta = tiff_scale_sample(sample[1], info.bits_per_sample);
                uint8_t yellow = tiff_scale_sample(sample[2], info.bits_per_sample);
                uint8_t black = tiff_scale_sample(sample[3], info.bits_per_sample);
                color[0] = (uint8_t)(255u - ((cyan + black > 255u) ? 255u : cyan + black));
                color[1] = (uint8_t)(255u - ((magenta + black > 255u) ? 255u : magenta + black));
                color[2] = (uint8_t)(255u - ((yellow + black > 255u) ? 255u : yellow + black));
                if (info.extra_sample_type) {
                    color[0] = tiff_composite_alpha(color[0], alpha, info.extra_sample_type);
                    color[1] = tiff_composite_alpha(color[1], alpha, info.extra_sample_type);
                    color[2] = tiff_composite_alpha(color[2], alpha, info.extra_sample_type);
                }
                memcpy(pixels + dst, color, 3u);
            } else {
                color[0] = tiff_scale_sample(sample[0], info.bits_per_sample);
                color[1] = tiff_scale_sample(sample[1], info.bits_per_sample);
                color[2] = tiff_scale_sample(sample[2], info.bits_per_sample);
                if (info.extra_sample_type) {
                    color[0] = tiff_composite_alpha(color[0], alpha, info.extra_sample_type);
                    color[1] = tiff_composite_alpha(color[1], alpha, info.extra_sample_type);
                    color[2] = tiff_composite_alpha(color[2], alpha, info.extra_sample_type);
                }
                memcpy(pixels + dst, color, 3u);
            }
        }
    }
    out->data = pixels;
    out->size = info.output_size;
    out->kind = info.output_channels == 1 ? QW_PAGE_IMAGE_GRAY8
                                          : QW_PAGE_IMAGE_RGB24;
    out->width = output_width;
    out->height = output_height;
    pixels = NULL;
    rc = QW_STATUS_OK;

cleanup:
    free(raw);
    free(palette);
    free(pixels);
    return rc;
}
