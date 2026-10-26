#include "image_internal.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t qoi_end_marker[8] = {
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u
};




static unsigned qoi_hash(const uint8_t pixel[4])
{
    return (unsigned)(pixel[0] * 3u + pixel[1] * 5u +
                      pixel[2] * 7u + pixel[3] * 11u) & 63u;
}

static int qoi_pixel_equal(const uint8_t left[4], const uint8_t right[4])
{
    return left[0] == right[0] && left[1] == right[1] &&
           left[2] == right[2] && left[3] == right[3];
}

int qw_qoi_probe(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t width;
    uint32_t height;
    size_t pixels;

    if (!bytes || size < 22u || memcmp(bytes, "qoif", 4u) != 0 ||
        memcmp(bytes + size - sizeof(qoi_end_marker), qoi_end_marker,
               sizeof(qoi_end_marker)) != 0) {
        return 0;
    }
    width = qw_read_u32_be(bytes + 4u);
    height = qw_read_u32_be(bytes + 8u);
    if (!width || !height || width > (uint32_t)INT_MAX ||
        height > (uint32_t)INT_MAX ||
        (bytes[12] != 3u && bytes[12] != 4u) || bytes[13] > 1u ||
        !qw_checked_size_mul((size_t)width, (size_t)height, &pixels) ||
        pixels > SIZE_MAX / 3u) {
        return 0;
    }
    return 1;
}

static uint8_t qoi_composite_white(uint8_t color, uint8_t alpha)
{
    uint32_t value = (uint32_t)color * alpha +
                     255u * (255u - alpha) + 127u;
    return (uint8_t)(value / 255u);
}

int qw_qoi_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint8_t index[64][4] = {{0}};
    uint8_t pixel[4] = {0u, 0u, 0u, 255u};
    uint8_t *rgb = NULL;
    uint32_t width;
    uint32_t height;
    uint8_t channels;
    size_t pixel_count;
    size_t output_size;
    size_t cursor = 14u;
    size_t data_end;
    size_t produced;
    unsigned run_remaining = 0u;
    int rc = QW_STATUS_ERR_DECODE;

    if (!data || !out_page) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    memset(out_page, 0, sizeof(*out_page));
    if (!qw_qoi_probe(data, size)) {
        return QW_STATUS_ERR_FORMAT;
    }
    width = qw_read_u32_be(bytes + 4u);
    height = qw_read_u32_be(bytes + 8u);
    if (!qw_image_dimensions_within_limits(width, height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    channels = bytes[12];
    if (!qw_checked_size_mul((size_t)width, (size_t)height, &pixel_count) ||
        !qw_checked_size_mul(pixel_count, 3u, &output_size)) {
        return QW_STATUS_ERR_FORMAT;
    }
    rgb = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!rgb) {
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    data_end = size - sizeof(qoi_end_marker);

    for (produced = 0; produced < pixel_count; ++produced) {
        if (run_remaining) {
            --run_remaining;
        } else {
            uint8_t op;
            if (cursor >= data_end) {
                goto done;
            }
            op = bytes[cursor++];
            if (op == 0xfeu) {
                if (data_end - cursor < 3u) {
                    goto done;
                }
                pixel[0] = bytes[cursor++];
                pixel[1] = bytes[cursor++];
                pixel[2] = bytes[cursor++];
            } else if (op == 0xffu) {
                if (data_end - cursor < 4u) {
                    goto done;
                }
                pixel[0] = bytes[cursor++];
                pixel[1] = bytes[cursor++];
                pixel[2] = bytes[cursor++];
                pixel[3] = bytes[cursor++];
            } else {
                switch (op & 0xc0u) {
                case 0x00u:
                    memcpy(pixel, index[op & 0x3fu], sizeof(pixel));
                    break;
                case 0x40u:
                    pixel[0] = (uint8_t)(pixel[0] + ((op >> 4) & 0x03u) - 2);
                    pixel[1] = (uint8_t)(pixel[1] + ((op >> 2) & 0x03u) - 2);
                    pixel[2] = (uint8_t)(pixel[2] + (op & 0x03u) - 2);
                    break;
                case 0x80u: {
                    uint8_t second;
                    int dg;
                    int dr_dg;
                    int db_dg;
                    if (cursor >= data_end) {
                        goto done;
                    }
                    second = bytes[cursor++];
                    dg = (int)(op & 0x3fu) - 32;
                    dr_dg = (int)(second >> 4) - 8;
                    db_dg = (int)(second & 0x0fu) - 8;
                    pixel[0] = (uint8_t)(pixel[0] + dg + dr_dg);
                    pixel[1] = (uint8_t)(pixel[1] + dg);
                    pixel[2] = (uint8_t)(pixel[2] + dg + db_dg);
                    break;
                }
                case 0xc0u: {
                    unsigned run_length = (unsigned)(op & 0x3fu) + 1u;
                    if ((size_t)run_length > pixel_count - produced) {
                        goto done;
                    }
                    run_remaining = run_length - 1u;
                    break;
                }
                }
            }
        }

        memcpy(index[qoi_hash(pixel)], pixel, sizeof(pixel));
        if (channels == 4u || pixel[3] != 255u) {
            rgb[produced * 3u] = qoi_composite_white(pixel[0], pixel[3]);
            rgb[produced * 3u + 1u] = qoi_composite_white(pixel[1], pixel[3]);
            rgb[produced * 3u + 2u] = qoi_composite_white(pixel[2], pixel[3]);
        } else {
            rgb[produced * 3u] = pixel[0];
            rgb[produced * 3u + 1u] = pixel[1];
            rgb[produced * 3u + 2u] = pixel[2];
        }
    }

    if (cursor != data_end) {
        goto done;
    }
    out_page->data = rgb;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = width;
    out_page->height = height;
    rgb = NULL;
    rc = QW_STATUS_OK;

done:
    free(rgb);
    return rc;
}

static int qoi_write_byte(uint8_t *data,
                          size_t capacity,
                          size_t *cursor,
                          uint8_t value)
{
    if (!data || !cursor || *cursor >= capacity) {
        return 0;
    }
    data[(*cursor)++] = value;
    return 1;
}

static int qoi_wrapped_difference(uint8_t current, uint8_t previous)
{
    int difference = (int)current - (int)previous;
    if (difference > 127) {
        difference -= 256;
    } else if (difference < -128) {
        difference += 256;
    }
    return difference;
}

int qw_qoi_encode_page(const QwPageImage *page,
                       uint8_t **out_data,
                       size_t *out_size)
{
    uint8_t index[64][4] = {{0}};
    uint8_t previous[4] = {0u, 0u, 0u, 255u};
    uint8_t *encoded = NULL;
    size_t pixel_count;
    size_t row_bytes;
    size_t source_size;
    size_t capacity;
    size_t cursor = 0u;
    size_t i;
    unsigned run = 0u;
    unsigned channels;
    int rc = QW_STATUS_ERR_FORMAT;

    if (!page || !out_data || !out_size) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_data = NULL;
    *out_size = 0u;
    if (!page->data || !page->width || !page->height) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (page->kind == QW_PAGE_IMAGE_GRAY8) {
        channels = 1u;
    } else if (page->kind == QW_PAGE_IMAGE_RGB24) {
        channels = 3u;
    } else {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (!qw_checked_size_mul((size_t)page->width, (size_t)page->height,
                              &pixel_count) ||
        !qw_checked_size_mul((size_t)page->width, (size_t)channels,
                              &row_bytes) ||
        !qw_checked_size_mul(row_bytes, (size_t)page->height, &source_size) ||
        source_size > page->size || pixel_count > (SIZE_MAX - 22u) / 5u) {
        return QW_STATUS_ERR_FORMAT;
    }
    capacity = 22u + pixel_count * 5u;
    encoded = (uint8_t *)malloc(capacity);
    if (!encoded) {
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }

    memcpy(encoded, "qoif", 4u);
    qw_write_u32_be(encoded + 4u, page->width);
    qw_write_u32_be(encoded + 8u, page->height);
    encoded[12] = 3u;
    encoded[13] = 0u;
    cursor = 14u;

    for (i = 0u; i < pixel_count; ++i) {
        uint8_t pixel[4];
        unsigned hash;
        int dr;
        int dg;
        int db;
        size_t source = i * channels;

        if (channels == 1u) {
            pixel[0] = page->data[source];
            pixel[1] = pixel[0];
            pixel[2] = pixel[0];
        } else {
            pixel[0] = page->data[source];
            pixel[1] = page->data[source + 1u];
            pixel[2] = page->data[source + 2u];
        }
        pixel[3] = 255u;

        if (qoi_pixel_equal(pixel, previous)) {
            ++run;
            if (run == 62u || i + 1u == pixel_count) {
                if (!qoi_write_byte(encoded, capacity, &cursor,
                                    (uint8_t)(0xc0u | (run - 1u)))) {
                    goto done;
                }
                run = 0u;
            }
        } else {
            if (run) {
                if (!qoi_write_byte(encoded, capacity, &cursor,
                                    (uint8_t)(0xc0u | (run - 1u)))) {
                    goto done;
                }
                run = 0u;
            }
            hash = qoi_hash(pixel);
            if (qoi_pixel_equal(index[hash], pixel)) {
                if (!qoi_write_byte(encoded, capacity, &cursor,
                                    (uint8_t)hash)) {
                    goto done;
                }
            } else {
                int dr_dg;
                int db_dg;
                memcpy(index[hash], pixel, sizeof(pixel));
                dr = qoi_wrapped_difference(pixel[0], previous[0]);
                dg = qoi_wrapped_difference(pixel[1], previous[1]);
                db = qoi_wrapped_difference(pixel[2], previous[2]);
                if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 &&
                    db >= -2 && db <= 1) {
                    uint8_t op = (uint8_t)(0x40u |
                        ((unsigned)(dr + 2) << 4) |
                        ((unsigned)(dg + 2) << 2) |
                        (unsigned)(db + 2));
                    if (!qoi_write_byte(encoded, capacity, &cursor, op)) {
                        goto done;
                    }
                } else {
                    dr_dg = dr - dg;
                    db_dg = db - dg;
                    if (dg >= -32 && dg <= 31 &&
                        dr_dg >= -8 && dr_dg <= 7 &&
                        db_dg >= -8 && db_dg <= 7) {
                        if (!qoi_write_byte(encoded, capacity, &cursor,
                                (uint8_t)(0x80u | (unsigned)(dg + 32))) ||
                            !qoi_write_byte(encoded, capacity, &cursor,
                                (uint8_t)(((unsigned)(dr_dg + 8) << 4) |
                                          (unsigned)(db_dg + 8)))) {
                            goto done;
                        }
                    } else if (!qoi_write_byte(encoded, capacity, &cursor,
                                               0xfeu) ||
                               !qoi_write_byte(encoded, capacity, &cursor,
                                               pixel[0]) ||
                               !qoi_write_byte(encoded, capacity, &cursor,
                                               pixel[1]) ||
                               !qoi_write_byte(encoded, capacity, &cursor,
                                               pixel[2])) {
                        goto done;
                    }
                }
            }
            memcpy(previous, pixel, sizeof(previous));
        }
        memcpy(index[qoi_hash(pixel)], pixel, sizeof(pixel));
    }

    memcpy(encoded + cursor, qoi_end_marker, sizeof(qoi_end_marker));
    cursor += sizeof(qoi_end_marker);
    *out_data = encoded;
    *out_size = cursor;
    encoded = NULL;
    rc = QW_STATUS_OK;

done:
    free(encoded);
    return rc;
}
