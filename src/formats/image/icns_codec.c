#include "image_internal.h"

#include "quirewise/status.h"

#include <stdint.h>
#include <string.h>

static int icns_parse(const void *data, size_t size, size_t *out_count)
{
    static const uint8_t png_signature[8] = {
        137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u
    };
    const uint8_t *bytes = (const uint8_t *)data;
    size_t offset = 8u;
    size_t count = 0u;
    uint32_t declared_size;

    if (!bytes || size < 16u || memcmp(bytes, "icns", 4u) != 0) return 0;
    declared_size = qw_read_u32_be(bytes + 4u);
    if (declared_size != size) return 0;
    while (offset < size) {
        uint32_t chunk_size;
        size_t payload_size;
        if (size - offset < 8u) return 0;
        chunk_size = qw_read_u32_be(bytes + offset + 4u);
        if (chunk_size < 8u || (size_t)chunk_size > size - offset) return 0;
        payload_size = (size_t)chunk_size - 8u;
        if (payload_size >= sizeof(png_signature) &&
            memcmp(bytes + offset + 8u, png_signature,
                   sizeof(png_signature)) == 0) {
            uint32_t width = 0u;
            uint32_t height = 0u;
            if (qw_png_dimensions(bytes + offset + 8u, payload_size,
                                  &width, &height) == QW_STATUS_OK) {
                ++count;
            }
        }
        offset += (size_t)chunk_size;
    }
    if (offset != size || !count) return 0;
    if (out_count) *out_count = count;
    return 1;
}

int qw_icns_probe(const void *data, size_t size)
{
    return icns_parse(data, size, NULL);
}

int qw_icns_page_count(const void *data, size_t size, size_t *out_count)
{
    if (!out_count) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_count = 0u;
    return icns_parse(data, size, out_count) ? QW_STATUS_OK : QW_STATUS_ERR_FORMAT;
}

int qw_icns_decode_page(const void *data,
                        size_t size,
                        size_t page_index,
                        QwPageImage *out_page)
{
    static const uint8_t png_signature[8] = {
        137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u
    };
    const uint8_t *bytes = (const uint8_t *)data;
    size_t offset = 8u;
    size_t current = 0u;
    size_t count = 0u;

    if (!data || !out_page) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (!icns_parse(data, size, &count) || page_index >= count) {
        return QW_STATUS_ERR_FORMAT;
    }
    memset(out_page, 0, sizeof(*out_page));
    while (offset < size) {
        uint32_t chunk_size = qw_read_u32_be(bytes + offset + 4u);
        size_t payload_size = (size_t)chunk_size - 8u;
        if (payload_size >= sizeof(png_signature) &&
            memcmp(bytes + offset + 8u, png_signature,
                   sizeof(png_signature)) == 0) {
            uint32_t width = 0u;
            uint32_t height = 0u;
            if (qw_png_dimensions(bytes + offset + 8u, payload_size,
                                  &width, &height) == QW_STATUS_OK) {
                if (current == page_index) {
                    return qw_image_decode_page(bytes + offset + 8u,
                                                payload_size, out_page);
                }
                ++current;
            }
        }
        offset += (size_t)chunk_size;
    }
    return QW_STATUS_ERR_FORMAT;
}
