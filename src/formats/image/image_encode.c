/* Image encoders: PNG (stored zlib), BMP and baseline TIFF.
 *
 * This is the write side of the image package. It accepts the package's public
 * Gray8/RGB24 QwImage and emits self-contained byte streams:
 *
 * - PNG: color types 0/2, filter 0 rows, a zlib-stored IDAT (no compression
 *   dependency beyond the internal stored-deflate helper).
 * - BMP: 24-bit BGR, bottom-up rows, 54-byte header.
 * - TIFF: baseline little-endian, uncompressed, one strip.
 *
 * JPEG and WebP output are not implemented yet; they report UNSUPPORTED so a
 * caller never receives a mislabeled stream.
 */

#include "quirewise/image.h"

#include <quirewise/compression.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define QW_IMAGE_ENCODE_MAX_PIXELS ((size_t)64u * 1024u * 1024u)

/* Baseline JPEG encoder (jpeg_encode.c). */
int qw_image_jpeg_encode(const QwImage *image, uint8_t **out_data,
                         size_t *out_size);
int qw_image_webp_encode(const QwImage *image, uint8_t **out_data,
                         size_t *out_size);
int qw_image_tga_encode(const QwImage *image, uint8_t **out_data,
                        size_t *out_size);
int qw_image_qoi_encode(const QwImage *image, uint8_t **out_data,
                        size_t *out_size);
int qw_image_ppm_encode(const QwImage *image, uint8_t **out_data,
                        size_t *out_size);
int qw_image_gif_encode(const QwImage *image, uint8_t **out_data,
                        size_t *out_size);
int qw_image_hdr_encode(const QwImage *image, uint8_t **out_data,
                        size_t *out_size);
int qw_image_sgi_encode(const QwImage *image, uint8_t **out_data,
                        size_t *out_size);
int qw_image_pcx_encode(const QwImage *image, uint8_t **out_data,
                        size_t *out_size);

static void encode_write_u16le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void encode_write_u32le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void encode_write_u32be(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static int image_channels(const QwImage *image, size_t *out_channels,
                          size_t *out_row_bytes)
{
    size_t channels;

    if (!image || !image->pixels || !image->width || !image->height)
        return 0;
    if (image->format == QW_IMAGE_PIXEL_GRAY8) channels = 1u;
    else if (image->format == QW_IMAGE_PIXEL_RGB24) channels = 3u;
    else return 0;
    if ((size_t)image->width > SIZE_MAX / channels) return 0;
    if (image->stride < (size_t)image->width * channels) return 0;
    if ((size_t)image->height >
        QW_IMAGE_ENCODE_MAX_PIXELS / ((size_t)image->width * channels))
        return 0;
    *out_channels = channels;
    *out_row_bytes = (size_t)image->width * channels;
    return 1;
}

static int image_buffer_free_ok(QwImageStatus status, uint8_t **out_data,
                                size_t *out_size)
{
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0u;
    return (int)status;
}

/* ------------------------------------------------------------------------- */
/* PNG                                                                       */
/* ------------------------------------------------------------------------- */

static void png_chunk(uint8_t *out, size_t *position, const char type[4],
                      const uint8_t *data, size_t size)
{
    size_t type_position;

    encode_write_u32be(out + *position, (uint32_t)size);
    *position += 4u;
    type_position = *position;
    memcpy(out + *position, type, 4u);
    *position += 4u;
    if (size) memcpy(out + *position, data, size);
    *position += size;
    encode_write_u32be(out + *position,
                       qw_compression_crc32(out + type_position, 4u + size));
    *position += 4u;
}

static int image_encode_png(const QwImage *image, uint8_t **out_data,
                            size_t *out_size)
{
    static const uint8_t signature[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au,
                                         0x1Au, 0x0Au};
    size_t channels;
    size_t row_bytes;
    size_t raw_size;
    size_t zlib_capacity;
    size_t zlib_size = 0u;
    size_t position = 0u;
    size_t row;
    uint8_t *raw;
    uint8_t *zlib_data;
    uint8_t *png;
    uint8_t header[13];

    if (!image_channels(image, &channels, &row_bytes))
        return (int)QW_IMAGE_ERR_INVALID_ARGUMENT;
    if (row_bytes + 1u > SIZE_MAX / image->height)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    raw_size = (row_bytes + 1u) * image->height;
    raw = (uint8_t *)malloc(raw_size);
    if (!raw) return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    for (row = 0u; row < image->height; ++row) {
        uint8_t *target = raw + row * (row_bytes + 1u);
        *target++ = 0u; /* filter: None */
        memcpy(target, image->pixels + row * image->stride, row_bytes);
    }
    zlib_capacity = qw_compression_zlib_bound(raw_size);
    if (zlib_capacity == SIZE_MAX) {
        free(raw);
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    }
    zlib_data = (uint8_t *)malloc(zlib_capacity);
    if (!zlib_data) {
        free(raw);
        return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    }
    if (!qw_compression_deflate_zlib(raw, raw_size, zlib_data,
                                     zlib_capacity, &zlib_size)) {
        free(raw);
        free(zlib_data);
        return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    }
    free(raw);
    if (zlib_size > SIZE_MAX - 8u - 25u - 12u - 12u) {
        free(zlib_data);
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    }
    png = (uint8_t *)malloc(8u + 25u + zlib_size + 12u + 12u);
    if (!png) {
        free(zlib_data);
        return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    }
    memcpy(png, signature, sizeof(signature));
    position = sizeof(signature);
    header[0] = (uint8_t)(image->width >> 24);
    header[1] = (uint8_t)(image->width >> 16);
    header[2] = (uint8_t)(image->width >> 8);
    header[3] = (uint8_t)image->width;
    header[4] = (uint8_t)(image->height >> 24);
    header[5] = (uint8_t)(image->height >> 16);
    header[6] = (uint8_t)(image->height >> 8);
    header[7] = (uint8_t)image->height;
    header[8] = 8u; /* bit depth */
    header[9] = image->format == QW_IMAGE_PIXEL_GRAY8 ? 0u : 2u;
    header[10] = 0u; /* deflate */
    header[11] = 0u; /* adaptive filtering */
    header[12] = 0u; /* no interlace */
    png_chunk(png, &position, "IHDR", header, sizeof(header));
    png_chunk(png, &position, "IDAT", zlib_data, zlib_size);
    png_chunk(png, &position, "IEND", NULL, 0u);
    free(zlib_data);
    *out_data = png;
    *out_size = position;
    return (int)QW_IMAGE_OK;
}

/* ------------------------------------------------------------------------- */
/* BMP                                                                       */
/* ------------------------------------------------------------------------- */

static int image_encode_bmp(const QwImage *image, uint8_t **out_data,
                            size_t *out_size)
{
    size_t channels;
    size_t row_bytes;
    size_t output_row;
    size_t padded_row;
    size_t pixel_bytes;
    size_t total;
    size_t row;
    size_t column;
    uint8_t *bmp;
    uint8_t *pixels;

    if (!image_channels(image, &channels, &row_bytes))
        return (int)QW_IMAGE_ERR_INVALID_ARGUMENT;
    output_row = (size_t)image->width * 3u;
    padded_row = output_row + (4u - (output_row % 4u)) % 4u;
    if (padded_row > SIZE_MAX / image->height)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    pixel_bytes = padded_row * image->height;
    total = 54u + pixel_bytes;
    bmp = (uint8_t *)malloc(total);
    if (!bmp) return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    memset(bmp, 0, 54u);
    bmp[0] = 'B';
    bmp[1] = 'M';
    encode_write_u32le(bmp + 2u, (uint32_t)total);
    encode_write_u32le(bmp + 10u, 54u);
    encode_write_u32le(bmp + 14u, 40u);
    encode_write_u32le(bmp + 18u, image->width);
    encode_write_u32le(bmp + 22u, image->height);
    encode_write_u16le(bmp + 26u, 1u);
    encode_write_u16le(bmp + 28u, 24u);
    encode_write_u32le(bmp + 34u, (uint32_t)pixel_bytes);
    encode_write_u32le(bmp + 38u, 2835u);
    encode_write_u32le(bmp + 42u, 2835u);
    pixels = bmp + 54u;
    memset(pixels, 0, pixel_bytes);
    for (row = 0u; row < image->height; ++row) {
        const uint8_t *source =
            image->pixels + (image->height - 1u - row) * image->stride;
        uint8_t *target = pixels + row * padded_row;

        for (column = 0u; column < image->width; ++column) {
            if (channels == 3u) {
                target[0] = source[2];
                target[1] = source[1];
                target[2] = source[0];
                source += 3u;
            } else {
                target[0] = source[0];
                target[1] = source[0];
                target[2] = source[0];
                source += 1u;
            }
            target += 3u;
        }
    }
    *out_data = bmp;
    *out_size = total;
    return (int)QW_IMAGE_OK;
}

/* ------------------------------------------------------------------------- */
/* TIFF                                                                      */
/* ------------------------------------------------------------------------- */

#define QW_TIFF_ENTRY_COUNT 9u
#define QW_TIFF_IFD_BYTES (2u + QW_TIFF_ENTRY_COUNT * 12u + 4u)

static void tiff_entry(uint8_t *out, size_t index, uint16_t tag,
                       uint16_t type, uint32_t count, uint32_t value)
{
    uint8_t *entry = out + index * 12u;

    encode_write_u16le(entry + 0u, tag);
    encode_write_u16le(entry + 2u, type);
    encode_write_u32le(entry + 4u, count);
    encode_write_u32le(entry + 8u, value);
}

static int image_encode_tiff(const QwImage *image, uint8_t **out_data,
                             size_t *out_size)
{
    size_t channels;
    size_t row_bytes;
    size_t pixel_bytes;
    size_t bits_offset;
    size_t ifd_offset;
    size_t extra_offset;
    size_t total;
    uint8_t *tiff;
    size_t row;

    if (!image_channels(image, &channels, &row_bytes))
        return (int)QW_IMAGE_ERR_INVALID_ARGUMENT;
    if (row_bytes > SIZE_MAX / image->height)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    pixel_bytes = row_bytes * image->height;
    bits_offset = 8u + pixel_bytes + (pixel_bytes % 2u);
    ifd_offset = bits_offset + (channels == 3u ? 6u : 0u);
    if (ifd_offset % 2u) ++ifd_offset;
    extra_offset = ifd_offset + QW_TIFF_IFD_BYTES;
    if (extra_offset > SIZE_MAX - 8u)
        return (int)QW_IMAGE_ERR_RESOURCE_LIMIT;
    total = extra_offset;
    tiff = (uint8_t *)malloc(total);
    if (!tiff) return (int)QW_IMAGE_ERR_OUT_OF_MEMORY;
    memset(tiff, 0, total);
    tiff[0] = 'I';
    tiff[1] = 'I';
    encode_write_u16le(tiff + 2u, 42u);
    encode_write_u32le(tiff + 4u, (uint32_t)ifd_offset);
    for (row = 0u; row < image->height; ++row) {
        memcpy(tiff + 8u + row * row_bytes,
               image->pixels + row * image->stride, row_bytes);
    }
    if (channels == 3u) {
        encode_write_u16le(tiff + 8u + pixel_bytes, 8u);
        encode_write_u16le(tiff + 8u + pixel_bytes + 2u, 8u);
        encode_write_u16le(tiff + 8u + pixel_bytes + 4u, 8u);
    }
    {
        uint8_t *ifd = tiff + ifd_offset;
        size_t entry = 0u;

        encode_write_u16le(ifd, QW_TIFF_ENTRY_COUNT);
        tiff_entry(ifd + 2u, entry++, 256u, 4u, 1u, image->width);
        tiff_entry(ifd + 2u, entry++, 257u, 4u, 1u, image->height);
        if (channels == 3u) {
            tiff_entry(ifd + 2u, entry++, 258u, 3u, 3u,
                       (uint32_t)(8u + pixel_bytes));
        } else {
            tiff_entry(ifd + 2u, entry++, 258u, 3u, 1u, 8u);
        }
        tiff_entry(ifd + 2u, entry++, 259u, 3u, 1u, 1u); /* no compression */
        tiff_entry(ifd + 2u, entry++, 262u, 3u, 1u,
                   channels == 3u ? 2u : 1u); /* RGB or black-is-zero */
        tiff_entry(ifd + 2u, entry++, 273u, 4u, 1u, 8u);
        tiff_entry(ifd + 2u, entry++, 277u, 3u, 1u, (uint32_t)channels);
        tiff_entry(ifd + 2u, entry++, 278u, 4u, 1u, image->height);
        tiff_entry(ifd + 2u, entry++, 279u, 4u, 1u, (uint32_t)pixel_bytes);
        encode_write_u32le(ifd + 2u + entry * 12u, 0u); /* next IFD */
    }
    *out_data = tiff;
    *out_size = total;
    return (int)QW_IMAGE_OK;
}

int qw_image_encode(const QwImage *image, QwImageEncodeFormat format,
                    uint8_t **out_data, size_t *out_size)
{
    if (!out_data || !out_size)
        return image_buffer_free_ok(QW_IMAGE_ERR_INVALID_ARGUMENT,
                                    out_data, out_size);
    *out_data = NULL;
    *out_size = 0u;
    if (!image)
        return image_buffer_free_ok(QW_IMAGE_ERR_INVALID_ARGUMENT, out_data,
                                    out_size);
    switch (format) {
    case QW_IMAGE_ENCODE_PNG:
        return image_encode_png(image, out_data, out_size);
    case QW_IMAGE_ENCODE_BMP:
        return image_encode_bmp(image, out_data, out_size);
    case QW_IMAGE_ENCODE_TIFF:
        return image_encode_tiff(image, out_data, out_size);
    case QW_IMAGE_ENCODE_JPEG:
        return qw_image_jpeg_encode(image, out_data, out_size);
    case QW_IMAGE_ENCODE_WEBP:
        return qw_image_webp_encode(image, out_data, out_size);
    case QW_IMAGE_ENCODE_TGA:
        return qw_image_tga_encode(image, out_data, out_size);
    case QW_IMAGE_ENCODE_QOI:
        return qw_image_qoi_encode(image, out_data, out_size);
    case QW_IMAGE_ENCODE_PPM:
        return qw_image_ppm_encode(image, out_data, out_size);
    case QW_IMAGE_ENCODE_GIF:
        return qw_image_gif_encode(image, out_data, out_size);
    case QW_IMAGE_ENCODE_HDR:
        return qw_image_hdr_encode(image, out_data, out_size);
    case QW_IMAGE_ENCODE_SGI:
        return qw_image_sgi_encode(image, out_data, out_size);
    case QW_IMAGE_ENCODE_PCX:
        return qw_image_pcx_encode(image, out_data, out_size);
    default:
        return image_buffer_free_ok(QW_IMAGE_ERR_INVALID_ARGUMENT, out_data,
                                    out_size);
    }
}

void qw_image_buffer_free(void *buffer)
{
    free(buffer);
}
