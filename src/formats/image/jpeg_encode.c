/* Baseline JPEG encoder (JFIF, 8-bit, 4:4:4, standard Huffman tables).
 *
 * Gray8 input becomes a one-component JPEG; RGB24 input is converted to YCbCr
 * and stored as three components without chroma subsampling. A single quality
 * (85) is used so the public image API stays size-free. The encoder writes a
 * complete JFIF stream: SOI, APP0, DQT, SOF0, DHT, SOS, entropy data, EOI.
 *
 * The DCT is a straightforward floating-point 2-D transform; coefficients are
 * rounded after quantization. This favors clarity over speed and is bounded by
 * the package pixel limit.
 */

#include "quirewise/image.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define JPEG_MAX_DIMENSION ((size_t)65535u)
#define JPEG_OUTPUT_LIMIT ((size_t)128u * 1024u * 1024u)
#define JPEG_QUALITY 85

/* Zigzag scan order (row-major source index). */
static const uint8_t jpeg_zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static const uint8_t jpeg_luminance_quant[64] = {
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99
};

static const uint8_t jpeg_chrominance_quant[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99
};

typedef struct JpegHuffman {
    uint16_t code[256];
    uint8_t size[256];
    int valid;
} JpegHuffman;

typedef struct JpegBuffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    int failed;
} JpegBuffer;

typedef struct JpegBitWriter {
    JpegBuffer *buffer;
    uint64_t accumulator;
    int bits;
} JpegBitWriter;

static const uint8_t jpeg_dc_luminance_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1,
                                                   1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t jpeg_dc_luminance_values[12] = {0, 1, 2, 3, 4, 5,
                                                     6, 7, 8, 9, 10, 11};
static const uint8_t jpeg_dc_chrominance_bits[16] = {0, 3, 1, 1, 1, 1, 1, 1,
                                                     1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t jpeg_dc_chrominance_values[12] = {0, 1, 2, 3, 4, 5,
                                                       6, 7, 8, 9, 10, 11};
static const uint8_t jpeg_ac_luminance_bits[16] = {
    0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d
};
static const uint8_t jpeg_ac_luminance_values[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa
};
static const uint8_t jpeg_ac_chrominance_bits[16] = {
    0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77
};
static const uint8_t jpeg_ac_chrominance_values[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
    0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa
};

static int jpeg_reserve(JpegBuffer *buffer, size_t additional)
{
    size_t needed;
    size_t capacity;
    uint8_t *grown;

    if (buffer->failed) return 0;
    if (buffer->size > JPEG_OUTPUT_LIMIT ||
        additional > JPEG_OUTPUT_LIMIT - buffer->size) {
        buffer->failed = 1;
        return 0;
    }
    needed = buffer->size + additional;
    if (needed <= buffer->capacity) return 1;
    capacity = buffer->capacity ? buffer->capacity : 8192u;
    while (capacity < needed) {
        if (capacity > JPEG_OUTPUT_LIMIT / 2u) {
            capacity = JPEG_OUTPUT_LIMIT;
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

static int jpeg_byte(JpegBuffer *buffer, uint8_t value)
{
    if (!jpeg_reserve(buffer, 1u)) return 0;
    buffer->data[buffer->size++] = value;
    return 1;
}

static int jpeg_bytes(JpegBuffer *buffer, const uint8_t *data, size_t size)
{
    if (!size) return 1;
    if (!jpeg_reserve(buffer, size)) return 0;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 1;
}

static int jpeg_marker(JpegBuffer *buffer, uint8_t marker)
{
    return jpeg_byte(buffer, 0xffu) && jpeg_byte(buffer, marker);
}

static int jpeg_word_be(JpegBuffer *buffer, uint32_t value)
{
    return jpeg_byte(buffer, (uint8_t)(value >> 8)) &&
           jpeg_byte(buffer, (uint8_t)value);
}

/* Build canonical Huffman codes from a JPEG DHT table definition. */
static void jpeg_build_huffman(JpegHuffman *table, const uint8_t bits[16],
                               const uint8_t *values, int value_count)
{
    uint16_t code = 0u;
    int index = 0;
    int length;

    memset(table, 0, sizeof(*table));
    for (length = 1; length <= 16; ++length) {
        int count = bits[length - 1];
        int item;

        for (item = 0; item < count; ++item) {
            uint8_t symbol = values[index++];
            table->code[symbol] = code;
            table->size[symbol] = (uint8_t)length;
            ++code;
        }
        code = (uint16_t)(code << 1);
    }
    table->valid = index == value_count;
}

static void jpeg_bit_put(JpegBitWriter *writer, uint32_t value, int size)
{
    if (!size) return;
    writer->accumulator = (writer->accumulator << size) |
                          (uint64_t)(value & ((1u << size) - 1u));
    writer->bits += size;
    while (writer->bits >= 8) {
        uint8_t byte =
            (uint8_t)((writer->accumulator >> (writer->bits - 8)) & 0xffu);
        writer->bits -= 8;
        if (!jpeg_byte(writer->buffer, byte)) return;
        if (byte == 0xffu && !jpeg_byte(writer->buffer, 0x00u)) return;
    }
}

static void jpeg_bit_flush(JpegBitWriter *writer)
{
    if (writer->bits > 0) {
        uint8_t byte = (uint8_t)(((writer->accumulator
                                  << (8 - writer->bits)) |
                                 ((1u << (8 - writer->bits)) - 1u)) &
                                0xffu);
        if (!jpeg_byte(writer->buffer, byte)) return;
        if (byte == 0xffu) (void)jpeg_byte(writer->buffer, 0x00u);
        writer->bits = 0;
        writer->accumulator = 0u;
    }
}

static void jpeg_huffman_put(JpegBitWriter *writer, const JpegHuffman *table,
                             uint8_t symbol)
{
    jpeg_bit_put(writer, table->code[symbol], table->size[symbol]);
}

static int jpeg_category(int value)
{
    int category = 0;

    if (value < 0) value = -value;
    while (value) {
        ++category;
        value >>= 1;
    }
    return category;
}

static void jpeg_amplitude_put(JpegBitWriter *writer, int value, int category)
{
    if (category) {
        uint32_t bits = (uint32_t)value;

        if (value < 0) bits = (uint32_t)(value + (1 << category) - 1);
        jpeg_bit_put(writer, bits, category);
    }
}

static uint8_t jpeg_quant_value(uint8_t base, int quality)
{
    int scaled = ((int)base * quality + 50) / 100;

    if (scaled < 1) scaled = 1;
    if (scaled > 255) scaled = 255;
    return (uint8_t)scaled;
}

static void jpeg_dct_block(const double input[64], double output[64])
{
    static int initialized;
    static double cosine[8][8];
    double temporary[64];
    int row;
    int column;
    int frequency;

    if (!initialized) {
        int u;
        for (u = 0; u < 8; ++u) {
            int x;
            for (x = 0; x < 8; ++x) {
                cosine[u][x] = cos(((2.0 * x + 1.0) * u * 3.141592653589793) /
                                   16.0);
            }
        }
        initialized = 1;
    }
    /* Rows: temporary[y][u]. */
    for (row = 0; row < 8; ++row) {
        for (frequency = 0; frequency < 8; ++frequency) {
            double sum = 0.0;
            for (column = 0; column < 8; ++column) {
                sum += input[row * 8 + column] * cosine[frequency][column];
            }
            temporary[row * 8 + frequency] =
                sum * (frequency == 0 ? 0.3535533905932738 : 0.5);
        }
    }
    /* Columns: output[v][u]. */
    for (frequency = 0; frequency < 8; ++frequency) {
        for (column = 0; column < 8; ++column) {
            double sum = 0.0;
            for (row = 0; row < 8; ++row) {
                sum += temporary[row * 8 + frequency] * cosine[column][row];
            }
            output[column * 8 + frequency] =
                sum * (column == 0 ? 0.3535533905932738 : 0.5);
        }
    }
}

static void jpeg_sample_block(const uint8_t *plane, size_t width,
                              size_t height, size_t block_x, size_t block_y,
                              double output[64])
{
    int y;
    int x;

    for (y = 0; y < 8; ++y) {
        size_t source_y = block_y * 8u + (size_t)y;
        if (source_y >= height) source_y = height - 1u;
        for (x = 0; x < 8; ++x) {
            size_t source_x = block_x * 8u + (size_t)x;
            if (source_x >= width) source_x = width - 1u;
            output[y * 8 + x] = (double)plane[source_y * width + source_x] -
                                128.0;
        }
    }
}

static int jpeg_encode_plane(JpegBitWriter *writer, const uint8_t *plane,
                             size_t width, size_t height,
                             const uint8_t quant[64],
                             const JpegHuffman *dc_table,
                             const JpegHuffman *ac_table,
                             int *previous_dc)
{
    size_t blocks_x = (width + 7u) / 8u;
    size_t blocks_y = (height + 7u) / 8u;
    size_t block_y;

    for (block_y = 0u; block_y < blocks_y; ++block_y) {
        size_t block_x;

        for (block_x = 0u; block_x < blocks_x; ++block_x) {
            double samples[64];
            double coefficients[64];
            int quantized[64];
            int index;

            jpeg_sample_block(plane, width, height, block_x, block_y, samples);
            jpeg_dct_block(samples, coefficients);
            for (index = 0; index < 64; ++index)
                quantized[index] =
                    (int)(coefficients[index] /
                              (double)quant[index] +
                          0.5);
            /* Zigzag reorder before entropy coding. */
            {
                int zigzag[64];
                int dc;
                int diff;
                int category;

                for (index = 0; index < 64; ++index)
                    zigzag[index] = quantized[jpeg_zigzag[index]];
                dc = zigzag[0];
                diff = dc - *previous_dc;
                *previous_dc = dc;
                category = jpeg_category(diff);
                jpeg_huffman_put(writer, dc_table, (uint8_t)category);
                jpeg_amplitude_put(writer, diff, category);
                {
                    int run = 0;
                    for (index = 1; index < 64; ++index) {
                        int value = zigzag[index];

                        if (!value) {
                            ++run;
                            continue;
                        }
                        while (run > 15) {
                            jpeg_huffman_put(writer, ac_table, 0xf0u);
                            run -= 16;
                        }
                        category = jpeg_category(value);
                        jpeg_huffman_put(writer, ac_table,
                                         (uint8_t)((run << 4) | category));
                        jpeg_amplitude_put(writer, value, category);
                        run = 0;
                    }
                    if (run) jpeg_huffman_put(writer, ac_table, 0x00u);
                }
            }
            if (writer->buffer->failed) return 0;
        }
    }
    return 1;
}

static int jpeg_write_dht(JpegBuffer *buffer, uint8_t table_class_id,
                          const uint8_t bits[16], const uint8_t *values,
                          int value_count)
{
    int index;

    if (!jpeg_byte(buffer, table_class_id)) return 0;
    for (index = 0; index < 16; ++index) {
        if (!jpeg_byte(buffer, bits[index])) return 0;
    }
    return jpeg_bytes(buffer, values, (size_t)value_count);
}

int qw_image_jpeg_encode(const QwImage *image, uint8_t **out_data,
                         size_t *out_size)
{
    JpegBuffer buffer;
    size_t width;
    size_t height;
    int components;
    uint8_t *plane_y = NULL;
    uint8_t *plane_cb = NULL;
    uint8_t *plane_cr = NULL;
    uint8_t quant_luminance[64];
    uint8_t quant_chrominance[64];
    int quality;
    int scale;
    int index;
    int previous_dc[3] = {0, 0, 0};
    JpegHuffman dc_luminance;
    JpegHuffman ac_luminance;
    JpegHuffman dc_chrominance;
    JpegHuffman ac_chrominance;
    JpegBitWriter writer;
    int status = (int)QW_IMAGE_ERR_OUT_OF_MEMORY;

    if (!image || !image->pixels || !out_data || !out_size ||
        !image->width || !image->height ||
        (image->format != QW_IMAGE_PIXEL_GRAY8 &&
         image->format != QW_IMAGE_PIXEL_RGB24))
        return (int)QW_IMAGE_ERR_INVALID_ARGUMENT;
    width = image->width;
    height = image->height;
    if (width > JPEG_MAX_DIMENSION || height > JPEG_MAX_DIMENSION)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    components = image->format == QW_IMAGE_PIXEL_RGB24 ? 3 : 1;
    memset(&buffer, 0, sizeof(buffer));

    plane_y = (uint8_t *)malloc(width * height);
    if (!plane_y) goto out_of_memory;
    if (components == 3) {
        plane_cb = (uint8_t *)malloc(width * height);
        plane_cr = (uint8_t *)malloc(width * height);
        if (!plane_cb || !plane_cr) goto out_of_memory;
    }
    for (index = 0; index < (int)(width * height); ++index) {
        if (components == 1) {
            plane_y[index] = image->pixels[index];
        } else {
            const uint8_t *pixel = image->pixels + (size_t)index * 3u;
            double red = pixel[0];
            double green = pixel[1];
            double blue = pixel[2];
            double y = 0.299 * red + 0.587 * green + 0.114 * blue;
            double cb = -0.168736 * red - 0.331264 * green + 0.5 * blue + 128.0;
            double cr = 0.5 * red - 0.418688 * green - 0.081312 * blue + 128.0;

            if (y < 0.0) y = 0.0;
            if (y > 255.0) y = 255.0;
            if (cb < 0.0) cb = 0.0;
            if (cb > 255.0) cb = 255.0;
            if (cr < 0.0) cr = 0.0;
            if (cr > 255.0) cr = 255.0;
            plane_y[index] = (uint8_t)(y + 0.5);
            plane_cb[index] = (uint8_t)(cb + 0.5);
            plane_cr[index] = (uint8_t)(cr + 0.5);
        }
    }
    quality = 200 - 2 * JPEG_QUALITY;
    for (index = 0; index < 64; ++index) {
        quant_luminance[index] =
            jpeg_quant_value(jpeg_luminance_quant[index], quality);
        quant_chrominance[index] =
            jpeg_quant_value(jpeg_chrominance_quant[index], quality);
    }
    jpeg_build_huffman(&dc_luminance, jpeg_dc_luminance_bits,
                       jpeg_dc_luminance_values, 12);
    jpeg_build_huffman(&dc_chrominance, jpeg_dc_chrominance_bits,
                       jpeg_dc_chrominance_values, 12);
    jpeg_build_huffman(&ac_luminance, jpeg_ac_luminance_bits,
                       jpeg_ac_luminance_values, 162);
    jpeg_build_huffman(&ac_chrominance, jpeg_ac_chrominance_bits,
                       jpeg_ac_chrominance_values, 162);
    if (!dc_luminance.valid || !dc_chrominance.valid ||
        !ac_luminance.valid || !ac_chrominance.valid)
        goto out_of_memory;

    if (!jpeg_marker(&buffer, 0xd8u)) goto out_of_memory; /* SOI */
    /* APP0/JFIF. */
    if (!jpeg_marker(&buffer, 0xe0u) || !jpeg_word_be(&buffer, 16u) ||
        !jpeg_bytes(&buffer, (const uint8_t *)"JFIF\0", 5u) ||
        !jpeg_byte(&buffer, 1u) || !jpeg_byte(&buffer, 1u) ||
        !jpeg_byte(&buffer, 0u) || !jpeg_word_be(&buffer, 1u) ||
        !jpeg_word_be(&buffer, 1u) || !jpeg_byte(&buffer, 0u) ||
        !jpeg_byte(&buffer, 0u))
        goto out_of_memory;
    /* DQT: luminance then chrominance (only luminance for gray). */
    {
        size_t tables = components == 3 ? 2u : 1u;
        if (!jpeg_marker(&buffer, 0xdbu) ||
            !jpeg_word_be(&buffer, (uint32_t)(2u + 65u * tables)) ||
            !jpeg_byte(&buffer, 0x00u))
            goto out_of_memory;
        for (index = 0; index < 64; ++index) {
            if (!jpeg_byte(&buffer, quant_luminance[jpeg_zigzag[index]]))
                goto out_of_memory;
        }
        if (components == 3) {
            if (!jpeg_byte(&buffer, 0x01u)) goto out_of_memory;
            for (index = 0; index < 64; ++index) {
                if (!jpeg_byte(&buffer, quant_chrominance[jpeg_zigzag[index]]))
                    goto out_of_memory;
            }
        }
    }
    /* SOF0. */
    if (!jpeg_marker(&buffer, 0xc0u) ||
        !jpeg_word_be(&buffer, (uint32_t)(8u + 3u * components)) ||
        !jpeg_byte(&buffer, 8u) || !jpeg_word_be(&buffer, (uint32_t)height) ||
        !jpeg_word_be(&buffer, (uint32_t)width) ||
        !jpeg_byte(&buffer, (uint8_t)components))
        goto out_of_memory;
    if (components == 1) {
        if (!jpeg_byte(&buffer, 1u) || !jpeg_byte(&buffer, 0x11u) ||
            !jpeg_byte(&buffer, 0x00u))
            goto out_of_memory;
    } else {
        int component;
        for (component = 1; component <= 3; ++component) {
            if (!jpeg_byte(&buffer, (uint8_t)component) ||
                !jpeg_byte(&buffer, 0x11u) ||
                !jpeg_byte(&buffer, (uint8_t)(component == 1 ? 0u : 1u)))
                goto out_of_memory;
        }
    }
    /* DHT: four standard tables (two for gray). */
    {
        uint32_t dht_length = 2u;
        dht_length += 1u + 16u + 12u;
        dht_length += 1u + 16u + 162u;
        if (components == 3) {
            dht_length += 1u + 16u + 12u;
            dht_length += 1u + 16u + 162u;
        }
        if (!jpeg_marker(&buffer, 0xc4u) || !jpeg_word_be(&buffer, dht_length) ||
            !jpeg_write_dht(&buffer, 0x00u, jpeg_dc_luminance_bits,
                            jpeg_dc_luminance_values, 12) ||
            !jpeg_write_dht(&buffer, 0x10u, jpeg_ac_luminance_bits,
                            jpeg_ac_luminance_values, 162))
            goto out_of_memory;
        if (components == 3 &&
            (!jpeg_write_dht(&buffer, 0x01u, jpeg_dc_chrominance_bits,
                             jpeg_dc_chrominance_values, 12) ||
             !jpeg_write_dht(&buffer, 0x11u, jpeg_ac_chrominance_bits,
                             jpeg_ac_chrominance_values, 162)))
            goto out_of_memory;
    }
    /* SOS. */
    if (!jpeg_marker(&buffer, 0xdau) ||
        !jpeg_word_be(&buffer, (uint32_t)(6u + 2u * components)) ||
        !jpeg_byte(&buffer, (uint8_t)components))
        goto out_of_memory;
    if (components == 1) {
        if (!jpeg_byte(&buffer, 1u) || !jpeg_byte(&buffer, 0x00u) ||
            !jpeg_byte(&buffer, 0u) || !jpeg_byte(&buffer, 63u) ||
            !jpeg_byte(&buffer, 0u))
            goto out_of_memory;
    } else {
        int component;
        for (component = 1; component <= 3; ++component) {
            if (!jpeg_byte(&buffer, (uint8_t)component) ||
                !jpeg_byte(&buffer, (uint8_t)(component == 1 ? 0x00u : 0x11u)))
                goto out_of_memory;
        }
        if (!jpeg_byte(&buffer, 0u) || !jpeg_byte(&buffer, 63u) ||
            !jpeg_byte(&buffer, 0u))
            goto out_of_memory;
    }
    /* Entropy-coded data. */
    writer.buffer = &buffer;
    writer.accumulator = 0u;
    writer.bits = 0;
    if (components == 1) {
        if (!jpeg_encode_plane(&writer, plane_y, width, height,
                               quant_luminance, &dc_luminance, &ac_luminance,
                               &previous_dc[0]))
            goto out_of_memory;
    } else {
        size_t blocks_x = (width + 7u) / 8u;
        size_t blocks_y = (height + 7u) / 8u;
        size_t block_y;

        /* Interleaved MCU order: Y, Cb, Cr block per 8x8 position. */
        for (block_y = 0u; block_y < blocks_y; ++block_y) {
            size_t block_x;
            for (block_x = 0u; block_x < blocks_x; ++block_x) {
                double samples[64];
                double coefficients[64];
                int quantized[64];
                int zigzag[64];
                int dc;
                int diff;
                int category;
                int component;
                const uint8_t *planes[3] = {plane_y, plane_cb, plane_cr};
                const uint8_t *quants[3] = {quant_luminance,
                                           quant_chrominance,
                                           quant_chrominance};
                const JpegHuffman *dc_tables[3] = {&dc_luminance,
                                                  &dc_chrominance,
                                                  &dc_chrominance};
                const JpegHuffman *ac_tables[3] = {&ac_luminance,
                                                  &ac_chrominance,
                                                  &ac_chrominance};

                for (component = 0; component < 3; ++component) {
                    int item;
                    jpeg_sample_block(planes[component], width, height,
                                      block_x, block_y, samples);
                    jpeg_dct_block(samples, coefficients);
                    for (item = 0; item < 64; ++item)
                        quantized[item] =
                            (int)(coefficients[item] /
                                      (double)quants[component][item] +
                                  0.5);
                    for (item = 0; item < 64; ++item)
                        zigzag[item] = quantized[jpeg_zigzag[item]];
                    dc = zigzag[0];
                    diff = dc - previous_dc[component];
                    previous_dc[component] = dc;
                    category = jpeg_category(diff);
                    jpeg_huffman_put(&writer, dc_tables[component],
                                     (uint8_t)category);
                    jpeg_amplitude_put(&writer, diff, category);
                    {
                        int run = 0;
                        for (item = 1; item < 64; ++item) {
                            int value = zigzag[item];

                            if (!value) {
                                ++run;
                                continue;
                            }
                            while (run > 15) {
                                jpeg_huffman_put(&writer, ac_tables[component],
                                                 0xf0u);
                                run -= 16;
                            }
                            category = jpeg_category(value);
                            jpeg_huffman_put(
                                &writer, ac_tables[component],
                                (uint8_t)((run << 4) | category));
                            jpeg_amplitude_put(&writer, value, category);
                            run = 0;
                        }
                        if (run) jpeg_huffman_put(&writer, ac_tables[component],
                                                  0x00u);
                    }
                    if (buffer.failed) goto out_of_memory;
                }
            }
        }
    }
    jpeg_bit_flush(&writer);
    if (!jpeg_marker(&buffer, 0xd9u)) goto out_of_memory; /* EOI */
    free(plane_y);
    free(plane_cb);
    free(plane_cr);
    *out_data = buffer.data;
    *out_size = buffer.size;
    return (int)QW_IMAGE_OK;

out_of_memory:
    status = buffer.failed ? (int)QW_IMAGE_ERR_RESOURCE_LIMIT
                           : (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    free(buffer.data);
    free(plane_y);
    free(plane_cb);
    free(plane_cr);
    return status;
}
