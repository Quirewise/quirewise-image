#include "quirewise/image.h"

#include "image_internal.h"
#include "tiff_decode.h"

#include <stdlib.h>
#include <string.h>

uint32_t qw_image_abi_version(void)
{
    return QUIREWISE_IMAGE_ABI_VERSION;
}

int qw_image_probe(const void *data, size_t size)
{
    uint32_t webp_width;
    uint32_t webp_height;
    int webp_has_animation;

    if (!data || !size) {
        return 0;
    }
    if (qw_jpeg_probe(data, size)) {
        uint32_t jpeg_width;
        uint32_t jpeg_height;
        return qw_jpeg_parse(data, size, &jpeg_width, &jpeg_height) == QW_STATUS_OK;
    }
    if (qw_webp_probe(data, size)) {
        return qw_webp_parse(data, size, &webp_width, &webp_height,
                             &webp_has_animation) == QW_STATUS_OK;
    }
    return qw_png_probe(data, size) || qw_bmp_probe(data, size) ||
           qw_gif_probe(data, size) || qw_jp2_probe(data, size) ||
           qw_pcx_probe(data, size) || qw_pfm_probe(data, size) ||
           qw_dds_probe(data, size) || qw_xpm_probe(data, size) ||
           qw_sgi_probe(data, size) || qw_ras_probe(data, size) ||
           qw_apng_probe(data, size) || qw_dcm_probe(data, size) ||
           qw_psd_probe(data, size) || qw_psb_probe(data, size) ||
           qw_exr_probe(data, size) || qw_miff_probe(data, size) ||
           qw_mng_probe(data, size) || qw_xcf_probe(data, size) ||
           qw_pcd_probe(data, size) || qw_tga_probe(data, size) ||
           qw_hdr_probe(data, size) || qw_pic_probe(data, size) ||
           qw_raw_probe(data, size) || qw_pam_probe(data, size) ||
           qw_pnm_probe(data, size) || qw_qoi_probe(data, size) ||
           qw_wbmp_probe(data, size) || qw_tiff_probe(data, size) ||
           qw_ico_probe(data, size) || qw_cur_probe(data, size);
}

int qw_image_decode(const void *data, size_t size, QwImage *out_image)
{
    QwPageImage page = {0};
    size_t channels;
    size_t stride;
    size_t expected_size;
    QwImagePixelFormat format;
    int rc;

    if (!out_image) {
        return QW_IMAGE_ERR_INVALID_ARGUMENT;
    }
    memset(out_image, 0, sizeof(*out_image));
    if (!data || !size) {
        return QW_IMAGE_ERR_INVALID_ARGUMENT;
    }

    rc = qw_image_decode_page(data, size, &page);
    if (rc != QW_IMAGE_OK) {
        return rc;
    }
    if (!page.data || !page.size || !page.width || !page.height) {
        qw_image_release_page(&page);
        return QW_IMAGE_ERR_FORMAT;
    }
    if (page.kind == QW_PAGE_IMAGE_GRAY8) {
        channels = 1u;
        format = QW_IMAGE_PIXEL_GRAY8;
    } else if (page.kind == QW_PAGE_IMAGE_RGB24) {
        channels = 3u;
        format = QW_IMAGE_PIXEL_RGB24;
    } else {
        qw_image_release_page(&page);
        return QW_IMAGE_ERR_UNSUPPORTED;
    }
    if ((size_t)page.width > SIZE_MAX / channels) {
        qw_image_release_page(&page);
        return QW_IMAGE_ERR_FORMAT;
    }
    stride = (size_t)page.width * channels;
    if ((size_t)page.height > SIZE_MAX / stride) {
        qw_image_release_page(&page);
        return QW_IMAGE_ERR_FORMAT;
    }
    expected_size = stride * (size_t)page.height;
    if (page.size != expected_size) {
        qw_image_release_page(&page);
        return QW_IMAGE_ERR_FORMAT;
    }

    out_image->pixels = (uint8_t *)page.data;
    out_image->size = page.size;
    out_image->stride = stride;
    out_image->width = page.width;
    out_image->height = page.height;
    out_image->format = format;
    page.data = NULL;
    qw_image_release_page(&page);
    return QW_IMAGE_OK;
}

void qw_image_free(QwImage *image)
{
    if (!image) {
        return;
    }
    free(image->pixels);
    memset(image, 0, sizeof(*image));
}

const char *qw_image_status_string(int status)
{
    switch (status) {
    case QW_IMAGE_OK: return "success";
    case QW_IMAGE_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QW_IMAGE_ERR_OUT_OF_MEMORY: return "out of memory";
    case QW_IMAGE_ERR_IO: return "I/O error";
    case QW_IMAGE_ERR_FORMAT: return "invalid image format";
    case QW_IMAGE_ERR_UNSUPPORTED: return "unsupported image feature";
    case QW_IMAGE_ERR_DECODE: return "image decode failed";
    case QW_IMAGE_ERR_EXTERNAL_DECODER_REQUIRED:
        return "an external decoder is required";
    case QW_IMAGE_ERR_RESOURCE_LIMIT: return "image exceeds resource limits";
    default: return "unknown image error";
    }
}
