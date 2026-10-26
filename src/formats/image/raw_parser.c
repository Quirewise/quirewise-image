#include "image_internal.h"

#include <string.h>

enum {
    RAW_TIFF_TAG_SUB_IFDS = 330,
    RAW_TIFF_TAG_PHOTOMETRIC = 262,
    RAW_TIFF_TAG_CFA_REPEAT_PATTERN_DIM = 33421,
    RAW_TIFF_TAG_CFA_PATTERN = 33422,
    RAW_TIFF_TAG_DNG_VERSION = 50706,
    RAW_TIFF_PHOTOMETRIC_CFA = 32803,
    RAW_TIFF_IFD_LIMIT = 64
};

typedef struct QwRawTiffMarkers {
    uint16_t photometric;
    uint16_t cfa_rows;
    uint16_t cfa_columns;
    size_t cfa_pattern_count;
    int has_photometric;
    int has_cfa_repeat_pattern;
    int has_cfa_pattern;
    int has_dng_version;
} QwRawTiffMarkers;

typedef struct QwRawTiffIfd {
    QwRawTiffMarkers markers;
    uint64_t next_ifd;
    uint64_t sub_ifds[RAW_TIFF_IFD_LIMIT];
    size_t sub_ifd_count;
} QwRawTiffIfd;

static uint16_t raw_read_u16(const uint8_t *bytes, int big_endian)
{
    return big_endian
        ? (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1])
        : (uint16_t)(((uint16_t)bytes[1] << 8) | bytes[0]);
}

static uint32_t raw_read_u32(const uint8_t *bytes, int big_endian)
{
    return big_endian
        ? ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
              ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3]
        : ((uint32_t)bytes[3] << 24) | ((uint32_t)bytes[2] << 16) |
              ((uint32_t)bytes[1] << 8) | (uint32_t)bytes[0];
}

static uint64_t raw_read_u64(const uint8_t *bytes, int big_endian)
{
    uint64_t value = 0u;
    if (big_endian) {
        for (size_t i = 0u; i < 8u; ++i)
            value = (value << 8) | bytes[i];
    } else {
        for (size_t i = 8u; i > 0u; --i)
            value = (value << 8) | bytes[i - 1u];
    }
    return value;
}

static int raw_range_ok(size_t size, uint64_t offset, uint64_t length)
{
    return offset <= (uint64_t)size &&
           length <= (uint64_t)size - offset;
}

static int raw_tiff_value(const uint8_t *bytes, size_t size,
                          size_t entry_offset, int big_tiff,
                          int big_endian, uint16_t type, uint64_t count,
                          const uint8_t **out_value)
{
    uint64_t byte_count;
    uint64_t value_offset;
    size_t slot_offset = entry_offset + (big_tiff ? 12u : 8u);
    size_t slot_size = big_tiff ? 8u : 4u;
    unsigned item_size;

    if (!out_value) return 0;
    *out_value = NULL;
    switch (type) {
    case 1u: case 2u: case 6u: case 7u: item_size = 1u; break;
    case 3u: case 8u: item_size = 2u; break;
    case 4u: case 9u: case 11u: case 13u: item_size = 4u; break;
    case 5u: case 10u: case 12u: case 16u: case 17u: case 18u:
        item_size = 8u;
        break;
    default: return 0;
    }
    if (count > UINT64_MAX / item_size) return 0;
    byte_count = count * item_size;
    if (byte_count <= slot_size) {
        *out_value = bytes + slot_offset;
        return 1;
    }
    value_offset = big_tiff
        ? raw_read_u64(bytes + slot_offset, big_endian)
        : raw_read_u32(bytes + slot_offset, big_endian);
    if (!raw_range_ok(size, value_offset, byte_count)) return 0;
    *out_value = bytes + (size_t)value_offset;
    return 1;
}

static int raw_tiff_parse_ifd(const uint8_t *bytes, size_t size,
                              uint64_t ifd_offset, int big_tiff,
                              int big_endian, QwRawTiffIfd *ifd)
{
    uint64_t entry_count;
    uint64_t entry_size = big_tiff ? 20u : 12u;
    uint64_t count_size = big_tiff ? 8u : 2u;
    uint64_t next_size = big_tiff ? 8u : 4u;
    uint64_t entries_size;
    uint64_t table_size;
    size_t entries_offset;

    if (!bytes || !ifd ||
        !raw_range_ok(size, ifd_offset, count_size)) {
        return 0;
    }
    memset(ifd, 0, sizeof(*ifd));
    entry_count = big_tiff
        ? raw_read_u64(bytes + (size_t)ifd_offset, big_endian)
        : raw_read_u16(bytes + (size_t)ifd_offset, big_endian);
    if (entry_count > (UINT64_MAX - count_size - next_size) / entry_size)
        return 0;
    entries_size = entry_count * entry_size;
    table_size = count_size + entries_size + next_size;
    if (!raw_range_ok(size, ifd_offset, table_size) ||
        ifd_offset + count_size > SIZE_MAX) {
        return 0;
    }
    entries_offset = (size_t)(ifd_offset + count_size);

    for (uint64_t index = 0u; index < entry_count; ++index) {
        size_t entry_offset = entries_offset + (size_t)(index * entry_size);
        uint16_t tag = raw_read_u16(bytes + entry_offset, big_endian);
        uint16_t type = raw_read_u16(bytes + entry_offset + 2u, big_endian);
        uint64_t count = big_tiff
            ? raw_read_u64(bytes + entry_offset + 4u, big_endian)
            : raw_read_u32(bytes + entry_offset + 4u, big_endian);
        const uint8_t *value;

        if (tag == RAW_TIFF_TAG_DNG_VERSION) {
            if (type != 1u || count != 4u ||
                !raw_tiff_value(bytes, size, entry_offset, big_tiff,
                                big_endian, type, count, &value)) {
                return 0;
            }
            ifd->markers.has_dng_version = value[0] == 1u;
        } else if (tag == RAW_TIFF_TAG_PHOTOMETRIC) {
            if (type != 3u || count != 1u ||
                !raw_tiff_value(bytes, size, entry_offset, big_tiff,
                                big_endian, type, count, &value)) {
                return 0;
            }
            ifd->markers.photometric = raw_read_u16(value, big_endian);
            ifd->markers.has_photometric = 1;
        } else if (tag == RAW_TIFF_TAG_CFA_REPEAT_PATTERN_DIM) {
            if (type != 3u || count != 2u ||
                !raw_tiff_value(bytes, size, entry_offset, big_tiff,
                                big_endian, type, count, &value)) {
                return 0;
            }
            ifd->markers.cfa_rows = raw_read_u16(value, big_endian);
            ifd->markers.cfa_columns = raw_read_u16(value + 2u, big_endian);
            ifd->markers.has_cfa_repeat_pattern =
                ifd->markers.cfa_rows != 0u && ifd->markers.cfa_rows <= 16u &&
                ifd->markers.cfa_columns != 0u &&
                ifd->markers.cfa_columns <= 16u;
        } else if (tag == RAW_TIFF_TAG_CFA_PATTERN) {
            if (type != 1u || count < 4u || count > 256u ||
                !raw_tiff_value(bytes, size, entry_offset, big_tiff,
                                big_endian, type, count, &value)) {
                return 0;
            }
            ifd->markers.cfa_pattern_count = (size_t)count;
            ifd->markers.has_cfa_pattern = 1;
        } else if (tag == RAW_TIFF_TAG_SUB_IFDS) {
            unsigned offset_size;
            if (count == 0u || count > RAW_TIFF_IFD_LIMIT ||
                (big_tiff ? (type != 4u && type != 13u &&
                             type != 16u && type != 18u)
                          : (type != 4u && type != 13u)) ||
                !raw_tiff_value(bytes, size, entry_offset, big_tiff,
                                big_endian, type, count, &value)) {
                return 0;
            }
            offset_size = type == 16u || type == 18u ? 8u : 4u;
            if (count > SIZE_MAX / offset_size) return 0;
            ifd->sub_ifd_count = (size_t)count;
            for (size_t sub_index = 0u;
                 sub_index < ifd->sub_ifd_count; ++sub_index) {
                uint64_t sub_offset = offset_size == 8u
                    ? raw_read_u64(value + sub_index * offset_size, big_endian)
                    : raw_read_u32(value + sub_index * offset_size, big_endian);
                if (sub_offset &&
                    !raw_range_ok(size, sub_offset, count_size)) {
                    return 0;
                }
                ifd->sub_ifds[sub_index] = sub_offset;
            }
        }
    }

    ifd->next_ifd = big_tiff
        ? raw_read_u64(bytes + entries_offset + (size_t)entries_size,
                       big_endian)
        : raw_read_u32(bytes + entries_offset + (size_t)entries_size,
                       big_endian);
    if (ifd->next_ifd &&
        !raw_range_ok(size, ifd->next_ifd, count_size)) {
        return 0;
    }
    return 1;
}

static int raw_schedule_ifd(uint64_t offset, int is_root,
                            uint64_t *offsets, uint8_t *root_flags,
                            size_t *count)
{
    if (!offset) return 1;
    for (size_t i = 0u; i < *count; ++i) {
        if (offsets[i] == offset) {
            if (is_root) root_flags[i] = 1u;
            return 1;
        }
    }
    if (*count >= RAW_TIFF_IFD_LIMIT) return 0;
    offsets[*count] = offset;
    root_flags[*count] = is_root ? 1u : 0u;
    ++*count;
    return 1;
}

static int raw_parse_tiff(const uint8_t *bytes, size_t size)
{
    uint64_t ifd_offset;
    uint64_t ifd_offsets[RAW_TIFF_IFD_LIMIT];
    uint8_t root_flags[RAW_TIFF_IFD_LIMIT] = {0};
    int big_endian;
    int big_tiff = 0;
    size_t ifd_count = 0u;
    size_t cursor = 0u;

    if (!bytes || size < 8u) return 0;
    if (bytes[0] == 'I' && bytes[1] == 'I') big_endian = 0;
    else if (bytes[0] == 'M' && bytes[1] == 'M') big_endian = 1;
    else return 0;

    if (raw_read_u16(bytes + 2u, big_endian) == 42u) {
        ifd_offset = raw_read_u32(bytes + 4u, big_endian);
    } else if (raw_read_u16(bytes + 2u, big_endian) == 43u) {
        if (size < 16u || raw_read_u16(bytes + 4u, big_endian) != 8u ||
            raw_read_u16(bytes + 6u, big_endian) != 0u) {
            return 0;
        }
        big_tiff = 1;
        ifd_offset = raw_read_u64(bytes + 8u, big_endian);
    } else {
        return 0;
    }

    if (!raw_schedule_ifd(ifd_offset, 1, ifd_offsets, root_flags,
                          &ifd_count)) {
        return 0;
    }
    while (cursor < ifd_count) {
        QwRawTiffIfd ifd;
        uint64_t current_offset = ifd_offsets[cursor];
        int is_root = root_flags[cursor] != 0u;
        ++cursor;
        if (!raw_tiff_parse_ifd(bytes, size, current_offset, big_tiff,
                                big_endian, &ifd)) {
            return 0;
        }
        if ((is_root && ifd.markers.has_dng_version) ||
            (ifd.markers.has_photometric &&
             ifd.markers.photometric == RAW_TIFF_PHOTOMETRIC_CFA &&
             ifd.markers.has_cfa_repeat_pattern &&
             ifd.markers.has_cfa_pattern &&
             ifd.markers.cfa_pattern_count >=
                 (size_t)ifd.markers.cfa_rows * ifd.markers.cfa_columns)) {
            return 1;
        }
        if (!raw_schedule_ifd(ifd.next_ifd, 0, ifd_offsets, root_flags,
                              &ifd_count)) {
            return 0;
        }
        for (size_t i = 0u; i < ifd.sub_ifd_count; ++i) {
            if (!raw_schedule_ifd(ifd.sub_ifds[i], 0, ifd_offsets,
                                  root_flags, &ifd_count)) {
                return 0;
            }
        }
    }
    return 0;
}

static int raw_parse_cr3(const uint8_t *bytes, size_t size)
{
    uint64_t ftyp_size;
    size_t ftyp_header_size = 8u;
    size_t position;
    size_t box_count = 0u;

    if (!bytes || size < 64u || memcmp(bytes + 4u, "ftyp", 4u) != 0) {
        return 0;
    }
    ftyp_size = raw_read_u32(bytes, 1);
    if (ftyp_size == 1u) {
        if (size < 24u) return 0;
        ftyp_size = raw_read_u64(bytes + 8u, 1);
        if (ftyp_size < 24u) return 0;
        ftyp_header_size = 16u;
    }
    if (ftyp_size < (uint64_t)ftyp_header_size + 8u ||
        ftyp_size > size ||
        ((ftyp_size - (uint64_t)ftyp_header_size - 8u) % 4u) != 0u ||
        memcmp(bytes + ftyp_header_size, "crx ", 4u) != 0) {
        return 0;
    }
    position = (size_t)ftyp_size;
    while (position < size) {
        uint64_t box_size;
        uint32_t box_type;
        size_t box_header = 8u;
        if (size - position < 8u) return 0;
        box_size = raw_read_u32(bytes + position, 1);
        box_type = raw_read_u32(bytes + position + 4u, 1);
        if (box_size == 1u) {
            if (size - position < 16u) return 0;
            box_size = raw_read_u64(bytes + position + 8u, 1);
            box_header = 16u;
        } else if (box_size == 0u) {
            box_size = (uint64_t)(size - position);
        }
        if (box_type == UINT32_C(0x75756964)) box_header += 16u;
        if (box_size < box_header || box_size > (uint64_t)(size - position))
            return 0;
        position += (size_t)box_size;
        ++box_count;
    }
    return position == size && box_count != 0u;
}

int qw_raw_probe(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    if (!bytes || size < 4u) return 0;
    if ((bytes[0] == 'I' && bytes[1] == 'I' &&
         ((bytes[2] == 42u && bytes[3] == 0u) ||
          (bytes[2] == 43u && bytes[3] == 0u))) ||
        (bytes[0] == 'M' && bytes[1] == 'M' &&
         bytes[2] == 0u && (bytes[3] == 42u || bytes[3] == 43u))) {
        return raw_parse_tiff(bytes, size);
    }
    if (raw_parse_cr3(bytes, size)) return 1;
    if (size >= 15u && memcmp(bytes, "FUJIFILMCCD-RAW", 15u) == 0)
        return size >= 64u;
    if (memcmp(bytes, "FOVb", 4u) == 0) return size >= 64u;
    if (size >= 14u &&
        memcmp(bytes, "II\x1a\0\0\0HEAPCCDR", 14u) == 0) {
        return size >= 64u;
    }
    return bytes[0] == 0u && bytes[1] == 'M' && bytes[2] == 'R' &&
           bytes[3] == 'M' && size >= 64u;
}
