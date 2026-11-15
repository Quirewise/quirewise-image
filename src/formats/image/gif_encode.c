/* GIF89a encoder.
 *
 * Single-frame still images with a global 256-entry color table:
 * - Gray8 inputs use the identity gray palette, so the round-trip is exact.
 * - RGB24 inputs use a fixed 3:3:2 color cube (256 colors) with bin centers.
 *
 * Pixel indices are compressed with the standard GIF variable-width LZW
 * variant (clear code 256, end code 257, 9..12 bit codes) and emitted in
 * length-prefixed sub-blocks. No transparency, animation, interlace or local
 * color tables are written.
 */

#include "quirewise/image.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GIF_MAX_DIMENSION ((size_t)65535u)
#define GIF_OUTPUT_LIMIT ((size_t)64u * 1024u * 1024u)
#define GIF_DICT_SIZE 4096
#define GIF_HASH_SIZE 5003
#define GIF_CLEAR_CODE 256
#define GIF_END_CODE 257

typedef struct GifBuffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    int failed;
} GifBuffer;

typedef struct GifBits {
    GifBuffer *buffer;
    uint32_t accumulator;
    int bits;
} GifBits;

static int gif_reserve(GifBuffer *buffer, size_t additional)
{
    size_t needed;
    size_t capacity;
    uint8_t *grown;

    if (buffer->failed) return 0;
    if (buffer->size > GIF_OUTPUT_LIMIT ||
        additional > GIF_OUTPUT_LIMIT - buffer->size) {
        buffer->failed = 1;
        return 0;
    }
    needed = buffer->size + additional;
    if (needed <= buffer->capacity) return 1;
    capacity = buffer->capacity ? buffer->capacity : 16384u;
    while (capacity < needed) {
        if (capacity > GIF_OUTPUT_LIMIT / 2u) {
            capacity = GIF_OUTPUT_LIMIT;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < needed) {
        buffer->failed = 1;
        return 0;
    }
    grown = (uint8_t *)realloc(buffer->data, capacity);
    if (!grown) {
        buffer->failed = 1;
        return 0;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
    return 1;
}

static int gif_put_byte(GifBuffer *buffer, uint8_t value)
{
    if (!gif_reserve(buffer, 1u)) return 0;
    buffer->data[buffer->size++] = value;
    return 1;
}

static int gif_put_u16(GifBuffer *buffer, uint32_t value)
{
    return gif_put_byte(buffer, (uint8_t)(value & 0xffu)) &&
           gif_put_byte(buffer, (uint8_t)((value >> 8) & 0xffu));
}

static int gif_put_bits(GifBits *writer, uint32_t code, int count)
{
    writer->accumulator |= code << writer->bits;
    writer->bits += count;
    while (writer->bits >= 8) {
        if (!gif_put_byte(writer->buffer,
                          (uint8_t)(writer->accumulator & 0xffu)))
            return 0;
        writer->accumulator >>= 8;
        writer->bits -= 8;
    }
    return 1;
}

static int gif_flush_bits(GifBits *writer)
{
    if (writer->bits > 0) {
        if (!gif_put_byte(writer->buffer,
                          (uint8_t)(writer->accumulator & 0xffu)))
            return 0;
        writer->accumulator = 0u;
        writer->bits = 0;
    }
    return 1;
}

static unsigned gif_hash(uint32_t key)
{
    return (unsigned)((key * 2654435761u) % GIF_HASH_SIZE);
}

static void gif_palette(const QwImage *image, uint8_t palette[768])
{
    unsigned index;

    if (image->format == QW_IMAGE_PIXEL_GRAY8) {
        for (index = 0u; index < 256u; ++index) {
            palette[index * 3u] = (uint8_t)index;
            palette[index * 3u + 1u] = (uint8_t)index;
            palette[index * 3u + 2u] = (uint8_t)index;
        }
        return;
    }
    for (index = 0u; index < 256u; ++index) {
        unsigned red = (index >> 5) & 0x07u;
        unsigned green = (index >> 2) & 0x07u;
        unsigned blue = index & 0x03u;

        palette[index * 3u] = (uint8_t)(red * 32u + 16u);
        palette[index * 3u + 1u] = (uint8_t)(green * 32u + 16u);
        palette[index * 3u + 2u] = (uint8_t)(blue * 64u + 32u);
    }
}

static unsigned gif_index(const QwImage *image, size_t pixel)
{
    if (image->format == QW_IMAGE_PIXEL_GRAY8) return image->pixels[pixel];
    {
        const uint8_t *source = image->pixels + pixel * 3u;
        return ((unsigned)(source[0] >> 5) << 5) |
               ((unsigned)(source[1] >> 5) << 2) |
               (unsigned)(source[2] >> 6);
    }
}

static int gif_lzw_compress(const uint8_t *indices, size_t count,
                            GifBuffer *out)
{
    static uint32_t keys[GIF_HASH_SIZE];
    static uint16_t codes[GIF_HASH_SIZE];
    GifBits writer;
    unsigned next = 258u;
    int code_size = 9;
    size_t position;
    unsigned prefix;

    memset(&writer, 0, sizeof(writer));
    writer.buffer = out;
    memset(keys, 0, sizeof(keys));
    if (!gif_put_bits(&writer, GIF_CLEAR_CODE, 9)) return 0;
    if (!count) return gif_put_bits(&writer, GIF_END_CODE, 9);
    prefix = indices[0];
    for (position = 1u; position < count; ++position) {
        unsigned suffix = indices[position];
        uint32_t key = (prefix << 8) | suffix;
        unsigned slot = gif_hash(key);

        for (;;) {
            if (keys[slot] == 0u) break;
            if (keys[slot] == key + 1u) {
                prefix = codes[slot];
                break;
            }
            slot = (slot + 1u) % GIF_HASH_SIZE;
        }
        if (keys[slot] != 0u) continue; /* dictionary hit: prefix updated */
        if (!gif_put_bits(&writer, prefix, code_size)) return 0;
        if (next < GIF_DICT_SIZE) {
            keys[slot] = key + 1u;
            codes[slot] = (uint16_t)next;
            ++next;
            if (next == (1u << (unsigned)code_size) + 1u &&
                code_size < 12)
                ++code_size;
        } else {
            if (!gif_put_bits(&writer, GIF_CLEAR_CODE, code_size)) return 0;
            memset(keys, 0, sizeof(keys));
            next = 258u;
            code_size = 9;
        }
        prefix = suffix;
    }
    if (!gif_put_bits(&writer, prefix, code_size)) return 0;
    if (!gif_put_bits(&writer, GIF_END_CODE, code_size)) return 0;
    return gif_flush_bits(&writer);
}

int qw_image_gif_encode(const QwImage *image, uint8_t **out_data,
                        size_t *out_size)
{
    uint8_t palette[768];
    uint8_t *indices;
    GifBuffer output;
    GifBuffer compressed;
    size_t pixel_count;
    size_t pixel;
    size_t offset;
    size_t index;

    if (!image || !image->pixels || !image->width || !image->height ||
        (image->format != QW_IMAGE_PIXEL_GRAY8 &&
         image->format != QW_IMAGE_PIXEL_RGB24) ||
        !out_data || !out_size)
        return (int)QW_IMAGE_ERR_INVALID_ARGUMENT;
    if (image->width > GIF_MAX_DIMENSION || image->height > GIF_MAX_DIMENSION)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    if (image->width > SIZE_MAX / image->height)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    pixel_count = (size_t)image->width * image->height;
    indices = (uint8_t *)malloc(pixel_count ? pixel_count : 1u);
    if (!indices) return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    for (pixel = 0u; pixel < pixel_count; ++pixel)
        indices[pixel] = (uint8_t)gif_index(image, pixel);
    gif_palette(image, palette);
    memset(&output, 0, sizeof(output));
    memset(&compressed, 0, sizeof(compressed));
    /* Header + logical screen descriptor + global color table. */
    if (!gif_put_byte(&output, 'G') || !gif_put_byte(&output, 'I') ||
        !gif_put_byte(&output, 'F') || !gif_put_byte(&output, '8') ||
        !gif_put_byte(&output, '9') || !gif_put_byte(&output, 'a') ||
        !gif_put_u16(&output, (uint32_t)image->width) ||
        !gif_put_u16(&output, (uint32_t)image->height) ||
        !gif_put_byte(&output, 0xf7u) /* GCT 256, 8-bit color */ ||
        !gif_put_byte(&output, 0u) || !gif_put_byte(&output, 0u))
        goto out_of_memory;
    if (!gif_reserve(&output, sizeof(palette))) goto out_of_memory;
    memcpy(output.data + output.size, palette, sizeof(palette));
    output.size += sizeof(palette);
    /* Image descriptor covering the whole canvas, no local table. */
    if (!gif_put_byte(&output, 0x2cu) || !gif_put_u16(&output, 0u) ||
        !gif_put_u16(&output, 0u) ||
        !gif_put_u16(&output, (uint32_t)image->width) ||
        !gif_put_u16(&output, (uint32_t)image->height) ||
        !gif_put_byte(&output, 0u))
        goto out_of_memory;
    if (!gif_put_byte(&output, 8u)) /* LZW minimum code size */
        goto out_of_memory;
    if (!gif_lzw_compress(indices, pixel_count, &compressed))
        goto out_of_memory;
    for (offset = 0u; offset < compressed.size; offset += 255u) {
        size_t block = compressed.size - offset;

        if (block > 255u) block = 255u;
        if (!gif_put_byte(&output, (uint8_t)block) ||
            !gif_reserve(&output, block))
            goto out_of_memory;
        memcpy(output.data + output.size, compressed.data + offset, block);
        output.size += block;
    }
    if (!gif_put_byte(&output, 0u) || !gif_put_byte(&output, 0x3bu))
        goto out_of_memory;
    free(indices);
    free(compressed.data);
    *out_data = output.data;
    *out_size = output.size;
    return (int)QW_IMAGE_OK;

out_of_memory:
    free(indices);
    free(output.data);
    free(compressed.data);
    return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
}
