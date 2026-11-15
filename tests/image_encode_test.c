/* Image encoder tests: PNG/BMP/TIFF round-trip through the decode API. */

#include <quirewise/image.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static int pixel_matches(const QwImage *decoded, unsigned row, unsigned column,
                         const uint8_t *expected, size_t expected_stride,
                         int expected_gray)
{
    const uint8_t *actual = decoded->pixels + (size_t)row * decoded->stride;

    if (decoded->format == QW_IMAGE_PIXEL_GRAY8) {
        if (!expected_gray) return 0;
        return actual[column] == expected[(size_t)row * expected_stride +
                                          (size_t)column];
    }
    if (expected_gray) {
        uint8_t value =
            expected[(size_t)row * expected_stride + (size_t)column];
        const uint8_t *pixel = actual + (size_t)column * 3u;
        return pixel[0] == value && pixel[1] == value && pixel[2] == value;
    }
    return memcmp(actual + (size_t)column * 3u,
                  expected + (size_t)row * expected_stride +
                      (size_t)column * 3u,
                  3u) == 0;
}

static int decode_and_compare(const uint8_t *bytes, size_t size,
                              uint32_t width, uint32_t height,
                              const uint8_t *expected, size_t expected_stride,
                              int expected_gray)
{
    QwImage decoded;
    unsigned row;

    memset(&decoded, 0, sizeof(decoded));
    if (qw_image_decode(bytes, size, &decoded) != QW_IMAGE_OK ||
        !decoded.pixels) {
        fprintf(stderr, "decode failed for %zu bytes\n", size);
        return 0;
    }
    if (decoded.width != width || decoded.height != height) {
        qw_image_free(&decoded);
        return 0;
    }
    for (row = 0u; row < height; ++row) {
        unsigned column;

        for (column = 0u; column < width; ++column) {
            if (!pixel_matches(&decoded, row, column, expected,
                               expected_stride, expected_gray)) {
                qw_image_free(&decoded);
                return 0;
            }
        }
    }
    qw_image_free(&decoded);
    return 1;
}

int main(void)
{
    static const uint8_t rgb_pixels[12] = {
        255u, 0u, 0u, 0u, 255u, 0u, 0u, 0u, 255u, 255u, 255u, 255u
    };
    static const uint8_t gray_pixels[4] = {0u, 85u, 170u, 255u};
    QwImage rgb;
    QwImage gray;
    QwImageEncodeFormat formats[] = { QW_IMAGE_ENCODE_PNG,
                                      QW_IMAGE_ENCODE_BMP,
                                      QW_IMAGE_ENCODE_TIFF,
                                      QW_IMAGE_ENCODE_WEBP };
    size_t index;

    memset(&rgb, 0, sizeof(rgb));
    rgb.pixels = (uint8_t *)rgb_pixels;
    rgb.size = sizeof(rgb_pixels);
    rgb.stride = 6u;
    rgb.width = 2u;
    rgb.height = 2u;
    rgb.format = QW_IMAGE_PIXEL_RGB24;

    memset(&gray, 0, sizeof(gray));
    gray.pixels = (uint8_t *)gray_pixels;
    gray.size = sizeof(gray_pixels);
    gray.stride = 2u;
    gray.width = 2u;
    gray.height = 2u;
    gray.format = QW_IMAGE_PIXEL_GRAY8;

    for (index = 0u; index < sizeof(formats) / sizeof(formats[0]); ++index) {
        uint8_t *encoded = NULL;
        size_t encoded_size = 0u;

        if (qw_image_encode(&rgb, formats[index], &encoded, &encoded_size) !=
                QW_IMAGE_OK ||
            !encoded || !encoded_size)
            fail("RGB encode failed");
        if (!decode_and_compare(encoded, encoded_size, 2u, 2u, rgb_pixels, 6u,
                                0)) {
            fprintf(stderr, "FAIL: RGB format %d did not round-trip\n",
                    (int)formats[index]);
            exit(1);
        }
        if (formats[index] == QW_IMAGE_ENCODE_PNG &&
            (encoded[0] != 0x89u || encoded[1] != 'P'))
            fail("PNG signature");
        if (formats[index] == QW_IMAGE_ENCODE_BMP &&
            (encoded[0] != 'B' || encoded[1] != 'M'))
            fail("BMP signature");
        if (formats[index] == QW_IMAGE_ENCODE_TIFF &&
            (encoded[0] != 'I' || encoded[1] != 'I' || encoded[2] != 42))
            fail("TIFF signature");
        qw_image_buffer_free(encoded);

        encoded = NULL;
        encoded_size = 0u;
        if (qw_image_encode(&gray, formats[index], &encoded, &encoded_size) !=
                QW_IMAGE_OK ||
            !encoded || !encoded_size)
            fail("gray encode failed");
        if (!decode_and_compare(encoded, encoded_size, 2u, 2u, gray_pixels, 2u,
                                1)) {
            fprintf(stderr,
                    "FAIL: gray format %d did not round-trip\n",
                    (int)formats[index]);
            exit(1);
        }
        qw_image_buffer_free(encoded);
    }

    /* Baseline JPEG is lossy: encode uniform 8x8 blocks so every pixel lands
     * in one DCT block, then check the decoded color within a tolerance. */
    {
        static uint8_t red_pixels[8u * 8u * 3u];
        static uint8_t gray_block[8u * 8u];
        QwImage red;
        QwImage gray_image;
        uint8_t *encoded = NULL;
        size_t encoded_size = 0u;
        QwImage decoded;
        int row;
        int column;

        memset(red_pixels, 0, sizeof(red_pixels));
        memset(gray_block, 200, sizeof(gray_block));
        for (row = 0; row < 8; ++row) {
            for (column = 0; column < 8; ++column) {
                red_pixels[(row * 8 + column) * 3u + 0u] = 255u;
            }
        }
        memset(&red, 0, sizeof(red));
        red.pixels = red_pixels;
        red.size = sizeof(red_pixels);
        red.stride = 24u;
        red.width = 8u;
        red.height = 8u;
        red.format = QW_IMAGE_PIXEL_RGB24;
        memset(&gray_image, 0, sizeof(gray_image));
        gray_image.pixels = gray_block;
        gray_image.size = sizeof(gray_block);
        gray_image.stride = 8u;
        gray_image.width = 8u;
        gray_image.height = 8u;
        gray_image.format = QW_IMAGE_PIXEL_GRAY8;

        if (qw_image_encode(&red, QW_IMAGE_ENCODE_JPEG, &encoded,
                            &encoded_size) != QW_IMAGE_OK ||
            !encoded || encoded_size < 4u || encoded[0] != 0xffu ||
            encoded[1] != 0xd8u)
            fail("JPEG encode failed");
        memset(&decoded, 0, sizeof(decoded));
        if (qw_image_decode(encoded, encoded_size, &decoded) != QW_IMAGE_OK ||
            decoded.width != 8u || decoded.height != 8u || !decoded.pixels) {
            if (decoded.pixels) qw_image_free(&decoded);
            qw_image_buffer_free(encoded);
            fail("JPEG output did not decode");
        }
        for (row = 0; row < 8; ++row) {
            for (column = 0; column < 8; ++column) {
                const uint8_t *pixel = decoded.pixels +
                                       (size_t)row * decoded.stride +
                                       (size_t)column * 3u;
                if (pixel[0] < 240u || pixel[1] > 20u || pixel[2] > 20u) {
                    fprintf(stderr,
                            "JPEG red block drifted: (%u,%u,%u) at %d,%d\n",
                            pixel[0], pixel[1], pixel[2], row, column);
                    qw_image_free(&decoded);
                    qw_image_buffer_free(encoded);
                    fail("JPEG color drifted");
                }
            }
        }
        qw_image_free(&decoded);
        qw_image_buffer_free(encoded);

        encoded = NULL;
        encoded_size = 0u;
        if (qw_image_encode(&gray_image, QW_IMAGE_ENCODE_JPEG, &encoded,
                            &encoded_size) != QW_IMAGE_OK)
            fail("gray JPEG encode failed");
        memset(&decoded, 0, sizeof(decoded));
        if (qw_image_decode(encoded, encoded_size, &decoded) != QW_IMAGE_OK ||
            !decoded.pixels) {
            if (decoded.pixels) qw_image_free(&decoded);
            qw_image_buffer_free(encoded);
            fail("gray JPEG did not decode");
        }
        for (row = 0; row < 8; ++row) {
            for (column = 0; column < 8; ++column) {
                unsigned value =
                    decoded.pixels[(size_t)row * decoded.stride +
                                   (size_t)column];
                if (value < 185u || value > 215u) {
                    qw_image_free(&decoded);
                    qw_image_buffer_free(encoded);
                    fail("gray JPEG value drifted");
                }
            }
        }
        qw_image_free(&decoded);
        qw_image_buffer_free(encoded);
    }

    if (qw_image_encode(NULL, QW_IMAGE_ENCODE_PNG, NULL, NULL) !=
        QW_IMAGE_ERR_INVALID_ARGUMENT)
        fail("NULL arguments were accepted");
    qw_image_buffer_free(NULL);
    puts("Quirewise image encoder tests passed");
    return 0;
}
