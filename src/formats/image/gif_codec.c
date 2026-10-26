#include "image_internal.h"

#include "../../core/byte_utils.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    GIF_HEADER_SIZE = 13,
    GIF_IMAGE_SEPARATOR = 0x2c,
    GIF_EXTENSION_INTRODUCER = 0x21,
    GIF_TRAILER = 0x3b,
    GIF_EXT_GRAPHIC_CONTROL = 0xf9,
    GIF_EXT_COMMENT = 0xfe,
    GIF_EXT_APPLICATION = 0xff,
    GIF_EXT_PLAIN_TEXT = 0x01
};

typedef struct QwGifSubblocks {
    const uint8_t *bytes;
    size_t size;
    size_t position;
    size_t block_remaining;
    uint32_t bit_buffer;
    unsigned bit_count;
    int ended;
} QwGifSubblocks;

static int gif_signature(const uint8_t *bytes, size_t size)
{
    return bytes && size >= 6u &&
           (memcmp(bytes, "GIF87a", 6u) == 0 ||
            memcmp(bytes, "GIF89a", 6u) == 0);
}

static int gif_skip_subblocks(const uint8_t *bytes,
                              size_t size,
                              size_t *position)
{
    for (;;) {
        size_t block_size;
        if (*position >= size) return QW_STATUS_ERR_FORMAT;
        block_size = bytes[(*position)++];
        if (!block_size) return QW_STATUS_OK;
        if (block_size > size - *position) return QW_STATUS_ERR_FORMAT;
        *position += block_size;
    }
}

static int gif_read_extension(const uint8_t *bytes,
                              size_t size,
                              size_t *position,
                              int *control_seen,
                              int *transparent,
                              uint8_t *transparent_index)
{
    uint8_t label;

    if (*position >= size) return QW_STATUS_ERR_FORMAT;
    label = bytes[(*position)++];
    if (label == GIF_EXT_GRAPHIC_CONTROL) {
        uint8_t block_size;
        uint8_t packed;
        if (*control_seen || *position >= size) return QW_STATUS_ERR_FORMAT;
        block_size = bytes[(*position)++];
        if (block_size != 4u || size - *position < 5u) {
            return QW_STATUS_ERR_FORMAT;
        }
        packed = bytes[*position];
        if ((packed & 0xe0u) != 0u) return QW_STATUS_ERR_FORMAT;
        *transparent = (packed & 0x01u) != 0u;
        *transparent_index = bytes[*position + 3u];
        *position += 4u;
        if (bytes[(*position)++] != 0u) return QW_STATUS_ERR_FORMAT;
        *control_seen = 1;
        return QW_STATUS_OK;
    }
    if (label == GIF_EXT_PLAIN_TEXT) return QW_STATUS_ERR_UNSUPPORTED;
    if (label == GIF_EXT_APPLICATION) {
        uint8_t block_size;
        if (*position >= size) return QW_STATUS_ERR_FORMAT;
        block_size = bytes[(*position)++];
        if (block_size != 11u || block_size > size - *position) {
            return QW_STATUS_ERR_FORMAT;
        }
        *position += block_size;
    } else if (label != GIF_EXT_COMMENT) {
        /* Unknown extension labels still use the common data sub-block form. */
    }
    return gif_skip_subblocks(bytes, size, position);
}

static int gif_subblock_read_byte(QwGifSubblocks *reader, uint8_t *out_byte)
{
    if (!reader || !out_byte) return -1;
    while (!reader->block_remaining) {
        size_t block_size;
        if (reader->ended) return 0;
        if (reader->position >= reader->size) return -1;
        block_size = reader->bytes[reader->position++];
        if (!block_size) {
            reader->ended = 1;
            return 0;
        }
        if (block_size > reader->size - reader->position) return -1;
        reader->block_remaining = block_size;
    }
    if (reader->position >= reader->size) return -1;
    *out_byte = reader->bytes[reader->position++];
    --reader->block_remaining;
    return 1;
}

static int gif_subblock_read_code(QwGifSubblocks *reader,
                                  unsigned code_size,
                                  uint16_t *out_code)
{
    while (reader->bit_count < code_size) {
        uint8_t value;
        int rc = gif_subblock_read_byte(reader, &value);
        if (rc != 1) return 0;
        reader->bit_buffer |= (uint32_t)value << reader->bit_count;
        reader->bit_count += 8u;
    }
    *out_code = (uint16_t)(reader->bit_buffer &
                           ((1u << code_size) - 1u));
    reader->bit_buffer >>= code_size;
    reader->bit_count -= code_size;
    return 1;
}

static int gif_subblock_finish(QwGifSubblocks *reader)
{
    while (!reader->ended) {
        size_t block_size;
        if (reader->block_remaining) {
            if (reader->block_remaining > reader->size - reader->position) {
                return QW_STATUS_ERR_FORMAT;
            }
            reader->position += reader->block_remaining;
            reader->block_remaining = 0u;
            continue;
        }
        if (reader->position >= reader->size) return QW_STATUS_ERR_FORMAT;
        block_size = reader->bytes[reader->position++];
        if (!block_size) {
            reader->ended = 1;
            break;
        }
        if (block_size > reader->size - reader->position) {
            return QW_STATUS_ERR_FORMAT;
        }
        reader->position += block_size;
    }
    return QW_STATUS_OK;
}

static int gif_emit_index(uint8_t value,
                          uint8_t *indices,
                          size_t capacity,
                          size_t *written)
{
    if (*written >= capacity) return QW_STATUS_ERR_FORMAT;
    indices[(*written)++] = value;
    return QW_STATUS_OK;
}

static int gif_lzw_decode(QwGifSubblocks *reader,
                          uint8_t minimum_code_size,
                          uint8_t *indices,
                          size_t expected_count)
{
    uint16_t prefix[4096];
    uint8_t suffix[4096];
    uint8_t stack[4096];
    unsigned clear_code;
    unsigned end_code;
    unsigned next_code;
    unsigned code_size;
    int old_code = -1;
    uint8_t first_char = 0u;
    size_t written = 0u;
    int saw_end = 0;

    if (minimum_code_size < 2u || minimum_code_size > 8u) {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    clear_code = 1u << minimum_code_size;
    end_code = clear_code + 1u;
    next_code = clear_code + 2u;
    code_size = (unsigned)minimum_code_size + 1u;

    for (;;) {
        uint16_t code;
        unsigned input_code;
        size_t stack_size = 0u;
        int rc;

        if (!gif_subblock_read_code(reader, code_size, &code)) {
            return QW_STATUS_ERR_FORMAT;
        }
        input_code = code;
        if (input_code == clear_code) {
            next_code = clear_code + 2u;
            code_size = (unsigned)minimum_code_size + 1u;
            old_code = -1;
            continue;
        }
        if (input_code == end_code) {
            saw_end = 1;
            break;
        }
        if (old_code < 0) {
            if (input_code >= clear_code) return QW_STATUS_ERR_FORMAT;
            first_char = (uint8_t)input_code;
            rc = gif_emit_index(first_char, indices, expected_count, &written);
            if (rc != QW_STATUS_OK) return rc;
            old_code = (int)input_code;
            continue;
        }

        if (input_code >= 4096u || input_code > next_code) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (input_code == next_code) {
            stack[stack_size++] = first_char;
            input_code = (unsigned)old_code;
        }
        while (input_code >= clear_code) {
            if (input_code >= next_code || stack_size >= sizeof(stack)) {
                return QW_STATUS_ERR_FORMAT;
            }
            stack[stack_size++] = suffix[input_code];
            input_code = prefix[input_code];
        }
        if (input_code >= clear_code || stack_size >= sizeof(stack)) {
            return QW_STATUS_ERR_FORMAT;
        }
        first_char = (uint8_t)input_code;
        stack[stack_size++] = first_char;
        while (stack_size) {
            rc = gif_emit_index(stack[--stack_size], indices,
                                expected_count, &written);
            if (rc != QW_STATUS_OK) return rc;
        }

        if (next_code < 4096u) {
            prefix[next_code] = (uint16_t)old_code;
            suffix[next_code] = first_char;
            ++next_code;
            if (next_code == (1u << code_size) && code_size < 12u) {
                ++code_size;
            }
        }
        old_code = (int)code;
    }

    if (!saw_end || written != expected_count) return QW_STATUS_ERR_FORMAT;
    return gif_subblock_finish(reader);
}

static int gif_make_row_order(uint16_t height,
                              int interlaced,
                              uint16_t *row_order)
{
    size_t count = 0u;
    if (!row_order) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (!interlaced) {
        for (uint16_t y = 0u; y < height; ++y) row_order[count++] = y;
        return QW_STATUS_OK;
    }
    for (uint16_t y = 0u; y < height; y = (uint16_t)(y + 8u)) {
        row_order[count++] = y;
    }
    for (uint16_t y = 4u; y < height; y = (uint16_t)(y + 8u)) {
        row_order[count++] = y;
    }
    for (uint16_t y = 2u; y < height; y = (uint16_t)(y + 4u)) {
        row_order[count++] = y;
    }
    for (uint16_t y = 1u; y < height; y = (uint16_t)(y + 2u)) {
        row_order[count++] = y;
    }
    return count == height ? QW_STATUS_OK : QW_STATUS_ERR_FORMAT;
}

static void gif_fill_canvas(uint8_t *pixels,
                            size_t pixel_count,
                            const uint8_t color[3])
{
    for (size_t i = 0u; i < pixel_count; ++i) {
        pixels[i * 3u] = color[0];
        pixels[i * 3u + 1u] = color[1];
        pixels[i * 3u + 2u] = color[2];
    }
}

static int gif_decode_first_image(const uint8_t *bytes,
                                  size_t size,
                                  size_t *position,
                                  const uint8_t *global_palette,
                                  size_t global_palette_entries,
                                  uint8_t background_index,
                                  int control_seen,
                                  int transparent,
                                  uint8_t transparent_index,
                                  uint32_t screen_width,
                                  uint32_t screen_height,
                                  QwPageImage *out_page)
{
    uint16_t left;
    uint16_t top;
    uint16_t frame_width;
    uint16_t frame_height;
    uint8_t descriptor;
    const uint8_t *palette = global_palette;
    size_t palette_entries = global_palette_entries;
    uint8_t *local_palette = NULL;
    uint8_t *indices = NULL;
    uint16_t *row_order = NULL;
    uint8_t *canvas = NULL;
    size_t frame_pixel_count;
    size_t screen_pixel_count;
    size_t output_size;
    int interlaced;
    int rc = QW_STATUS_ERR_FORMAT;

    if (size - *position < 9u) return QW_STATUS_ERR_FORMAT;
    left = qw_read_u16_le(bytes + *position);
    top = qw_read_u16_le(bytes + *position + 2u);
    frame_width = qw_read_u16_le(bytes + *position + 4u);
    frame_height = qw_read_u16_le(bytes + *position + 6u);
    descriptor = bytes[*position + 8u];
    *position += 9u;
    if (!frame_width || !frame_height ||
        (uint32_t)left + frame_width > screen_width ||
        (uint32_t)top + frame_height > screen_height) {
        return QW_STATUS_ERR_FORMAT;
    }
    if ((descriptor & 0x18u) != 0u) return QW_STATUS_ERR_UNSUPPORTED;
    interlaced = (descriptor & 0x40u) != 0u;
    if (descriptor & 0x80u) {
        unsigned table_bits = (unsigned)(descriptor & 0x07u) + 1u;
        palette_entries = (size_t)1u << table_bits;
        if (palette_entries > (size - *position) / 3u) {
            return QW_STATUS_ERR_FORMAT;
        }
        local_palette = (uint8_t *)malloc(palette_entries * 3u);
        if (!local_palette) return QW_STATUS_ERR_OUT_OF_MEMORY;
        memcpy(local_palette, bytes + *position, palette_entries * 3u);
        palette = local_palette;
        *position += palette_entries * 3u;
    }
    if (!palette || !palette_entries) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    if (transparent && transparent_index >= palette_entries) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    if (*position >= size) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }

    {
        uint8_t minimum_code_size = bytes[(*position)++];
        QwGifSubblocks reader;
        if (!qw_checked_size_mul((size_t)frame_width, (size_t)frame_height,
                                 &frame_pixel_count) ||
            !qw_checked_size_mul((size_t)screen_width,
                                 (size_t)screen_height,
                                 &screen_pixel_count) ||
            !qw_checked_size_mul(screen_pixel_count, 3u, &output_size)) {
            rc = QW_STATUS_ERR_FORMAT;
            goto cleanup;
        }
        indices = (uint8_t *)malloc(frame_pixel_count ? frame_pixel_count : 1u);
        row_order = (uint16_t *)malloc((size_t)frame_height * sizeof(*row_order));
        canvas = (uint8_t *)malloc(output_size ? output_size : 1u);
        if (!indices || !row_order || !canvas) {
            rc = QW_STATUS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        rc = gif_make_row_order(frame_height, interlaced, row_order);
        if (rc != QW_STATUS_OK) goto cleanup;
        memset(&reader, 0, sizeof(reader));
        reader.bytes = bytes;
        reader.size = size;
        reader.position = *position;
        rc = gif_lzw_decode(&reader, minimum_code_size, indices,
                            frame_pixel_count);
        if (rc != QW_STATUS_OK) goto cleanup;
        *position = reader.position;

        {
            uint8_t background[3] = {255u, 255u, 255u};
            if (!transparent && global_palette &&
                background_index < global_palette_entries) {
                memcpy(background,
                       global_palette + (size_t)background_index * 3u, 3u);
            }
            gif_fill_canvas(canvas, screen_pixel_count, background);
        }
        for (size_t i = 0u; i < frame_pixel_count; ++i) {
            uint8_t index = indices[i];
            uint32_t x = (uint32_t)(i % frame_width);
            uint32_t y = row_order[i / frame_width];
            uint8_t *destination;
            if (index >= palette_entries) {
                rc = QW_STATUS_ERR_FORMAT;
                goto cleanup;
            }
            if (transparent && index == transparent_index) continue;
            destination = canvas +
                ((size_t)(top + y) * screen_width + left + x) * 3u;
            memcpy(destination, palette + (size_t)index * 3u, 3u);
        }
    }

    out_page->data = canvas;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = screen_width;
    out_page->height = screen_height;
    canvas = NULL;
    rc = QW_STATUS_OK;

cleanup:
    free(local_palette);
    free(indices);
    free(row_order);
    free(canvas);
    (void)control_seen;
    return rc;
}

int qw_gif_probe(const void *data, size_t size)
{
    return gif_signature((const uint8_t *)data, size);
}

int qw_gif_decode_page(const void *data,
                       size_t size,
                       QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;
    const uint8_t *global_palette = NULL;
    size_t global_palette_entries = 0u;
    size_t position = GIF_HEADER_SIZE;
    uint32_t screen_width;
    uint32_t screen_height;
    uint8_t screen_packed;
    uint8_t background_index;
    int control_seen = 0;
    int transparent = 0;
    uint8_t transparent_index = 0u;

    if (!data || !size || !out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    if (!gif_signature(bytes, size) || size < GIF_HEADER_SIZE) {
        return QW_STATUS_ERR_FORMAT;
    }
    screen_width = qw_read_u16_le(bytes + 6u);
    screen_height = qw_read_u16_le(bytes + 8u);
    screen_packed = bytes[10];
    background_index = bytes[11];
    if (!qw_image_dimensions_within_limits(screen_width, screen_height)) {
        return screen_width && screen_height
            ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_FORMAT;
    }
    if (screen_packed & 0x80u) {
        global_palette_entries = (size_t)1u <<
            ((unsigned)(screen_packed & 0x07u) + 1u);
        if (global_palette_entries > (size - position) / 3u) {
            return QW_STATUS_ERR_FORMAT;
        }
        global_palette = bytes + position;
        position += global_palette_entries * 3u;
        if (background_index >= global_palette_entries) {
            return QW_STATUS_ERR_FORMAT;
        }
    }

    while (position < size) {
        uint8_t marker = bytes[position++];
        if (marker == GIF_IMAGE_SEPARATOR) {
            return gif_decode_first_image(bytes, size, &position,
                                          global_palette,
                                          global_palette_entries,
                                          background_index, control_seen,
                                          transparent, transparent_index,
                                          screen_width, screen_height,
                                          out_page);
        }
        if (marker == GIF_EXTENSION_INTRODUCER) {
            int rc = gif_read_extension(bytes, size, &position,
                                        &control_seen, &transparent,
                                        &transparent_index);
            if (rc != QW_STATUS_OK) return rc;
            continue;
        }
        if (marker == GIF_TRAILER) return QW_STATUS_ERR_FORMAT;
        return QW_STATUS_ERR_FORMAT;
    }
    return QW_STATUS_ERR_FORMAT;
}
