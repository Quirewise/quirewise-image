#include <quirewise/compression.h>
#include <quirewise/image.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 0; \
    } \
} while (0)

static void write_be32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static int append_be32(uint8_t *output, size_t capacity, size_t *position,
                       uint32_t value)
{
    if (!output || !position || *position > capacity ||
        4u > capacity - *position) {
        return 0;
    }
    write_be32(output + *position, value);
    *position += 4u;
    return 1;
}

static int patch_be32(uint8_t *output, size_t size, size_t offset,
                      uint32_t value)
{
    if (!output || offset > size || 4u > size - offset) return 0;
    write_be32(output + offset, value);
    return 1;
}

static void write_le16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

static int append_png_chunk(uint8_t *output, size_t capacity, size_t *position,
                            const char type[4], const uint8_t *payload,
                            size_t payload_size)
{
    size_t chunk_size;
    uint32_t crc;
    if (!output || !position || !type ||
        (payload_size && !payload) || payload_size > UINT32_MAX ||
        payload_size > SIZE_MAX - 12u) {
        return 0;
    }
    chunk_size = payload_size + 12u;
    if (*position > capacity || chunk_size > capacity - *position) return 0;
    write_be32(output + *position, (uint32_t)payload_size);
    memcpy(output + *position + 4u, type, 4u);
    if (payload_size) {
        memcpy(output + *position + 8u, payload, payload_size);
    }
    crc = qw_compression_crc32(output + *position + 4u, payload_size + 4u);
    write_be32(output + *position + 8u + payload_size, crc);
    *position += chunk_size;
    return 1;
}

static void write_tiff_entry(uint8_t *entry, uint16_t tag, uint16_t type,
                             uint32_t count, uint32_t value)
{
    write_le16(entry, tag);
    write_le16(entry + 2u, type);
    write_le32(entry + 4u, count);
    write_le32(entry + 8u, value);
}

static int decode_rgb_fixture(const uint8_t *encoded, size_t encoded_size,
                              const char *format_name)
{
    static const uint8_t expected_rgb[3] = {0x12u, 0x34u, 0x56u};
    QwImage image = {0};
    int decoded = qw_image_decode(encoded, encoded_size, &image);
    if (decoded != QW_IMAGE_OK) {
        fprintf(stderr, "%s fixture did not decode: %s\n", format_name,
                qw_image_status_string(decoded));
        return 0;
    }
    if (image.width != 1u || image.height != 1u || image.size != 3u ||
        image.stride != 3u || image.format != QW_IMAGE_PIXEL_RGB24 ||
        memcmp(image.pixels, expected_rgb, sizeof(expected_rgb)) != 0) {
        fprintf(stderr, "%s fixture produced unexpected pixels\n", format_name);
        qw_image_free(&image);
        return 0;
    }
    qw_image_free(&image);
    return 1;
}

static int test_png_zlib_path(void)
{
    static const uint8_t signature[8] = {
        0x89u, 'P', 'N', 'G', '\r', '\n', 0x1au, '\n'
    };
    static const uint8_t ihdr[13] = {
        0u, 0u, 0u, 1u, 0u, 0u, 0u, 1u, 8u, 2u, 0u, 0u, 0u
    };
    static const uint8_t raw_pixels[4] = {0u, 0x12u, 0x34u, 0x56u};
    uint8_t compressed[64];
    uint8_t png[128];
    size_t compressed_size = 0u;
    size_t png_size = 0u;
    CHECK(qw_compression_deflate_zlib_stored(
        raw_pixels, sizeof(raw_pixels), compressed, sizeof(compressed),
        &compressed_size));
    memcpy(png, signature, sizeof(signature));
    png_size = sizeof(signature);
    CHECK(append_png_chunk(png, sizeof(png), &png_size, "IHDR",
                           ihdr, sizeof(ihdr)));
    CHECK(append_png_chunk(png, sizeof(png), &png_size, "IDAT",
                           compressed, compressed_size));
    CHECK(append_png_chunk(png, sizeof(png), &png_size, "IEND", NULL, 0u));
    return decode_rgb_fixture(png, png_size, "PNG");
}

static int test_xcf_zlib_tile_path(void)
{
    static const uint8_t magic[14] = {
        'g', 'i', 'm', 'p', ' ', 'x', 'c', 'f', ' ',
        'f', 'i', 'l', 'e', 0u
    };
    static const uint8_t raw_pixels[3] = {0x12u, 0x34u, 0x56u};
    uint8_t compressed[64];
    uint8_t xcf[256] = {0};
    size_t compressed_size = 0u;
    size_t position = 0u;
    size_t layer_pointer_position;
    size_t hierarchy_pointer_position;
    size_t level_pointer_position;
    size_t tile_pointer_position;
    uint32_t layer_offset;
    uint32_t hierarchy_offset;
    uint32_t level_offset;
    uint32_t tile_offset;
    CHECK(qw_compression_deflate_zlib_stored(
        raw_pixels, sizeof(raw_pixels), compressed, sizeof(compressed),
        &compressed_size));
    CHECK(sizeof(magic) <= sizeof(xcf));
    memcpy(xcf, magic, sizeof(magic));
    position = sizeof(magic);
    CHECK(append_be32(xcf, sizeof(xcf), &position, 1u)); /* Image width */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 1u)); /* Image height */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u)); /* RGB base type */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 17u)); /* Compression prop */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 1u));
    CHECK(position < sizeof(xcf));
    xcf[position++] = 2u; /* Zlib-compressed tile */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u)); /* Property end */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u));

    layer_pointer_position = position;
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u)); /* Layer offset */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u)); /* Layer list end */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u)); /* Selected paths */
    layer_offset = (uint32_t)position;
    CHECK(patch_be32(xcf, sizeof(xcf), layer_pointer_position, layer_offset));

    CHECK(append_be32(xcf, sizeof(xcf), &position, 1u)); /* Layer width */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 1u)); /* Layer height */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u)); /* RGB layer */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u)); /* Empty name */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u)); /* Property end */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u));
    hierarchy_pointer_position = position;
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u)); /* Hierarchy */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u)); /* Mask */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u)); /* Effects end */

    hierarchy_offset = (uint32_t)position;
    CHECK(patch_be32(xcf, sizeof(xcf), hierarchy_pointer_position,
                     hierarchy_offset));
    CHECK(append_be32(xcf, sizeof(xcf), &position, 1u)); /* Hierarchy width */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 1u)); /* Hierarchy height */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 3u)); /* Bytes per pixel */
    level_pointer_position = position;
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u)); /* First level */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u)); /* Level list end */

    level_offset = (uint32_t)position;
    CHECK(patch_be32(xcf, sizeof(xcf), level_pointer_position, level_offset));
    CHECK(append_be32(xcf, sizeof(xcf), &position, 1u)); /* Level width */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 1u)); /* Level height */
    tile_pointer_position = position;
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u)); /* Tile offset */
    CHECK(append_be32(xcf, sizeof(xcf), &position, 0u)); /* Tile list end */

    tile_offset = (uint32_t)position;
    CHECK(patch_be32(xcf, sizeof(xcf), tile_pointer_position, tile_offset));
    CHECK(compressed_size <= sizeof(xcf) - position);
    memcpy(xcf + position, compressed, compressed_size);
    position += compressed_size;
    return decode_rgb_fixture(xcf, position, "XCF zlib tile");
}

static int test_tiff_deflate_variant(int raw_deflate)
{
    enum {
        TIFF_IFD_OFFSET = 8u,
        TIFF_ENTRY_COUNT = 10u,
        TIFF_IFD_END = TIFF_IFD_OFFSET + 2u + TIFF_ENTRY_COUNT * 12u + 4u,
        TIFF_BITS_OFFSET = TIFF_IFD_END,
        TIFF_STRIP_OFFSET = TIFF_BITS_OFFSET + 6u
    };
    static const uint8_t raw_pixels[3] = {0x12u, 0x34u, 0x56u};
    uint8_t compressed[64];
    uint8_t tiff[256] = {0};
    size_t compressed_size = 0u;
    size_t tiff_size;
    uint8_t *entry;
    if (raw_deflate) {
        CHECK(qw_compression_deflate_raw_stored(
            raw_pixels, sizeof(raw_pixels), compressed, sizeof(compressed),
            &compressed_size));
    } else {
        CHECK(qw_compression_deflate_zlib_stored(
            raw_pixels, sizeof(raw_pixels), compressed, sizeof(compressed),
            &compressed_size));
    }
    CHECK(compressed_size <= sizeof(tiff) - TIFF_STRIP_OFFSET);

    tiff[0] = 'I';
    tiff[1] = 'I';
    write_le16(tiff + 2u, 42u);
    write_le32(tiff + 4u, TIFF_IFD_OFFSET);
    write_le16(tiff + TIFF_IFD_OFFSET, TIFF_ENTRY_COUNT);
    entry = tiff + TIFF_IFD_OFFSET + 2u;
    write_tiff_entry(entry + 0u * 12u, 256u, 4u, 1u, 1u); /* Width */
    write_tiff_entry(entry + 1u * 12u, 257u, 4u, 1u, 1u); /* Height */
    write_tiff_entry(entry + 2u * 12u, 258u, 3u, 3u, TIFF_BITS_OFFSET);
    write_tiff_entry(entry + 3u * 12u, 259u, 3u, 1u, 8u); /* Deflate */
    write_tiff_entry(entry + 4u * 12u, 262u, 3u, 1u, 2u); /* RGB */
    write_tiff_entry(entry + 5u * 12u, 273u, 4u, 1u, TIFF_STRIP_OFFSET);
    write_tiff_entry(entry + 6u * 12u, 277u, 3u, 1u, 3u); /* Samples */
    write_tiff_entry(entry + 7u * 12u, 278u, 4u, 1u, 1u); /* Rows/strip */
    write_tiff_entry(entry + 8u * 12u, 279u, 4u, 1u,
                     (uint32_t)compressed_size);
    write_tiff_entry(entry + 9u * 12u, 284u, 3u, 1u, 1u); /* Chunky */
    write_le32(tiff + TIFF_IFD_OFFSET + 2u + TIFF_ENTRY_COUNT * 12u, 0u);
    write_le16(tiff + TIFF_BITS_OFFSET, 8u);
    write_le16(tiff + TIFF_BITS_OFFSET + 2u, 8u);
    write_le16(tiff + TIFF_BITS_OFFSET + 4u, 8u);
    memcpy(tiff + TIFF_STRIP_OFFSET, compressed, compressed_size);
    tiff_size = TIFF_STRIP_OFFSET + compressed_size;
    return decode_rgb_fixture(tiff, tiff_size,
                              raw_deflate ? "TIFF raw Deflate"
                                          : "TIFF zlib Deflate");
}

int main(void)
{
    if (!test_png_zlib_path()) return 1;
    if (!test_xcf_zlib_tile_path()) return 1;
    if (!test_tiff_deflate_variant(0)) return 1;
    if (!test_tiff_deflate_variant(1)) return 1;
    return 0;
}
