#include "image_internal.h"
#include "../../core/byte_utils.h"

#include <stdint.h>
#include <string.h>

static const uint8_t apng_png_signature[8] = {
    0x89u, 'P', 'N', 'G', '\r', '\n', 0x1au, '\n'
};


int qw_apng_probe(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t position = sizeof(apng_png_signature);
    int have_ihdr = 0;
    int have_actl = 0;
    int have_idat = 0;
    int have_iend = 0;
    if (!bytes || size < sizeof(apng_png_signature) ||
        memcmp(bytes, apng_png_signature, sizeof(apng_png_signature)) != 0) {
        return 0;
    }
    while (position <= size && size - position >= 12u) {
        uint32_t length = qw_read_u32_be(bytes + position);
        const uint8_t *type = bytes + position + 4u;
        size_t chunk_size;
        if ((size_t)length > size - position - 12u) return 0;
        chunk_size = (size_t)length + 12u;
        if (!have_ihdr) {
            if (memcmp(type, "IHDR", 4u) != 0 || length != 13u) return 0;
            have_ihdr = 1;
        } else if (memcmp(type, "acTL", 4u) == 0) {
            if (have_actl || have_idat || length != 8u ||
                !qw_read_u32_be(bytes + position + 8u)) return 0;
            have_actl = 1;
        } else if (memcmp(type, "IDAT", 4u) == 0) {
            have_idat = 1;
        } else if (memcmp(type, "IEND", 4u) == 0) {
            if (length != 0u) return 0;
            have_iend = 1;
            position += chunk_size;
            break;
        }
        position += chunk_size;
    }
    return have_ihdr && have_actl && have_idat && have_iend && position == size;
}
