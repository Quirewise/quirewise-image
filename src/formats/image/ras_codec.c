#include "image_internal.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RAS_HEADER_SIZE 32u
#define RAS_MAGIC UINT32_C(0x59a66a95)
#define RAS_MAX_PIXELS 20000000u
#define RAS_TYPE_FORMAT_RGB 3u

typedef struct QwRasHeader {
    uint32_t width;
    uint32_t height;
    size_t row_bytes;
    size_t pixel_count;
    size_t data_size;
} QwRasHeader;


static int ras_read_header(const void *data, size_t size, QwRasHeader *header)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t depth;
    uint32_t declared_length;
    uint64_t pixels;
    uint64_t row_bits;
    uint64_t row_bytes;
    if (!bytes || !header || size < RAS_HEADER_SIZE ||
        qw_read_u32_be(bytes) != RAS_MAGIC) {
        return 0;
    }
    header->width = qw_read_u32_be(bytes + 4u);
    header->height = qw_read_u32_be(bytes + 8u);
    depth = qw_read_u32_be(bytes + 12u);
    declared_length = qw_read_u32_be(bytes + 16u);
    pixels = (uint64_t)header->width * header->height;
    row_bits = (uint64_t)header->width * depth;
    row_bytes = ((row_bits + 15u) / 16u) * 2u;
    if (!header->width || !header->height || depth != 24u ||
        qw_read_u32_be(bytes + 20u) != RAS_TYPE_FORMAT_RGB ||
        qw_read_u32_be(bytes + 24u) != 0u || qw_read_u32_be(bytes + 28u) != 0u ||
        pixels == 0u || pixels > RAS_MAX_PIXELS || pixels > SIZE_MAX / 3u ||
        row_bytes > SIZE_MAX || row_bytes * header->height > SIZE_MAX) {
        return 0;
    }
    header->pixel_count = (size_t)pixels;
    header->row_bytes = (size_t)row_bytes;
    header->data_size = header->row_bytes * header->height;
    return declared_length == header->data_size &&
           header->data_size <= size - RAS_HEADER_SIZE &&
           size == RAS_HEADER_SIZE + header->data_size;
}

int qw_ras_probe(const void *data, size_t size)
{
    QwRasHeader header;
    return ras_read_header(data, size, &header);
}

int qw_ras_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwRasHeader header;
    uint8_t *pixels;
    size_t output_size;
    if (!out_page || !ras_read_header(data, size, &header)) {
        return QW_STATUS_ERR_FORMAT;
    }
    memset(out_page, 0, sizeof(*out_page));
    output_size = header.pixel_count * 3u;
    pixels = (uint8_t *)malloc(output_size);
    if (!pixels) return QW_STATUS_ERR_OUT_OF_MEMORY;
    for (size_t y = 0u; y < header.height; ++y) {
        const uint8_t *source = bytes + RAS_HEADER_SIZE + y * header.row_bytes;
        memcpy(pixels + y * header.width * 3u, source,
               (size_t)header.width * 3u);
    }
    out_page->data = pixels;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = header.width;
    out_page->height = header.height;
    return QW_STATUS_OK;
}
