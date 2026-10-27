#include "image_internal.h"

#include <openjpeg.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct QwJp2MemoryStream {
    const uint8_t *data;
    size_t size;
    size_t position;
} QwJp2MemoryStream;

static const uint8_t qw_jp2_signature[12] = {
    0x00u, 0x00u, 0x00u, 0x0cu, 0x6au, 0x50u, 0x20u, 0x20u,
    0x0du, 0x0au, 0x87u, 0x0au
};

static int qw_jp2_is_container(const uint8_t *data, size_t size)
{
    return data && size >= sizeof(qw_jp2_signature) &&
           memcmp(data, qw_jp2_signature, sizeof(qw_jp2_signature)) == 0;
}

int qw_jp2_probe(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    if (!bytes || size < 4u) return 0;
    return qw_jp2_is_container(bytes, size) ||
           (bytes[0] == 0xffu && bytes[1] == 0x4fu &&
            bytes[2] == 0xffu && bytes[3] == 0x51u);
}

static OPJ_SIZE_T qw_jp2_read(void *buffer,
                              OPJ_SIZE_T byte_count,
                              void *user_data)
{
    QwJp2MemoryStream *stream = (QwJp2MemoryStream *)user_data;
    size_t available;
    size_t count;
    if (!stream || !buffer || stream->position > stream->size) {
        return (OPJ_SIZE_T)-1;
    }
    if (stream->position == stream->size) return (OPJ_SIZE_T)-1;
    available = stream->size - stream->position;
    count = byte_count > (OPJ_SIZE_T)available
        ? available : (size_t)byte_count;
    if (!count) return 0;
    memcpy(buffer, stream->data + stream->position, count);
    stream->position += count;
    return (OPJ_SIZE_T)count;
}

static OPJ_OFF_T qw_jp2_skip(OPJ_OFF_T byte_count, void *user_data)
{
    QwJp2MemoryStream *stream = (QwJp2MemoryStream *)user_data;
    uint64_t position;
    if (!stream || byte_count < 0 || stream->position > stream->size) return -1;
    position = (uint64_t)stream->position + (uint64_t)byte_count;
    if (position > (uint64_t)stream->size) return -1;
    stream->position = (size_t)position;
    return byte_count;
}

static OPJ_BOOL qw_jp2_seek(OPJ_OFF_T position, void *user_data)
{
    QwJp2MemoryStream *stream = (QwJp2MemoryStream *)user_data;
    if (!stream || position < 0 || (uint64_t)position > (uint64_t)stream->size) {
        return OPJ_FALSE;
    }
    stream->position = (size_t)position;
    return OPJ_TRUE;
}

static uint8_t qw_jp2_sample_to_u8(const opj_image_comp_t *component,
                                   OPJ_INT32 sample)
{
    uint64_t value;
    uint64_t maximum;
    unsigned precision;
    if (!component) return 0u;
    precision = component->prec;
    if (!precision) return 0u;
    if (component->sgnd) {
        int64_t signed_sample = sample;
        signed_sample += (int64_t)1 << (precision - 1u);
        if (signed_sample < 0) signed_sample = 0;
        value = (uint64_t)signed_sample;
    } else {
        value = sample < 0 ? 0u : (uint64_t)(uint32_t)sample;
    }
    if (precision >= 63u) {
        maximum = UINT64_MAX;
    } else {
        maximum = ((uint64_t)1u << precision) - 1u;
    }
    if (value > maximum) value = maximum;
    if (precision == 8u) return (uint8_t)value;
    if (precision < 8u) return (uint8_t)((value * 255u + maximum / 2u) / maximum);
    return (uint8_t)((value * 255u + maximum / 2u) / maximum);
}

static void qw_jp2_error_callback(const char *message, void *user_data)
{
    (void)message;
    (void)user_data;
}

static void qw_jp2_warning_callback(const char *message, void *user_data)
{
    (void)message;
    (void)user_data;
}

static void qw_jp2_info_callback(const char *message, void *user_data)
{
    (void)message;
    (void)user_data;
}

int qw_jp2_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwJp2MemoryStream memory = {0};
    opj_stream_t *stream = NULL;
    opj_codec_t *codec = NULL;
    opj_image_t *image = NULL;
    opj_dparameters_t parameters;
    OPJ_CODEC_FORMAT format;
    uint32_t width;
    uint32_t height;
    size_t pixels;
    size_t output_size;
    size_t components;
    uint8_t *output = NULL;
    int rc = QW_STATUS_ERR_DECODE;

    if (!bytes || !size || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    if (!qw_jp2_probe(bytes, size)) return QW_STATUS_ERR_UNSUPPORTED;
    if (qw_jp2_is_container(bytes, size)) {
        format = OPJ_CODEC_JP2;
    } else {
        format = OPJ_CODEC_J2K;
    }

    memory.data = bytes;
    memory.size = size;
    stream = opj_stream_default_create(OPJ_TRUE);
    codec = opj_create_decompress(format);
    if (!stream || !codec) goto cleanup;
    opj_stream_set_user_data(stream, &memory, NULL);
    opj_stream_set_user_data_length(stream, (OPJ_UINT64)size);
    opj_stream_set_read_function(stream, qw_jp2_read);
    opj_stream_set_skip_function(stream, qw_jp2_skip);
    opj_stream_set_seek_function(stream, qw_jp2_seek);
    opj_set_error_handler(codec, qw_jp2_error_callback, NULL);
    opj_set_warning_handler(codec, qw_jp2_warning_callback, NULL);
    opj_set_info_handler(codec, qw_jp2_info_callback, NULL);
    opj_set_default_decoder_parameters(&parameters);
    if (!opj_setup_decoder(codec, &parameters) ||
        !opj_read_header(stream, codec, &image) || !image ||
        !opj_decode(codec, stream, image) ||
        !opj_end_decompress(codec, stream)) {
        goto cleanup;
    }
    if (image->x1 <= image->x0 || image->y1 <= image->y0 ||
        image->numcomps == 0u || image->numcomps > 4u) {
        rc = QW_STATUS_ERR_UNSUPPORTED;
        goto cleanup;
    }
    width = image->x1 - image->x0;
    height = image->y1 - image->y0;
    if (!qw_image_dimensions_within_limits(width, height)) {
        rc = QW_STATUS_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    for (OPJ_UINT32 component_index = 0u;
         component_index < image->numcomps; ++component_index) {
        const opj_image_comp_t *component = &image->comps[component_index];
        if (!component->data || component->w != width ||
            component->h != height || component->prec == 0u ||
            component->prec > 32u) {
            rc = QW_STATUS_ERR_UNSUPPORTED;
            goto cleanup;
        }
    }
    components = image->numcomps >= 3u ? 3u : 1u;
    if (!qw_checked_size_mul((size_t)width, (size_t)height, &pixels) ||
        !qw_checked_size_mul(pixels, components, &output_size)) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    output = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!output) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (size_t y = 0u; y < (size_t)height; ++y) {
        for (size_t x = 0u; x < (size_t)width; ++x) {
            size_t pixel = y * (size_t)width + x;
            size_t component_index = pixel;
            uint8_t red = qw_jp2_sample_to_u8(&image->comps[0],
                                              image->comps[0].data[component_index]);
            uint8_t green = image->numcomps >= 3u
                ? qw_jp2_sample_to_u8(&image->comps[1], image->comps[1].data[component_index])
                : red;
            uint8_t blue = image->numcomps >= 3u
                ? qw_jp2_sample_to_u8(&image->comps[2], image->comps[2].data[component_index])
                : red;
            uint8_t alpha = image->numcomps == 2u
                ? qw_jp2_sample_to_u8(&image->comps[1], image->comps[1].data[component_index])
                : image->numcomps >= 4u
                    ? qw_jp2_sample_to_u8(&image->comps[3], image->comps[3].data[component_index])
                    : 255u;
            if (components == 1u) {
                output[pixel] = (uint8_t)((red * alpha + 255u * (255u - alpha) + 127u) / 255u);
            } else {
                output[pixel * 3u] = (uint8_t)((red * alpha + 255u * (255u - alpha) + 127u) / 255u);
                output[pixel * 3u + 1u] = (uint8_t)((green * alpha + 255u * (255u - alpha) + 127u) / 255u);
                output[pixel * 3u + 2u] = (uint8_t)((blue * alpha + 255u * (255u - alpha) + 127u) / 255u);
            }
        }
    }
    out_page->data = output;
    out_page->size = output_size;
    out_page->kind = components == 1u ? QW_PAGE_IMAGE_GRAY8 : QW_PAGE_IMAGE_RGB24;
    out_page->width = width;
    out_page->height = height;
    output = NULL;
    rc = QW_STATUS_OK;

cleanup:
    free(output);
    if (image) opj_image_destroy(image);
    if (codec) opj_destroy_codec(codec);
    if (stream) opj_stream_destroy(stream);
    return rc;
}
