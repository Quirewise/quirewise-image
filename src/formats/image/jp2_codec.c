#include "image_internal.h"

#include <stdint.h>
#include <string.h>

static const uint8_t qw_jp2_signature[12] = {
    0x00u, 0x00u, 0x00u, 0x0cu, 0x6au, 0x50u, 0x20u, 0x20u,
    0x0du, 0x0au, 0x87u, 0x0au
};

static int qw_jp2_is_container(const uint8_t *data, size_t size)
{
    return data && size >= sizeof(qw_jp2_signature) &&
           memcmp(data, qw_jp2_signature, sizeof(qw_jp2_signature)) == 0;
}

int qw_jp2_probe(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    if (!bytes || size < 4u) return 0;
    if (size < sizeof(qw_jp2_signature) &&
        memcmp(bytes, qw_jp2_signature, size) == 0) {
        return 1;
    }
    return qw_jp2_is_container(bytes, size) ||
           (bytes[0] == 0xffu && bytes[1] == 0x4fu &&
            bytes[2] == 0xffu && bytes[3] == 0x51u);
}

int qw_jp2_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;

    if (!bytes || !size || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    if (!qw_jp2_probe(bytes, size)) return QW_STATUS_ERR_UNSUPPORTED;

    return qw_jp2_decode_internal_gray8(bytes, size, out_page);
}