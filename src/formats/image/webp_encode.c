/* WebP lossless (VP8L) encoder.
 *
 * The stream is deliberately simple but spec-conforming: no transforms, no
 * color cache, no LZ77 back-references and one Huffman group. Every channel
 * (green, red, blue, alpha) uses a complete 256-symbol code with 8-bit codes,
 * transmitted as literal code lengths plus repeat codes; the unused distance
 * tree is a one-symbol simple code. The result is lossless but unoptimized
 * (roughly four bytes per pixel), which is the point: the package can now
 * write WebP instead of reporting UNSUPPORTED.
 *
 * VP8L bit order is LSB-first; canonical Huffman codes travel most
 * significant bit first, so each code is emitted through a bit reversal.
 */

#include "quirewise/image.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WEBP_MAX_DIMENSION ((size_t)16383u)
#define WEBP_OUTPUT_LIMIT ((size_t)128u * 1024u * 1024u)

typedef struct WebpBitWriter {
    uint8_t *data;
    size_t size;
    size_t capacity;
    unsigned bit; /* bits used in the last byte (0..7) */
    int failed;
} WebpBitWriter;

static int webp_reserve(WebpBitWriter *writer, size_t additional)
{
    size_t needed;
    size_t capacity;
    uint8_t *grown;

    if (writer->failed) return 0;
    if (writer->size > WEBP_OUTPUT_LIMIT ||
        additional > WEBP_OUTPUT_LIMIT - writer->size) {
        writer->failed = 1;
        return 0;
    }
    needed = writer->size + additional;
    if (needed <= writer->capacity) return 1;
    capacity = writer->capacity ? writer->capacity : 8192u;
    while (capacity < needed) {
        if (capacity > WEBP_OUTPUT_LIMIT / 2u) {
            capacity = WEBP_OUTPUT_LIMIT;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < needed) {
        writer->failed = 1;
        return 0;
    }
    grown = (uint8_t *)realloc(writer->data, capacity);
    if (!grown) {
        writer->failed = 1;
        return 0;
    }
    writer->data = grown;
    writer->capacity = capacity;
    return 1;
}

static int webp_put_bits(WebpBitWriter *writer, uint32_t value, int count)
{
    int index;

    for (index = 0; index < count; ++index) {
        if (writer->bit == 0u) {
            if (!webp_reserve(writer, 1u)) return 0;
            writer->data[writer->size] = 0u;
        }
        if ((value >> index) & 1u)
            writer->data[writer->size] |= (uint8_t)(1u << writer->bit);
        ++writer->bit;
        if (writer->bit == 8u) {
            writer->bit = 0u;
            ++writer->size;
        }
    }
    return 1;
}

static uint32_t webp_reverse_bits(uint32_t value, int count)
{
    uint32_t reversed = 0u;
    int index;

    for (index = 0; index < count; ++index) {
        reversed = (reversed << 1) | (value & 1u);
        value >>= 1;
    }
    return reversed;
}

/* Emit a canonical Huffman code (MSB-first) through the LSB-first writer. */
static int webp_put_code(WebpBitWriter *writer, uint32_t code, int count)
{
    return webp_put_bits(writer, webp_reverse_bits(code, count), count);
}

/* Code-length alphabet codes used by the fixed length tables:
 * symbol 8 -> 1 bit code 0; symbol 16 -> 2-bit code 2; symbol 18 -> 2-bit
 * code 3. */
static int webp_put_code_length_symbol(WebpBitWriter *writer, int symbol)
{
    if (symbol == 8) return webp_put_code(writer, 0u, 1);
    if (symbol == 16) return webp_put_code(writer, 2u, 2);
    if (symbol == 18) return webp_put_code(writer, 3u, 2);
    return 0;
}

/* Emit one normal code whose first `literal_count` symbols all have length 8
 * and the remaining symbols have length 0. */
static int webp_put_fixed_code(WebpBitWriter *writer, int alphabet_size,
                               int literal_count)
{
    static const uint8_t order[19] = {17, 18, 0, 1, 2,  3,  4,  5,  16, 6,
                                      7,  8,  9, 10, 11, 12, 13, 14, 15};
    int lengths[19];
    int index;
    int remaining;
    int tail;

    /* Normal (not simple) code. */
    if (!webp_put_bits(writer, 0u, 1)) return 0;
    /* num_code_lengths = 4 + read(4) = 12. */
    if (!webp_put_bits(writer, 8u, 4)) return 0;
    memset(lengths, 0, sizeof(lengths));
    lengths[8] = 1;
    lengths[16] = 2;
    lengths[18] = 2;
    for (index = 0; index < 12; ++index) {
        if (!webp_put_bits(writer, (uint32_t)lengths[order[index]], 3))
            return 0;
    }
    /* use_length = 0: explicit lengths for the whole alphabet. */
    if (!webp_put_bits(writer, 0u, 1)) return 0;
    /* One literal length 8. */
    if (!webp_put_code_length_symbol(writer, 8)) return 0;
    remaining = literal_count - 1;
    while (remaining > 0) {
        int repeat = remaining > 6 ? 6 : remaining;
        if (repeat < 3) {
            /* Repeats are 3..6; a 1-2 remainder is written as literals. */
            int extra;
            for (extra = 0; extra < repeat; ++extra) {
                if (!webp_put_code_length_symbol(writer, 8)) return 0;
            }
            remaining -= repeat;
            continue;
        }
        if (!webp_put_code_length_symbol(writer, 16) ||
            !webp_put_bits(writer, (uint32_t)(repeat - 3), 2))
            return 0;
        remaining -= repeat;
    }
    tail = alphabet_size - literal_count;
    while (tail > 0) {
        int repeat = tail > 138 ? 138 : tail;
        if (repeat < 11) {
            int extra;
            for (extra = 0; extra < repeat; ++extra) {
                if (!webp_put_code_length_symbol(writer, 0)) return 0;
            }
            tail -= repeat;
            continue;
        }
        if (!webp_put_code_length_symbol(writer, 18) ||
            !webp_put_bits(writer, (uint32_t)(repeat - 11), 7))
            return 0;
        tail -= repeat;
    }
    return 1;
}

/* A one-symbol simple code for the (unused) distance tree. */
static int webp_put_simple_symbol(WebpBitWriter *writer, int symbol)
{
    if (!webp_put_bits(writer, 1u, 1)) return 0; /* simple code */
    if (!webp_put_bits(writer, 0u, 1)) return 0; /* one symbol */
    if (!webp_put_bits(writer, 0u, 1)) return 0; /* 1-bit symbol */
    return webp_put_bits(writer, (uint32_t)symbol, 1);
}

int qw_image_webp_encode(const QwImage *image, uint8_t **out_data,
                         size_t *out_size)
{
    WebpBitWriter writer;
    size_t width;
    size_t height;
    size_t pixel;
    uint8_t *riff;
    size_t payload_size;
    size_t chunk_padded;
    size_t total;
    int status;

    if (!image || !image->pixels || !out_data || !out_size ||
        !image->width || !image->height ||
        (image->format != QW_IMAGE_PIXEL_GRAY8 &&
         image->format != QW_IMAGE_PIXEL_RGB24))
        return (int)QW_IMAGE_ERR_INVALID_ARGUMENT;
    width = image->width;
    height = image->height;
    if (width > WEBP_MAX_DIMENSION || height > WEBP_MAX_DIMENSION)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    if (width > SIZE_MAX / height ||
        width * height > WEBP_OUTPUT_LIMIT / 4u)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    memset(&writer, 0, sizeof(writer));
    /* VP8L header. */
    if (!webp_put_bits(&writer, 0x2fu, 8) ||
        !webp_put_bits(&writer, (uint32_t)(width - 1u), 14) ||
        !webp_put_bits(&writer, (uint32_t)(height - 1u), 14) ||
        !webp_put_bits(&writer, 1u, 1) /* alpha channel is written */ ||
        !webp_put_bits(&writer, 0u, 3) /* version */
        /* No transforms, no color cache, single Huffman group. */
        || !webp_put_bits(&writer, 0u, 1) || !webp_put_bits(&writer, 0u, 1) ||
        !webp_put_bits(&writer, 0u, 1))
        goto out_of_memory;
    /* Five codes: green (280), red (256), blue (256), alpha (256), distance. */
    if (!webp_put_fixed_code(&writer, 280, 256) ||
        !webp_put_fixed_code(&writer, 256, 256) ||
        !webp_put_fixed_code(&writer, 256, 256) ||
        !webp_put_fixed_code(&writer, 256, 256) ||
        !webp_put_simple_symbol(&writer, 0))
        goto out_of_memory;
    /* Pixels: green, red, blue, alpha; all code lengths are 8. */
    for (pixel = 0u; pixel < width * height; ++pixel) {
        uint8_t red;
        uint8_t green;
        uint8_t blue;

        if (image->format == QW_IMAGE_PIXEL_GRAY8) {
            red = image->pixels[pixel];
            green = red;
            blue = red;
        } else {
            const uint8_t *source = image->pixels + pixel * 3u;
            red = source[0];
            green = source[1];
            blue = source[2];
        }
        if (!webp_put_code(&writer, green, 8) ||
            !webp_put_code(&writer, red, 8) ||
            !webp_put_code(&writer, blue, 8) ||
            !webp_put_code(&writer, 255u, 8))
            goto out_of_memory;
        if (writer.failed) goto out_of_memory;
    }
    payload_size = writer.size + (writer.bit ? 1u : 0u);
    chunk_padded = payload_size + (payload_size & 1u);
    if (chunk_padded > SIZE_MAX - 20u) goto out_of_memory;
    total = 20u + chunk_padded;
    riff = (uint8_t *)malloc(total);
    if (!riff) goto out_of_memory;
    memset(riff, 0, total);
    memcpy(riff, "RIFF", 4u);
    {
        uint32_t riff_size = (uint32_t)(total - 8u);
        riff[4] = (uint8_t)riff_size;
        riff[5] = (uint8_t)(riff_size >> 8);
        riff[6] = (uint8_t)(riff_size >> 16);
        riff[7] = (uint8_t)(riff_size >> 24);
    }
    memcpy(riff + 8u, "WEBP", 4u);
    memcpy(riff + 12u, "VP8L", 4u);
    {
        uint32_t chunk_size = (uint32_t)payload_size;
        riff[16] = (uint8_t)chunk_size;
        riff[17] = (uint8_t)(chunk_size >> 8);
        riff[18] = (uint8_t)(chunk_size >> 16);
        riff[19] = (uint8_t)(chunk_size >> 24);
    }
    memcpy(riff + 20u, writer.data, payload_size);
    free(writer.data);
    *out_data = riff;
    *out_size = total;
    return (int)QW_IMAGE_OK;

out_of_memory:
    status = writer.failed ? (int)QW_IMAGE_ERR_RESOURCE_LIMIT
                           : (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    free(writer.data);
    return status;
}
