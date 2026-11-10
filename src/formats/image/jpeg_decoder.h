/* First-party JPEG (JFIF) decoder: baseline sequential and progressive,
 * Huffman entropy coding, integer/float IDCT, chroma upsampling and YCbCr to
 * RGB conversion. Decode side only. */

#ifndef QUIREWISE_IMAGE_JPEG_DECODER_H
#define QUIREWISE_IMAGE_JPEG_DECODER_H

#include <stddef.h>
#include <stdint.h>

typedef struct QwJpegImage {
    uint8_t *pixels; /* malloc()ed, 1 or 3 channels */
    size_t size;
    int width;
    int height;
    int channels;
} QwJpegImage;

int qw_jpeg_decode(const uint8_t *data, size_t size, QwJpegImage *out_image);
void qw_jpeg_image_free(QwJpegImage *image);

#endif
