#include "image_internal.h"

#include "../../core/byte_utils.h"

#include <string.h>

typedef struct QwBmffBox {
    const uint8_t *data;
    size_t payload_size;
    size_t next;
    uint32_t type;
} QwBmffBox;

static uint32_t bmff_fourcc(const char value[4])
{
    return ((uint32_t)(uint8_t)value[0] << 24) |
           ((uint32_t)(uint8_t)value[1] << 16) |
           ((uint32_t)(uint8_t)value[2] << 8) |
           (uint32_t)(uint8_t)value[3];
}

static int bmff_read_box(const uint8_t *bytes,
                         size_t end,
                         size_t position,
                         QwBmffBox *box)
{
    uint64_t box_size;
    size_t header_size = 8u;
    if (!bytes || !box || position > end || end - position < 8u)
        return 0;
    box_size = qw_read_u32_be(bytes + position);
    box->type = qw_read_u32_be(bytes + position + 4u);
    if (box_size == 1u) {
        if (end - position < 16u) return 0;
        box_size = qw_read_u64_be(bytes + position + 8u);
        header_size = 16u;
    } else if (box_size == 0u) {
        box_size = (uint64_t)(end - position);
    }
    if (box->type == bmff_fourcc("uuid")) {
        if (header_size > SIZE_MAX - 16u) return 0;
        header_size += 16u;
    }
    if (box_size < header_size || box_size > (uint64_t)(end - position) ||
        box_size > SIZE_MAX) {
        return 0;
    }
    box->data = bytes + position + header_size;
    box->payload_size = (size_t)box_size - header_size;
    box->next = position + (size_t)box_size;
    return 1;
}

static uint32_t bmff_fullbox_flags(const uint8_t *data)
{
    return ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

static int bmff_skip_bytes(size_t size, size_t *position, size_t count)
{
    if (!position || *position > size || count > size - *position) return 0;
    *position += count;
    return 1;
}

static int bmff_skip_uint(const uint8_t *data,
                          size_t size,
                          size_t *position,
                          uint8_t width)
{
    (void)data;
    return width <= 8u && bmff_skip_bytes(size, position, width);
}

static int bmff_valid_uint_size(uint8_t width)
{
    return width == 0u || width == 4u || width == 8u;
}

static int bmff_parse_hdlr(const QwBmffBox *box)
{
    return box && box->payload_size >= 24u && box->data[0] == 0u &&
           bmff_fullbox_flags(box->data) == 0u &&
           memcmp(box->data + 8u, "pict", 4u) == 0;
}

static int bmff_parse_pitm(const QwBmffBox *box)
{
    size_t item_id_size;
    if (!box || box->payload_size < 4u ||
        bmff_fullbox_flags(box->data) != 0u || box->data[0] > 1u) {
        return 0;
    }
    item_id_size = box->data[0] == 0u ? 2u : 4u;
    return box->payload_size >= 4u + item_id_size;
}

static int bmff_parse_iloc(const QwBmffBox *box)
{
    const uint8_t *data;
    size_t size;
    size_t position = 0u;
    uint8_t version;
    uint8_t offset_size;
    uint8_t length_size;
    uint8_t base_offset_size;
    uint8_t index_size;
    uint32_t item_count;

    if (!box || box->payload_size < 8u) return 0;
    data = box->data;
    size = box->payload_size;
    version = data[0];
    if (version > 2u || bmff_fullbox_flags(data) != 0u) return 0;
    offset_size = (uint8_t)(data[4] >> 4);
    length_size = (uint8_t)(data[4] & 0x0fu);
    base_offset_size = (uint8_t)(data[5] >> 4);
    index_size = (uint8_t)(data[5] & 0x0fu);
    if (!bmff_valid_uint_size(offset_size) ||
        !bmff_valid_uint_size(length_size) ||
        !bmff_valid_uint_size(base_offset_size) ||
        (version == 0u && index_size != 0u) ||
        !bmff_valid_uint_size(index_size)) {
        return 0;
    }
    position = 6u;
    if (version == 2u) {
        if (size - position < 4u) return 0;
        item_count = qw_read_u32_be(data + position);
        position += 4u;
    } else {
        if (size - position < 2u) return 0;
        item_count = qw_read_u16_be(data + position);
        position += 2u;
    }
    if (!item_count) return 0;

    for (uint32_t item = 0u; item < item_count; ++item) {
        uint16_t construction_method;
        uint16_t data_reference_index;
        uint16_t extent_count;
        size_t item_id_size = version == 2u ? 4u : 2u;
        if (!bmff_skip_bytes(size, &position, item_id_size)) return 0;
        if (version >= 1u) {
            if (size - position < 2u) return 0;
            construction_method = qw_read_u16_be(data + position);
            position += 2u;
            if ((construction_method & 0xfff0u) != 0u ||
                (construction_method & 0x000fu) > 2u) return 0;
        }
        if (size - position < 2u) return 0;
        data_reference_index = qw_read_u16_be(data + position);
        position += 2u;
        if (data_reference_index != 0u ||
            !bmff_skip_uint(data, size, &position, base_offset_size) ||
            size - position < 2u) {
            return 0;
        }
        extent_count = qw_read_u16_be(data + position);
        position += 2u;
        for (uint16_t extent = 0u; extent < extent_count; ++extent) {
            if (version >= 1u && index_size &&
                !bmff_skip_uint(data, size, &position, index_size)) {
                return 0;
            }
            if (!bmff_skip_uint(data, size, &position, offset_size) ||
                !bmff_skip_uint(data, size, &position, length_size)) {
                return 0;
            }
        }
    }
    return position == size;
}

static int bmff_parse_infe(const QwBmffBox *box)
{
    uint8_t version;
    size_t name_offset;
    if (!box || box->payload_size < 4u) return 0;
    version = box->data[0];
    if (version == 2u) {
        if (box->payload_size < 13u) return 0;
        name_offset = 12u;
    } else if (version == 3u) {
        if (box->payload_size < 15u) return 0;
        name_offset = 14u;
    } else {
        return 0;
    }
    return memchr(box->data + name_offset, 0,
                  box->payload_size - name_offset) != NULL;
}

static int bmff_parse_iinf(const QwBmffBox *box)
{
    size_t position;
    uint32_t entry_count;
    uint32_t actual_count = 0u;
    uint8_t version;
    if (!box || box->payload_size < 6u) return 0;
    version = box->data[0];
    if (version > 1u || bmff_fullbox_flags(box->data) != 0u) return 0;
    if (version == 0u) {
        entry_count = qw_read_u16_be(box->data + 4u);
        position = 6u;
    } else {
        if (box->payload_size < 8u) return 0;
        entry_count = qw_read_u32_be(box->data + 4u);
        position = 8u;
    }
    if (!entry_count) return 0;
    while (position < box->payload_size) {
        QwBmffBox entry;
        if (!bmff_read_box(box->data, box->payload_size, position, &entry) ||
            entry.type != bmff_fourcc("infe") || !bmff_parse_infe(&entry)) {
            return 0;
        }
        position = entry.next;
        ++actual_count;
    }
    return position == box->payload_size && actual_count == entry_count;
}

static int bmff_parse_ipco(const QwBmffBox *box, size_t *out_ispe_count)
{
    size_t position = 0u;
    size_t ispe_count = 0u;
    if (!box || !out_ispe_count) return 0;
    while (position < box->payload_size) {
        QwBmffBox property;
        if (!bmff_read_box(box->data, box->payload_size, position, &property))
            return 0;
        if (property.type == bmff_fourcc("ispe")) {
            if (property.payload_size != 12u || property.data[0] != 0u ||
                bmff_fullbox_flags(property.data) != 0u ||
                !qw_read_u32_be(property.data + 4u) ||
                !qw_read_u32_be(property.data + 8u)) {
                return 0;
            }
            ++ispe_count;
        }
        position = property.next;
    }
    *out_ispe_count = ispe_count;
    return position == box->payload_size;
}

static int bmff_parse_ipma(const QwBmffBox *box)
{
    size_t position = 8u;
    uint8_t version;
    uint32_t flags;
    uint32_t entry_count;
    if (!box || box->payload_size < 8u) return 0;
    version = box->data[0];
    flags = bmff_fullbox_flags(box->data);
    if (version > 1u || (flags & ~1u) != 0u) return 0;
    entry_count = qw_read_u32_be(box->data + 4u);
    if (!entry_count) return 0;
    for (uint32_t item = 0u; item < entry_count; ++item) {
        uint8_t association_count;
        size_t item_id_size = version == 0u ? 2u : 4u;
        if (!bmff_skip_bytes(box->payload_size, &position, item_id_size) ||
            position >= box->payload_size) return 0;
        association_count = box->data[position++];
        if (!bmff_skip_bytes(box->payload_size, &position,
                             (size_t)association_count *
                                 ((flags & 1u) ? 2u : 1u))) {
            return 0;
        }
    }
    return position == box->payload_size;
}

static int bmff_parse_iprp(const QwBmffBox *box)
{
    size_t position = 0u;
    size_t ispe_count = 0u;
    int has_ipco = 0;
    int has_ipma = 0;
    if (!box) return 0;
    while (position < box->payload_size) {
        QwBmffBox child;
        if (!bmff_read_box(box->data, box->payload_size, position, &child))
            return 0;
        if (child.type == bmff_fourcc("ipco")) {
            if (has_ipco || !bmff_parse_ipco(&child, &ispe_count)) return 0;
            has_ipco = 1;
        } else if (child.type == bmff_fourcc("ipma")) {
            if (has_ipma || !bmff_parse_ipma(&child)) return 0;
            has_ipma = 1;
        }
        position = child.next;
    }
    return position == box->payload_size && has_ipco && has_ipma &&
           ispe_count != 0u;
}

static int bmff_parse_meta(const QwBmffBox *box, int *out_has_idat)
{
    size_t position = 4u;
    int has_hdlr = 0;
    int has_pitm = 0;
    int has_iloc = 0;
    int has_iinf = 0;
    int has_iprp = 0;
    int has_idat = 0;
    if (!box || !out_has_idat || box->payload_size < 4u ||
        box->data[0] != 0u || bmff_fullbox_flags(box->data) != 0u) {
        return 0;
    }
    while (position < box->payload_size) {
        QwBmffBox child;
        if (!bmff_read_box(box->data, box->payload_size, position, &child))
            return 0;
        if (child.type == bmff_fourcc("hdlr")) {
            if (has_hdlr || !bmff_parse_hdlr(&child)) return 0;
            has_hdlr = 1;
        } else if (child.type == bmff_fourcc("pitm")) {
            if (has_pitm || !bmff_parse_pitm(&child)) return 0;
            has_pitm = 1;
        } else if (child.type == bmff_fourcc("iloc")) {
            if (has_iloc || !bmff_parse_iloc(&child)) return 0;
            has_iloc = 1;
        } else if (child.type == bmff_fourcc("iinf")) {
            if (has_iinf || !bmff_parse_iinf(&child)) return 0;
            has_iinf = 1;
        } else if (child.type == bmff_fourcc("iprp")) {
            if (has_iprp || !bmff_parse_iprp(&child)) return 0;
            has_iprp = 1;
        } else if (child.type == bmff_fourcc("idat")) {
            if (has_idat || child.payload_size == 0u) return 0;
            has_idat = 1;
        }
        position = child.next;
    }
    if (position != box->payload_size || !has_hdlr || !has_pitm ||
        !has_iloc || !has_iinf || !has_iprp) {
        return 0;
    }
    *out_has_idat = has_idat;
    return 1;
}

static int heif_brand_matches(const uint8_t *brand, const char *text)
{
    return memcmp(brand, text, 4u) == 0;
}

static int heif_parse(const void *data, size_t size, int want_avif)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwBmffBox ftyp;
    uint64_t brands_size;
    size_t position;
    int avif_brand = 0;
    int heic_brand = 0;
    int generic_brand = 0;
    int has_meta = 0;
    int has_idat = 0;
    int has_mdat = 0;

    if (!bytes || !bmff_read_box(bytes, size, 0u, &ftyp) ||
        ftyp.type != bmff_fourcc("ftyp") || ftyp.payload_size < 8u ||
        ((ftyp.payload_size - 8u) % 4u) != 0u) {
        return 0;
    }
    brands_size = (uint64_t)ftyp.payload_size - 8u;
    avif_brand = heif_brand_matches(ftyp.data, "avif") ||
                 heif_brand_matches(ftyp.data, "avis");
    heic_brand = heif_brand_matches(ftyp.data, "heic") ||
                 heif_brand_matches(ftyp.data, "heix") ||
                 heif_brand_matches(ftyp.data, "hevc") ||
                 heif_brand_matches(ftyp.data, "hevx") ||
                 heif_brand_matches(ftyp.data, "heim") ||
                 heif_brand_matches(ftyp.data, "heis") ||
                 heif_brand_matches(ftyp.data, "hevm") ||
                 heif_brand_matches(ftyp.data, "hevs") ||
                 heif_brand_matches(ftyp.data, "heif");
    generic_brand = heif_brand_matches(ftyp.data, "mif1") ||
                    heif_brand_matches(ftyp.data, "msf1");
    for (size_t offset = 8u; (uint64_t)(offset - 8u) < brands_size;
         offset += 4u) {
        const uint8_t *brand = ftyp.data + offset;
        avif_brand |= heif_brand_matches(brand, "avif") ||
                      heif_brand_matches(brand, "avis");
        heic_brand |= heif_brand_matches(brand, "heic") ||
                      heif_brand_matches(brand, "heix") ||
                      heif_brand_matches(brand, "hevc") ||
                      heif_brand_matches(brand, "hevx") ||
                      heif_brand_matches(brand, "heim") ||
                      heif_brand_matches(brand, "heis") ||
                      heif_brand_matches(brand, "hevm") ||
                      heif_brand_matches(brand, "hevs") ||
                      heif_brand_matches(brand, "heif");
        generic_brand |= heif_brand_matches(brand, "mif1") ||
                         heif_brand_matches(brand, "msf1");
    }
    if ((want_avif && !avif_brand) ||
        (!want_avif && avif_brand) ||
        (!want_avif && !heic_brand && !generic_brand)) {
        return 0;
    }

    position = ftyp.next;
    while (position < size) {
        QwBmffBox box;
        if (!bmff_read_box(bytes, size, position, &box) ||
            box.type == bmff_fourcc("ftyp")) {
            return 0;
        }
        if (box.type == bmff_fourcc("meta")) {
            int meta_idat = 0;
            if (has_meta || !bmff_parse_meta(&box, &meta_idat)) return 0;
            has_meta = 1;
            has_idat = meta_idat;
        } else if (box.type == bmff_fourcc("mdat")) {
            if (box.payload_size == 0u) return 0;
            has_mdat = 1;
        }
        position = box.next;
    }
    return position == size && has_meta && (has_mdat || has_idat);
}

int qw_avif_probe(const void *data, size_t size)
{
    return heif_parse(data, size, 1);
}

int qw_heic_probe(const void *data, size_t size)
{
    return heif_parse(data, size, 0);
}
