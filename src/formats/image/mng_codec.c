#include "image_internal.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MNG_HEADER_SIZE 8u
static const uint8_t mng_signature[MNG_HEADER_SIZE] = {
    0x8au, 'M', 'N', 'G', '\r', '\n', 0x1au, '\n'
};

static const uint8_t png_signature[MNG_HEADER_SIZE] = {
    0x89u, 'P', 'N', 'G', '\r', '\n', 0x1au, '\n'
};

typedef struct QwMngInfo {
    size_t png_begin;
    size_t png_end;
    uint32_t width;
    uint32_t height;
} QwMngInfo;


static int mng_png_ancillary_allowed(const uint8_t type[4])
{
    static const uint8_t palette[] = {'P', 'L', 'T', 'E'};
    static const uint8_t animation_chunks[][4] = {
        {'a', 'c', 'T', 'L'}, {'f', 'c', 'T', 'L'}, {'f', 'd', 'A', 'T'}
    };
    if (memcmp(type, palette, sizeof(palette)) == 0) return 1;
    for (size_t i = 0u; i < sizeof(animation_chunks) / sizeof(animation_chunks[0]);
         ++i) {
        if (memcmp(type, animation_chunks[i], sizeof(animation_chunks[i])) == 0) {
            return 0;
        }
    }
    return type[0] >= 'a' && type[0] <= 'z';
}

static int mng_read_info(const void *data, size_t size, QwMngInfo *info)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t position = MNG_HEADER_SIZE;
    int have_mhdr = 0;
    int have_ihdr = 0;
    int have_idat = 0;
    int idat_finished = 0;
    int have_iend = 0;

    if (!bytes || !info || size < MNG_HEADER_SIZE ||
        memcmp(bytes, mng_signature, sizeof(mng_signature)) != 0) {
        return 0;
    }
    memset(info, 0, sizeof(*info));
    while (position < size) {
        uint32_t length;
        const uint8_t *type;
        size_t chunk_size;
        if (size - position < 12u) return 0;
        length = qw_read_u32_be(bytes + position);
        if ((size_t)length > size - position - 12u) return 0;
        type = bytes + position + 4u;
        chunk_size = (size_t)length + 12u;
        if (!have_mhdr) {
            if (memcmp(type, "MHDR", 4u) != 0 || length != 28u) return 0;
            have_mhdr = 1;
        } else if (!have_ihdr) {
            if (memcmp(type, "IHDR", 4u) != 0 || length != 13u) return 0;
            info->png_begin = position;
            info->width = qw_read_u32_be(bytes + position + 8u);
            info->height = qw_read_u32_be(bytes + position + 12u);
            have_ihdr = 1;
        } else if (memcmp(type, "IDAT", 4u) == 0) {
            if (idat_finished) return 0;
            have_idat = 1;
        } else if (memcmp(type, "IEND", 4u) == 0) {
            if (!have_idat || length != 0u) return 0;
            have_iend = 1;
            position += chunk_size;
            if (position == size) {
                info->png_end = position;
                break;
            }
            if (size - position == 12u &&
                qw_read_u32_be(bytes + position) == 0u &&
                memcmp(bytes + position + 4u, "MEND", 4u) == 0) {
                position += 12u;
                if (position != size) return 0;
                info->png_end = position - 12u;
                break;
            }
            return 0;
        } else {
            if (have_idat) {
                idat_finished = 1;
                return 0;
            }
            if (!mng_png_ancillary_allowed(type)) return 0;
        }
        position += chunk_size;
    }
    return have_mhdr && have_ihdr && have_idat && have_iend &&
           position == size && info->png_end > info->png_begin &&
           info->width && info->height;
}

int qw_mng_probe(const void *data, size_t size)
{
    QwMngInfo info;
    return mng_read_info(data, size, &info);
}

int qw_mng_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwMngInfo info;
    QwPageImage decoded = {0};
    uint8_t *png = NULL;
    uint8_t *pixels = NULL;
    size_t png_size;
    size_t pixel_count;
    size_t output_size;
    int rc = QW_STATUS_ERR_FORMAT;

    if (!out_page || !mng_read_info(data, size, &info)) return QW_STATUS_ERR_FORMAT;
    if (!qw_image_dimensions_within_limits(info.width, info.height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    memset(out_page, 0, sizeof(*out_page));
    if (info.png_end - info.png_begin > SIZE_MAX - MNG_HEADER_SIZE) {
        return QW_STATUS_ERR_FORMAT;
    }
    png_size = MNG_HEADER_SIZE + info.png_end - info.png_begin;
    png = (uint8_t *)malloc(png_size);
    if (!png) return QW_STATUS_ERR_OUT_OF_MEMORY;
    memcpy(png, png_signature, MNG_HEADER_SIZE);
    memcpy(png + MNG_HEADER_SIZE, bytes + info.png_begin,
           info.png_end - info.png_begin);
    rc = qw_png_decode_page(png, png_size, &decoded);
    if (rc != QW_STATUS_OK) goto cleanup;
    if (decoded.width != info.width || decoded.height != info.height) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    if (!qw_checked_size_mul((size_t)decoded.width, (size_t)decoded.height,
                             &pixel_count) ||
        !qw_checked_size_mul(pixel_count, 3u, &output_size)) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    if (decoded.kind == QW_PAGE_IMAGE_RGB24) {
        out_page->data = decoded.data;
        out_page->size = decoded.size;
        out_page->kind = QW_PAGE_IMAGE_RGB24;
        out_page->width = decoded.width;
        out_page->height = decoded.height;
        decoded.data = NULL;
        rc = QW_STATUS_OK;
        goto cleanup;
    }
    if (decoded.kind != QW_PAGE_IMAGE_GRAY8 || decoded.size < pixel_count) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    pixels = (uint8_t *)malloc(output_size);
    if (!pixels) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (size_t i = 0u; i < pixel_count; ++i) {
        pixels[i * 3u] = decoded.data[i];
        pixels[i * 3u + 1u] = decoded.data[i];
        pixels[i * 3u + 2u] = decoded.data[i];
    }
    out_page->data = pixels;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = decoded.width;
    out_page->height = decoded.height;
    pixels = NULL;
    rc = QW_STATUS_OK;

cleanup:
    qw_image_release_page(&decoded);
    free(png);
    free(pixels);
    return rc;
}
