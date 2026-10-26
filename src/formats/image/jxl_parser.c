#include "image_internal.h"

#include "../../core/byte_utils.h"

#include <string.h>

typedef struct QwJxlBox {
    const uint8_t *data;
    size_t payload_size;
    size_t next;
    uint32_t type;
} QwJxlBox;

static uint32_t jxl_fourcc(const char value[4])
{
    return ((uint32_t)(uint8_t)value[0] << 24) |
           ((uint32_t)(uint8_t)value[1] << 16) |
           ((uint32_t)(uint8_t)value[2] << 8) |
           (uint32_t)(uint8_t)value[3];
}

static int jxl_read_box(const uint8_t *bytes,
                        size_t size,
                        size_t position,
                        QwJxlBox *box)
{
    uint64_t box_size;
    size_t header_size = 8u;
    if (!bytes || !box || position > size || size - position < 8u)
        return 0;
    box_size = qw_read_u32_be(bytes + position);
    box->type = qw_read_u32_be(bytes + position + 4u);
    if (box_size == 1u) {
        if (size - position < 16u) return 0;
        box_size = qw_read_u64_be(bytes + position + 8u);
        header_size = 16u;
    } else if (box_size == 0u) {
        box_size = (uint64_t)(size - position);
    }
    if (box->type == jxl_fourcc("uuid")) {
        if (header_size > SIZE_MAX - 16u) return 0;
        header_size += 16u;
    }
    if (box_size < header_size || box_size > (uint64_t)(size - position) ||
        box_size > SIZE_MAX) {
        return 0;
    }
    box->data = bytes + position + header_size;
    box->payload_size = (size_t)box_size - header_size;
    box->next = position + (size_t)box_size;
    return 1;
}

static int jxl_parse_container(const uint8_t *bytes, size_t size)
{
    static const uint8_t signature[12] = {
        0x00u, 0x00u, 0x00u, 0x0cu, 'J', 'X', 'L', ' ',
        0x0du, 0x0au, 0x87u, 0x0au
    };
    uint32_t next_partial_index = 0u;
    size_t position = 32u;
    int has_codestream = 0;
    int has_jxlc = 0;
    int has_jxlp = 0;
    int last_partial_seen = 0;

    if (size < 32u || memcmp(bytes, signature, sizeof(signature)) != 0)
        return 0;
    if (qw_read_u32_be(bytes + 12u) != 20u ||
        qw_read_u32_be(bytes + 16u) != jxl_fourcc("ftyp") ||
        memcmp(bytes + 20u, "jxl ", 4u) != 0 ||
        memcmp(bytes + 28u, "jxl ", 4u) != 0) {
        return 0;
    }

    while (position < size) {
        QwJxlBox box;
        if (!jxl_read_box(bytes, size, position, &box)) return 0;
        if (box.type == jxl_fourcc("JXL ") ||
            box.type == jxl_fourcc("ftyp")) {
            return 0;
        } else if (box.type == jxl_fourcc("jxlc")) {
            if (has_codestream || box.payload_size < 2u ||
                box.data[0] != 0xffu || box.data[1] != 0x0au) {
                return 0;
            }
            has_codestream = 1;
            has_jxlc = 1;
        } else if (box.type == jxl_fourcc("jxlp")) {
            uint32_t sequence;
            uint32_t index;
            int is_last;
            if (has_jxlc || box.payload_size < 5u || last_partial_seen)
                return 0;
            sequence = qw_read_u32_be(box.data);
            index = sequence & 0x7fffffffu;
            is_last = (sequence & 0x80000000u) != 0u;
            if (index != next_partial_index) return 0;
            if (index == 0u &&
                (box.payload_size < 6u || box.data[4] != 0xffu ||
                 box.data[5] != 0x0au)) {
                return 0;
            }
            if (next_partial_index == 0x7fffffffu && !is_last) return 0;
            ++next_partial_index;
            has_codestream = 1;
            has_jxlp = 1;
            last_partial_seen = is_last;
        }
        position = box.next;
    }
    if (position != size || !has_codestream || (has_jxlc && has_jxlp) ||
        (has_jxlp && !last_partial_seen)) {
        return 0;
    }
    return 1;
}

int qw_jxl_probe(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    if (!bytes || size < 2u) return 0;
    if (bytes[0] == 0xffu && bytes[1] == 0x0au) return 1;
    return jxl_parse_container(bytes, size);
}
