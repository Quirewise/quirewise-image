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
                                      QW_IMAGE_ENCODE_TIFF };
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

    {
        uint8_t *encoded = (uint8_t *)1;
        size_t encoded_size = 7u;

        if (qw_image_encode(&rgb, QW_IMAGE_ENCODE_JPEG, &encoded,
                            &encoded_size) != QW_IMAGE_ERR_UNSUPPORTED)
            fail("JPEG encoder was accepted");
        if (encoded || encoded_size) fail("failed encode produced output");
        encoded = (uint8_t *)1;
        encoded_size = 7u;
        if (qw_image_encode(&rgb, QW_IMAGE_ENCODE_WEBP, &encoded,
                            &encoded_size) != QW_IMAGE_ERR_UNSUPPORTED)
            fail("WebP encoder was accepted");
    }

    if (qw_image_encode(NULL, QW_IMAGE_ENCODE_PNG, NULL, NULL) !=
        QW_IMAGE_ERR_INVALID_ARGUMENT)
        fail("NULL arguments were accepted");
    qw_image_buffer_free(NULL);
    puts("Quirewise image encoder tests passed");
    return 0;
}
