#include "image_internal.h"

#include <stdlib.h>
#include <string.h>

enum {
    WEBP_LOSSLESS_MAX_CODE_LENGTH = 15,
    WEBP_LOSSLESS_MAX_GROUPS = 1024,
    WEBP_LOSSLESS_MAX_CACHE_BITS = 11,
    WEBP_LOSSLESS_MAX_BACKREF_LENGTH = 4096
};

typedef struct WebPLBitReader {
    const uint8_t *data;
    size_t size;
    size_t byte_position;
    unsigned bit_position;
    int failed;
} WebPLBitReader;

typedef struct WebPLHuffman {
    uint16_t *symbols;
    size_t symbol_count;
    uint32_t counts[WEBP_LOSSLESS_MAX_CODE_LENGTH + 1];
    uint32_t first_codes[WEBP_LOSSLESS_MAX_CODE_LENGTH + 1];
    uint32_t first_symbols[WEBP_LOSSLESS_MAX_CODE_LENGTH + 1];
    uint32_t max_length;
} WebPLHuffman;

typedef struct WebPLPrefixGroup {
    WebPLHuffman codes[5];
} WebPLPrefixGroup;

static const int8_t webp_distance_map[120][2] = {
    { 0, 1}, { 1, 0}, { 1, 1}, {-1, 1}, { 0, 2}, { 2, 0}, { 1, 2}, {-1, 2},
    { 2, 1}, {-2, 1}, { 2, 2}, {-2, 2}, { 0, 3}, { 3, 0}, { 1, 3}, {-1, 3},
    { 3, 1}, {-3, 1}, { 2, 3}, {-2, 3}, { 3, 2}, {-3, 2}, { 0, 4}, { 4, 0},
    { 1, 4}, {-1, 4}, { 4, 1}, {-4, 1}, { 3, 3}, {-3, 3}, { 2, 4}, {-2, 4},
    { 4, 2}, {-4, 2}, { 0, 5}, { 3, 4}, {-3, 4}, { 4, 3}, {-4, 3}, { 5, 0},
    { 1, 5}, {-1, 5}, { 5, 1}, {-5, 1}, { 2, 5}, {-2, 5}, { 5, 2}, {-5, 2},
    { 4, 4}, {-4, 4}, { 3, 5}, {-3, 5}, { 5, 3}, {-5, 3}, { 0, 6}, { 6, 0},
    { 1, 6}, {-1, 6}, { 6, 1}, {-6, 1}, { 2, 6}, {-2, 6}, { 6, 2}, {-6, 2},
    { 4, 5}, {-4, 5}, { 5, 4}, {-5, 4}, { 3, 6}, {-3, 6}, { 6, 3}, {-6, 3},
    { 0, 7}, { 7, 0}, { 1, 7}, {-1, 7}, { 5, 5}, {-5, 5}, { 7, 1}, {-7, 1},
    { 4, 6}, {-4, 6}, { 6, 4}, {-6, 4}, { 2, 7}, {-2, 7}, { 7, 2}, {-7, 2},
    { 3, 7}, {-3, 7}, { 7, 3}, {-7, 3}, { 5, 6}, {-5, 6}, { 6, 5}, {-6, 5},
    { 8, 0}, { 4, 7}, {-4, 7}, { 7, 4}, {-7, 4}, { 8, 1}, { 8, 2}, { 6, 6},
    {-6, 6}, { 8, 3}, { 5, 7}, {-5, 7}, { 7, 5}, {-7, 5}, { 8, 4}, { 6, 7},
    {-6, 7}, { 7, 6}, {-7, 6}, { 8, 5}, { 7, 7}, {-7, 7}, { 8, 6}, { 8, 7}
};

static uint32_t webpl_read_bits(WebPLBitReader *reader, unsigned count)
{
    uint32_t value = 0u;
    if (!reader || count > 32u || reader->failed) return 0u;
    for (unsigned i = 0u; i < count; ++i) {
        if (reader->byte_position >= reader->size) {
            reader->failed = 1;
            return 0u;
        }
        value |= ((uint32_t)(reader->data[reader->byte_position] >>
                             reader->bit_position) & 1u) << i;
        ++reader->bit_position;
        if (reader->bit_position == 8u) {
            reader->bit_position = 0u;
            ++reader->byte_position;
        }
    }
    return value;
}

static void webpl_huffman_clear(WebPLHuffman *tree)
{
    if (!tree) return;
    free(tree->symbols);
    memset(tree, 0, sizeof(*tree));
}

static int webpl_huffman_build(const uint8_t *lengths, size_t alphabet_size,
                               WebPLHuffman *tree)
{
    size_t used = 0u;
    size_t output_position = 0u;
    int32_t remaining = 1;
    uint32_t code = 0u;
    if (!lengths || !tree || alphabet_size == 0u || alphabet_size > UINT16_MAX)
        return QW_STATUS_ERR_FORMAT;
    memset(tree, 0, sizeof(*tree));
    for (size_t i = 0u; i < alphabet_size; ++i) {
        uint8_t length = lengths[i];
        if (length > WEBP_LOSSLESS_MAX_CODE_LENGTH)
            return QW_STATUS_ERR_FORMAT;
        if (length) {
            ++tree->counts[length];
            ++used;
            if (length > tree->max_length) tree->max_length = length;
        }
    }
    if (!used) return QW_STATUS_OK;
    for (unsigned length = 1u;
         length <= WEBP_LOSSLESS_MAX_CODE_LENGTH; ++length) {
        remaining = remaining * 2 - (int32_t)tree->counts[length];
        if (remaining < 0) return QW_STATUS_ERR_FORMAT;
    }
    /* VP8L permits incomplete trees; only oversubscribed trees are invalid. */
    tree->symbols = (uint16_t *)malloc(used * sizeof(*tree->symbols));
    if (!tree->symbols) return QW_STATUS_ERR_OUT_OF_MEMORY;
    tree->symbol_count = used;
    for (unsigned length = 1u;
         length <= WEBP_LOSSLESS_MAX_CODE_LENGTH; ++length) {
        code = (code + tree->counts[length - 1u]) << 1;
        tree->first_codes[length] = code;
        tree->first_symbols[length] = (uint32_t)output_position;
        for (size_t symbol = 0u; symbol < alphabet_size; ++symbol) {
            if (lengths[symbol] == length)
                tree->symbols[output_position++] = (uint16_t)symbol;
        }
    }
    return output_position == used ? QW_STATUS_OK : QW_STATUS_ERR_FORMAT;
}

static int webpl_huffman_read_symbol(WebPLBitReader *reader,
                                     const WebPLHuffman *tree,
                                     uint32_t *out_symbol)
{
    uint32_t code = 0u;
    if (!reader || !tree || !out_symbol || !tree->symbol_count)
        return 0;
    if (tree->symbol_count == 1u) {
        *out_symbol = tree->symbols[0];
        return 1;
    }
    for (uint32_t length = 1u; length <= tree->max_length; ++length) {
        uint32_t bit = webpl_read_bits(reader, 1u);
        uint32_t first = tree->first_codes[length];
        if (reader->failed) return 0;
        code = (code << 1u) | bit;
        if (code >= first && code - first < tree->counts[length]) {
            size_t symbol_index = tree->first_symbols[length] + code - first;
            if (symbol_index >= tree->symbol_count) return 0;
            *out_symbol = tree->symbols[symbol_index];
            return 1;
        }
    }
    return 0;
}

static int webpl_read_huffman(WebPLBitReader *reader, size_t alphabet_size,
                              WebPLHuffman *out_tree)
{
    static const uint8_t code_length_order[19] = {
        17u, 18u, 0u, 1u, 2u, 3u, 4u, 5u, 16u, 6u,
        7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u
    };
    uint8_t *lengths = NULL;
    uint8_t code_length_lengths[19] = {0};
    WebPLHuffman code_length_tree = {0};
    int rc = QW_STATUS_ERR_FORMAT;

    if (!reader || !out_tree || alphabet_size == 0u ||
        alphabet_size > UINT16_MAX) {
        return QW_STATUS_ERR_FORMAT;
    }
    memset(out_tree, 0, sizeof(*out_tree));
    lengths = (uint8_t *)calloc(alphabet_size, sizeof(*lengths));
    if (!lengths) return QW_STATUS_ERR_OUT_OF_MEMORY;
    if (webpl_read_bits(reader, 1u)) {
        uint32_t symbol_count = webpl_read_bits(reader, 1u) + 1u;
        uint32_t first_is_8_bits = webpl_read_bits(reader, 1u);
        uint32_t symbol0 = webpl_read_bits(reader, 1u + 7u * first_is_8_bits);
        if (reader->failed || symbol0 >= alphabet_size) goto done;
        lengths[symbol0] = 1u;
        if (symbol_count == 2u) {
            uint32_t symbol1 = webpl_read_bits(reader, 8u);
            if (reader->failed || symbol1 >= alphabet_size) goto done;
            lengths[symbol1] = 1u;
        }
    } else {
        uint32_t num_code_lengths = 4u + webpl_read_bits(reader, 4u);
        uint32_t max_symbols;
        uint32_t position = 0u;
        uint8_t previous_nonzero = 0u;
        if (reader->failed) goto done;
        for (uint32_t i = 0u; i < num_code_lengths; ++i) {
            code_length_lengths[code_length_order[i]] =
                (uint8_t)webpl_read_bits(reader, 3u);
            if (reader->failed) goto done;
        }
        rc = webpl_huffman_build(code_length_lengths,
                                 sizeof(code_length_lengths),
                                 &code_length_tree);
        if (rc != QW_STATUS_OK || !code_length_tree.symbol_count) {
            rc = QW_STATUS_ERR_FORMAT;
            goto done;
        }
        if (!webpl_read_bits(reader, 1u)) {
            max_symbols = (uint32_t)alphabet_size;
        } else {
            uint32_t length_bits = 2u + 2u * webpl_read_bits(reader, 3u);
            max_symbols = 2u + webpl_read_bits(reader, length_bits);
        }
        if (reader->failed || max_symbols > alphabet_size) {
            rc = QW_STATUS_ERR_FORMAT;
            goto done;
        }
        while (position < alphabet_size) {
            uint32_t symbol;
            uint32_t repeat;
            if (max_symbols == 0u) break;
            --max_symbols;
            if (!webpl_huffman_read_symbol(reader, &code_length_tree,
                                           &symbol)) {
                rc = QW_STATUS_ERR_FORMAT;
                goto done;
            }
            if (symbol <= 15u) {
                lengths[position++] = (uint8_t)symbol;
                if (symbol) previous_nonzero = (uint8_t)symbol;
                continue;
            }
            if (symbol == 16u) {
                uint8_t repeated = previous_nonzero ? previous_nonzero : 8u;
                repeat = 3u + webpl_read_bits(reader, 2u);
                if (reader->failed || repeat > alphabet_size - position) {
                    rc = QW_STATUS_ERR_FORMAT;
                    goto done;
                }
                while (repeat--) lengths[position++] = repeated;
                previous_nonzero = repeated;
            } else if (symbol == 17u) {
                repeat = 3u + webpl_read_bits(reader, 3u);
                if (reader->failed) {
                    rc = QW_STATUS_ERR_FORMAT;
                    goto done;
                }
                if (repeat > alphabet_size - position) {
                    rc = QW_STATUS_ERR_FORMAT;
                    goto done;
                }
                position += repeat;
            } else if (symbol == 18u) {
                repeat = 11u + webpl_read_bits(reader, 7u);
                if (reader->failed) {
                    rc = QW_STATUS_ERR_FORMAT;
                    goto done;
                }
                if (repeat > alphabet_size - position) {
                    rc = QW_STATUS_ERR_FORMAT;
                    goto done;
                }
                position += repeat;
            } else {
                rc = QW_STATUS_ERR_FORMAT;
                goto done;
            }
        }
    }
    if (reader->failed) {
        rc = QW_STATUS_ERR_FORMAT;
        goto done;
    }
    rc = webpl_huffman_build(lengths, alphabet_size, out_tree);

done:
    webpl_huffman_clear(&code_length_tree);
    free(lengths);
    return rc;
}

static void webpl_prefix_group_clear(WebPLPrefixGroup *group)
{
    if (!group) return;
    for (size_t i = 0u; i < 5u; ++i)
        webpl_huffman_clear(&group->codes[i]);
}

static void webpl_prefix_groups_clear(WebPLPrefixGroup *groups,
                                      size_t group_count)
{
    if (!groups) return;
    for (size_t i = 0u; i < group_count; ++i)
        webpl_prefix_group_clear(&groups[i]);
    free(groups);
}

static int webpl_read_prefix_group(WebPLBitReader *reader,
                                   size_t cache_size,
                                   WebPLPrefixGroup *group)
{
    size_t green_size = 256u + 24u + cache_size;
    int rc;
    rc = webpl_read_huffman(reader, green_size, &group->codes[0]);
    if (rc != QW_STATUS_OK) return rc;
    rc = webpl_read_huffman(reader, 256u, &group->codes[1]);
    if (rc != QW_STATUS_OK) return rc;
    rc = webpl_read_huffman(reader, 256u, &group->codes[2]);
    if (rc != QW_STATUS_OK) return rc;
    rc = webpl_read_huffman(reader, 256u, &group->codes[3]);
    if (rc != QW_STATUS_OK) return rc;
    rc = webpl_read_huffman(reader, 40u, &group->codes[4]);
    return rc;
}

static int webpl_read_prefix_value(WebPLBitReader *reader, uint32_t prefix,
                                   uint32_t *out_value)
{
    uint32_t extra_bits;
    uint32_t offset;
    uint32_t extra;
    if (!reader || !out_value || prefix >= 40u) return 0;
    if (prefix < 4u) {
        *out_value = prefix + 1u;
        return 1;
    }
    extra_bits = (prefix - 2u) >> 1;
    offset = (2u + (prefix & 1u)) << extra_bits;
    extra = webpl_read_bits(reader, extra_bits);
    if (reader->failed || offset > UINT32_MAX - extra - 1u) return 0;
    *out_value = offset + extra + 1u;
    return 1;
}

static uint32_t webpl_cache_index(uint32_t pixel, unsigned cache_bits)
{
    return (uint32_t)((UINT32_C(0x1e35a7bd) * pixel) >> (32u - cache_bits));
}

static void webpl_cache_insert(uint32_t *cache, size_t cache_size,
                               unsigned cache_bits, uint32_t pixel)
{
    if (cache) cache[webpl_cache_index(pixel, cache_bits) % cache_size] = pixel;
}

static int webpl_decode_image_data(WebPLBitReader *reader,
                                   uint32_t width,
                                   uint32_t height,
                                   int allow_meta_prefix,
                                   uint32_t **out_pixels)
{
    uint32_t *pixels = NULL;
    uint32_t *entropy_image = NULL;
    uint32_t *cache = NULL;
    WebPLPrefixGroup *groups = NULL;
    size_t pixel_count;
    size_t cache_size = 0u;
    unsigned cache_bits = 0u;
    uint32_t prefix_bits = 0u;
    uint32_t prefix_width = 0u;
    size_t group_count = 1u;
    size_t decoded = 0u;
    int has_meta_prefix = 0;
    int rc = QW_STATUS_ERR_FORMAT;

    if (!reader || !out_pixels) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pixels = NULL;
    if (!qw_image_dimensions_within_limits(width, height) ||
        !qw_checked_size_mul((size_t)width, (size_t)height, &pixel_count) ||
        pixel_count > SIZE_MAX / sizeof(*pixels)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (webpl_read_bits(reader, 1u)) {
        cache_bits = webpl_read_bits(reader, 4u);
        if (reader->failed || cache_bits == 0u ||
            cache_bits > WEBP_LOSSLESS_MAX_CACHE_BITS) {
            return QW_STATUS_ERR_FORMAT;
        }
        cache_size = (size_t)1u << cache_bits;
        cache = (uint32_t *)calloc(cache_size, sizeof(*cache));
        if (!cache) return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    if (allow_meta_prefix) {
        has_meta_prefix = (int)webpl_read_bits(reader, 1u);
        if (reader->failed) goto done;
        if (has_meta_prefix) {
            uint32_t max_group = 0u;
            uint32_t prefix_shift;
            prefix_bits = 2u + webpl_read_bits(reader, 3u);
            if (reader->failed) goto done;
            prefix_shift = 1u << prefix_bits;
            prefix_width = (width >> prefix_bits) +
                ((width & (prefix_shift - 1u)) != 0u);
            {
                uint32_t prefix_height = (height >> prefix_bits) +
                    ((height & (prefix_shift - 1u)) != 0u);
                rc = webpl_decode_image_data(reader, prefix_width,
                                             prefix_height, 0,
                                             &entropy_image);
                if (rc != QW_STATUS_OK) goto done;
            }
            {
                size_t entropy_count =
                    (size_t)prefix_width * ((height >> prefix_bits) +
                    ((height & (prefix_shift - 1u)) != 0u));
                for (size_t i = 0u; i < entropy_count; ++i) {
                    uint32_t group = (entropy_image[i] >> 8) & 0xffffu;
                    if (group > max_group) max_group = group;
                }
            }
            group_count = (size_t)max_group + 1u;
            if (!group_count || group_count > WEBP_LOSSLESS_MAX_GROUPS) {
                rc = QW_STATUS_ERR_RESOURCE_LIMIT;
                goto done;
            }
        }
    }
    groups = (WebPLPrefixGroup *)calloc(group_count, sizeof(*groups));
    if (!groups) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto done;
    }
    for (size_t i = 0u; i < group_count; ++i) {
        rc = webpl_read_prefix_group(reader, cache_size, &groups[i]);
        if (rc != QW_STATUS_OK) goto done;
    }
    pixels = (uint32_t *)malloc(pixel_count * sizeof(*pixels));
    if (!pixels) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto done;
    }

    while (decoded < pixel_count) {
        size_t group_index = 0u;
        WebPLPrefixGroup *group;
        uint32_t symbol;
        uint32_t pixel;
        if (has_meta_prefix) {
            uint32_t x = (uint32_t)(decoded % width);
            uint32_t y = (uint32_t)(decoded / width);
            size_t entropy_index =
                (size_t)(y >> prefix_bits) * prefix_width +
                (size_t)(x >> prefix_bits);
            uint32_t code = (entropy_image[entropy_index] >> 8) & 0xffffu;
            if (code >= group_count) {
                rc = QW_STATUS_ERR_FORMAT;
                goto done;
            }
            group_index = code;
        }
        group = &groups[group_index];
        if (!webpl_huffman_read_symbol(reader, &group->codes[0], &symbol)) {
            rc = QW_STATUS_ERR_FORMAT;
            goto done;
        }
        if (symbol < 256u) {
            uint32_t red;
            uint32_t blue;
            uint32_t alpha;
            if (!webpl_huffman_read_symbol(reader, &group->codes[1], &red) ||
                !webpl_huffman_read_symbol(reader, &group->codes[2], &blue) ||
                !webpl_huffman_read_symbol(reader, &group->codes[3], &alpha) ||
                red > 255u || blue > 255u || alpha > 255u) {
                rc = QW_STATUS_ERR_FORMAT;
                goto done;
            }
            pixel = (alpha << 24) | (red << 16) | (symbol << 8) | blue;
            pixels[decoded++] = pixel;
            webpl_cache_insert(cache, cache_size, cache_bits, pixel);
        } else if (symbol < 280u) {
            uint32_t length;
            uint32_t distance_prefix;
            uint32_t distance_code;
            uint64_t distance;
            if (!webpl_read_prefix_value(reader, symbol - 256u, &length) ||
                length > WEBP_LOSSLESS_MAX_BACKREF_LENGTH ||
                !webpl_huffman_read_symbol(reader, &group->codes[4],
                                           &distance_prefix) ||
                !webpl_read_prefix_value(reader, distance_prefix,
                                         &distance_code)) {
                rc = QW_STATUS_ERR_FORMAT;
                goto done;
            }
            if (distance_code > 120u) {
                distance = distance_code - 120u;
            } else {
                const int8_t *offset = webp_distance_map[distance_code - 1u];
                int64_t mapped = (int64_t)offset[0] +
                                 (int64_t)offset[1] * width;
                distance = mapped < 1 ? 1u : (uint64_t)mapped;
            }
            if (!distance || distance > decoded ||
                length > pixel_count - decoded) {
                rc = QW_STATUS_ERR_FORMAT;
                goto done;
            }
            for (uint32_t i = 0u; i < length; ++i) {
                pixel = pixels[decoded - (size_t)distance];
                pixels[decoded++] = pixel;
                webpl_cache_insert(cache, cache_size, cache_bits, pixel);
            }
        } else {
            size_t cache_index = symbol - 280u;
            if (!cache || cache_index >= cache_size) {
                rc = QW_STATUS_ERR_FORMAT;
                goto done;
            }
            pixel = cache[cache_index];
            pixels[decoded++] = pixel;
            webpl_cache_insert(cache, cache_size, cache_bits, pixel);
        }
        if (reader->failed) {
            rc = QW_STATUS_ERR_FORMAT;
            goto done;
        }
    }
    rc = QW_STATUS_OK;
    *out_pixels = pixels;
    pixels = NULL;

done:
    free(pixels);
    free(entropy_image);
    free(cache);
    webpl_prefix_groups_clear(groups, group_count);
    return rc;
}

enum {
    WEBP_LOSSLESS_TRANSFORM_PREDICTOR = 0,
    WEBP_LOSSLESS_TRANSFORM_COLOR = 1,
    WEBP_LOSSLESS_TRANSFORM_SUBTRACT_GREEN = 2,
    WEBP_LOSSLESS_TRANSFORM_COLOR_INDEXING = 3
};

typedef struct WebPLTransform {
    uint32_t *data;
    uint32_t input_width;
    uint32_t input_height;
    uint32_t data_width;
    uint32_t data_height;
    uint32_t palette_size;
    uint8_t type;
    uint8_t size_bits;
    uint8_t width_bits;
} WebPLTransform;

static uint32_t webpl_ceil_shift(uint32_t value, unsigned shift)
{
    uint32_t block = 1u << shift;
    return (value >> shift) + ((value & (block - 1u)) != 0u);
}

static uint8_t webpl_component(uint32_t pixel, unsigned component)
{
    return (uint8_t)(pixel >> (24u - component * 8u));
}

static uint32_t webpl_pack_components(const uint8_t components[4])
{
    return ((uint32_t)components[0] << 24) |
           ((uint32_t)components[1] << 16) |
           ((uint32_t)components[2] << 8) |
           (uint32_t)components[3];
}

static uint32_t webpl_add_pixels(uint32_t left, uint32_t right)
{
    uint8_t components[4];
    for (unsigned i = 0u; i < 4u; ++i) {
        components[i] = (uint8_t)(webpl_component(left, i) +
                                  webpl_component(right, i));
    }
    return webpl_pack_components(components);
}

static uint32_t webpl_average_pixels(uint32_t left, uint32_t right)
{
    uint8_t components[4];
    for (unsigned i = 0u; i < 4u; ++i) {
        components[i] = (uint8_t)(((unsigned)webpl_component(left, i) +
                                   webpl_component(right, i)) / 2u);
    }
    return webpl_pack_components(components);
}

static int webpl_abs(int value)
{
    return value < 0 ? -value : value;
}

static uint32_t webpl_select_predictor(uint32_t left, uint32_t top,
                                       uint32_t top_left)
{
    int left_distance = 0;
    int top_distance = 0;
    for (unsigned component = 0u; component < 4u; ++component) {
        int l = webpl_component(left, component);
        int t = webpl_component(top, component);
        int tl = webpl_component(top_left, component);
        int prediction = l + t - tl;
        left_distance += webpl_abs(prediction - l);
        top_distance += webpl_abs(prediction - t);
    }
    return left_distance < top_distance ? left : top;
}

static uint8_t webpl_clamp_component(int value)
{
    if (value < 0) return 0u;
    if (value > 255) return 255u;
    return (uint8_t)value;
}

static uint32_t webpl_predict(uint32_t mode,
                              uint32_t left,
                              uint32_t top,
                              uint32_t top_left,
                              uint32_t top_right)
{
    uint8_t output[4];
    if (mode == 0u) return UINT32_C(0xff000000);
    if (mode == 1u) return left;
    if (mode == 2u) return top;
    if (mode == 3u) return top_right;
    if (mode == 4u) return top_left;
    if (mode == 11u) return webpl_select_predictor(left, top, top_left);
    for (unsigned component = 0u; component < 4u; ++component) {
        int l = webpl_component(left, component);
        int t = webpl_component(top, component);
        int tl = webpl_component(top_left, component);
        int tr = webpl_component(top_right, component);
        int value;
        switch (mode) {
        case 5u:
            value = webpl_component(
                webpl_average_pixels(webpl_average_pixels(left, top_right),
                                     top), component);
            break;
        case 6u:
            value = (l + tl) / 2;
            break;
        case 7u:
            value = (l + t) / 2;
            break;
        case 8u:
            value = (tl + t) / 2;
            break;
        case 9u:
            value = (t + tr) / 2;
            break;
        case 10u:
            value = webpl_component(
                webpl_average_pixels(webpl_average_pixels(left, top_left),
                    webpl_average_pixels(top, top_right)), component);
            break;
        case 12u:
            value = l + t - tl;
            break;
        case 13u: {
            int average = (l + t) / 2;
            value = average + (average - tl) / 2;
            break;
        }
        default:
            return 0u;
        }
        output[component] = webpl_clamp_component(value);
    }
    return webpl_pack_components(output);
}

static int webpl_apply_predictor(uint32_t *pixels,
                                 uint32_t width,
                                 uint32_t height,
                                 const WebPLTransform *transform)
{
    for (uint32_t y = 0u; y < height; ++y) {
        for (uint32_t x = 0u; x < width; ++x) {
            size_t index = (size_t)y * width + x;
            uint32_t mode = (transform->data[
                (size_t)(y >> transform->size_bits) * transform->data_width +
                (x >> transform->size_bits)] >> 8) & 0xffu;
            uint32_t prediction;
            uint32_t left;
            uint32_t top;
            uint32_t top_left;
            uint32_t top_right;
            if (mode > 13u) return QW_STATUS_ERR_FORMAT;
            if (x == 0u && y == 0u) {
                prediction = UINT32_C(0xff000000);
            } else if (y == 0u) {
                prediction = pixels[index - 1u];
            } else if (x == 0u) {
                prediction = pixels[index - width];
            } else {
                left = pixels[index - 1u];
                top = pixels[index - width];
                top_left = pixels[index - width - 1u];
                top_right = x + 1u < width
                    ? pixels[index - width + 1u]
                    : pixels[(size_t)y * width];
                prediction = webpl_predict(mode, left, top, top_left,
                                           top_right);
            }
            pixels[index] = webpl_add_pixels(pixels[index], prediction);
        }
    }
    return QW_STATUS_OK;
}

static int webpl_signed_byte(uint32_t value)
{
    int signed_value = (int)(value & 0xffu);
    return signed_value < 128 ? signed_value : signed_value - 256;
}

static int webpl_color_delta(uint32_t transform_value, unsigned component,
                             int color)
{
    int transform_byte;
    int product;
    int delta;
    if (component == 0u) {
        transform_byte = webpl_signed_byte(transform_value);
    } else if (component == 1u) {
        transform_byte = webpl_signed_byte(transform_value >> 8);
    } else {
        transform_byte = webpl_signed_byte(transform_value >> 16);
    }
    product = transform_byte * color;
    delta = product / 32;
    if (product < 0 && product % 32) --delta;
    return delta;
}

static void webpl_apply_color_transform(uint32_t *pixels,
                                        uint32_t width,
                                        uint32_t height,
                                        const WebPLTransform *transform)
{
    for (uint32_t y = 0u; y < height; ++y) {
        for (uint32_t x = 0u; x < width; ++x) {
            size_t block_index =
                (size_t)(y >> transform->size_bits) * transform->data_width +
                (x >> transform->size_bits);
            size_t pixel_index = (size_t)y * width + x;
            uint32_t value = pixels[pixel_index];
            uint32_t element = transform->data[block_index];
            int green = webpl_signed_byte(value >> 8);
            int red = (int)((value >> 16) & 0xffu);
            int blue = (int)(value & 0xffu);
            int new_red = red + webpl_color_delta(element, 0u, green);
            int new_blue = blue + webpl_color_delta(element, 1u, green);
            new_blue += webpl_color_delta(element, 2u,
                                          webpl_signed_byte((uint32_t)new_red));
            pixels[pixel_index] = (value & UINT32_C(0xff00ff00)) |
                (((uint32_t)new_red & 0xffu) << 16) |
                ((uint32_t)new_blue & 0xffu);
        }
    }
}

static void webpl_apply_subtract_green(uint32_t *pixels, size_t pixel_count)
{
    for (size_t i = 0u; i < pixel_count; ++i) {
        uint32_t value = pixels[i];
        uint32_t green = (value >> 8) & 0xffu;
        uint32_t red = (((value >> 16) & 0xffu) + green) & 0xffu;
        uint32_t blue = ((value & 0xffu) + green) & 0xffu;
        pixels[i] = (value & UINT32_C(0xff00ff00)) |
                    (red << 16) | blue;
    }
}

static uint32_t webpl_color_index(uint32_t encoded_pixel,
                                  const WebPLTransform *transform)
{
    uint32_t index = (encoded_pixel >> 8) & 0xffu;
    return index < transform->palette_size ? transform->data[index] : 0u;
}

static int webpl_apply_color_indexing(uint32_t **pixels,
                                      uint32_t *width,
                                      uint32_t height,
                                      const WebPLTransform *transform)
{
    uint32_t original_width = transform->input_width;
    uint32_t factor = 1u << transform->width_bits;
    size_t output_count;
    uint32_t *output;
    if (!pixels || !*pixels || !width || *width != webpl_ceil_shift(
            original_width, transform->width_bits) ||
        !qw_checked_size_mul((size_t)original_width, (size_t)height,
                             &output_count) ||
        output_count > SIZE_MAX / sizeof(*output)) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (!transform->width_bits) {
        size_t count = (size_t)*width * height;
        for (size_t i = 0u; i < count; ++i)
            (*pixels)[i] = webpl_color_index((*pixels)[i], transform);
        return QW_STATUS_OK;
    }
    output = (uint32_t *)malloc(output_count * sizeof(*output));
    if (!output) return QW_STATUS_ERR_OUT_OF_MEMORY;
    for (uint32_t y = 0u; y < height; ++y) {
        for (uint32_t x = 0u; x < *width; ++x) {
            uint32_t encoded = (*pixels)[(size_t)y * *width + x];
            uint32_t packed = (encoded >> 8) & 0xffu;
            uint32_t index_bits = 8u >> transform->width_bits;
            uint32_t mask = (1u << index_bits) - 1u;
            for (uint32_t i = 0u; i < factor; ++i) {
                uint32_t x_out = x * factor + i;
                if (x_out < original_width) {
                    uint32_t indexed = (encoded & UINT32_C(0xff0000ff)) |
                        (((packed >> (i * index_bits)) & mask) << 8);
                    output[(size_t)y * original_width + x_out] =
                        webpl_color_index(indexed, transform);
                }
            }
        }
    }
    free(*pixels);
    *pixels = output;
    *width = original_width;
    return QW_STATUS_OK;
}

static int webpl_decode_lossless_stream(WebPLBitReader *reader,
                                        uint32_t width,
                                        uint32_t height,
                                        uint32_t **out_pixels)
{
    WebPLTransform transforms[4] = {{0}};
    size_t transform_count = 0u;
    uint32_t current_width = width;
    uint32_t current_height = height;
    uint32_t *pixels = NULL;
    uint32_t seen_transform_mask = 0u;
    int rc = QW_STATUS_ERR_FORMAT;

    if (!reader || !out_pixels ||
        !qw_image_dimensions_within_limits(width, height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    *out_pixels = NULL;
    for (;;) {
        WebPLTransform *transform;
        uint32_t has_transform = webpl_read_bits(reader, 1u);
        uint32_t type;
        uint32_t shift;
        if (reader->failed) {
            rc = QW_STATUS_ERR_FORMAT;
            goto done;
        }
        if (!has_transform) break;
        rc = QW_STATUS_ERR_FORMAT;
        type = webpl_read_bits(reader, 2u);
        if (reader->failed || transform_count >= 4u) goto done;
        if (seen_transform_mask & (1u << type)) goto done;
        seen_transform_mask |= 1u << type;
        transform = &transforms[transform_count];
        transform->type = (uint8_t)type;
        transform->input_width = current_width;
        transform->input_height = current_height;
        if (type == WEBP_LOSSLESS_TRANSFORM_PREDICTOR ||
            type == WEBP_LOSSLESS_TRANSFORM_COLOR) {
            transform->size_bits = (uint8_t)(2u + webpl_read_bits(reader, 3u));
            if (reader->failed) goto done;
            shift = 1u << transform->size_bits;
            transform->data_width = webpl_ceil_shift(current_width,
                                                      transform->size_bits);
            transform->data_height = webpl_ceil_shift(current_height,
                                                       transform->size_bits);
            rc = webpl_decode_image_data(reader, transform->data_width,
                                         transform->data_height, 0,
                                         &transform->data);
            if (rc != QW_STATUS_OK) goto done;
            (void)shift;
        } else if (type == WEBP_LOSSLESS_TRANSFORM_COLOR_INDEXING) {
            uint32_t table_size = 1u + webpl_read_bits(reader, 8u);
            if (reader->failed) goto done;
            transform->palette_size = table_size;
            transform->data_width = table_size;
            transform->data_height = 1u;
            rc = webpl_decode_image_data(reader, table_size, 1u, 0,
                                         &transform->data);
            if (rc != QW_STATUS_OK) goto done;
            for (uint32_t i = 1u; i < table_size; ++i) {
                transform->data[i] = webpl_add_pixels(
                    transform->data[i - 1u], transform->data[i]);
            }
            transform->width_bits = table_size <= 2u ? 3u
                : table_size <= 4u ? 2u
                : table_size <= 16u ? 1u : 0u;
            current_width = webpl_ceil_shift(current_width,
                                               transform->width_bits);
        }
        ++transform_count;
    }
    if (reader->failed) {
        rc = QW_STATUS_ERR_FORMAT;
        goto done;
    }
    rc = webpl_decode_image_data(reader, current_width, current_height, 1,
                                 &pixels);
    if (rc != QW_STATUS_OK) goto done;
    for (size_t reverse = transform_count; reverse > 0u; --reverse) {
        WebPLTransform *transform = &transforms[reverse - 1u];
        size_t count;
        if (transform->type == WEBP_LOSSLESS_TRANSFORM_PREDICTOR) {
            if (current_width != transform->input_width ||
                current_height != transform->input_height) {
                rc = QW_STATUS_ERR_FORMAT;
                goto done;
            }
            rc = webpl_apply_predictor(pixels, current_width, current_height,
                                       transform);
            if (rc != QW_STATUS_OK) goto done;
        } else if (transform->type == WEBP_LOSSLESS_TRANSFORM_COLOR) {
            if (current_width != transform->input_width ||
                current_height != transform->input_height) {
                rc = QW_STATUS_ERR_FORMAT;
                goto done;
            }
            webpl_apply_color_transform(pixels, current_width, current_height,
                                        transform);
        } else if (transform->type == WEBP_LOSSLESS_TRANSFORM_SUBTRACT_GREEN) {
            if (!qw_checked_size_mul((size_t)current_width,
                                     (size_t)current_height, &count)) {
                rc = QW_STATUS_ERR_FORMAT;
                goto done;
            }
            webpl_apply_subtract_green(pixels, count);
        } else if (transform->type == WEBP_LOSSLESS_TRANSFORM_COLOR_INDEXING) {
            rc = webpl_apply_color_indexing(&pixels, &current_width,
                                            current_height, transform);
            if (rc != QW_STATUS_OK) goto done;
        }
    }
    if (current_width != width || current_height != height) {
        rc = QW_STATUS_ERR_FORMAT;
        goto done;
    }
    *out_pixels = pixels;
    pixels = NULL;
    rc = QW_STATUS_OK;

done:
    free(pixels);
    for (size_t i = 0u; i < 4u; ++i)
        free(transforms[i].data);
    return rc;
}

static int webpl_decode_lossless_stream_bytes(const uint8_t *data,
                                               size_t size,
                                               uint32_t width,
                                               uint32_t height,
                                               uint32_t **out_pixels)
{
    WebPLBitReader reader;
    if (!data || !size || !out_pixels) return QW_STATUS_ERR_FORMAT;
    reader.data = data;
    reader.size = size;
    reader.byte_position = 0u;
    reader.bit_position = 0u;
    reader.failed = 0;
    return webpl_decode_lossless_stream(&reader, width, height, out_pixels);
}

static int webpl_decode_alpha(const uint8_t *data,
                              size_t size,
                              uint32_t width,
                              uint32_t height,
                              uint8_t **out_alpha)
{
    uint32_t *decoded_argb = NULL;
    uint8_t *alpha = NULL;
    size_t pixel_count;
    uint8_t flags;
    uint8_t compression;
    uint8_t filter;
    uint8_t preprocessing;
    int rc = QW_STATUS_ERR_FORMAT;

    if (!data || !out_alpha || size < 1u) return QW_STATUS_ERR_FORMAT;
    *out_alpha = NULL;
    if (!qw_image_dimensions_within_limits(width, height) ||
        !qw_checked_size_mul((size_t)width, (size_t)height, &pixel_count)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    flags = data[0];
    compression = flags & 3u;
    filter = (flags >> 2) & 3u;
    preprocessing = (flags >> 4) & 3u;
    if ((flags & 0xc0u) != 0u || compression > 1u || preprocessing > 1u)
        return QW_STATUS_ERR_FORMAT;
    alpha = (uint8_t *)malloc(pixel_count ? pixel_count : 1u);
    if (!alpha) return QW_STATUS_ERR_OUT_OF_MEMORY;
    if (compression == 0u) {
        if (size - 1u != pixel_count) goto done;
        memcpy(alpha, data + 1u, pixel_count);
    } else {
        if (size <= 1u) goto done;
        rc = webpl_decode_lossless_stream_bytes(data + 1u, size - 1u,
                                                 width, height,
                                                 &decoded_argb);
        if (rc != QW_STATUS_OK) goto done;
        for (size_t i = 0u; i < pixel_count; ++i)
            alpha[i] = (uint8_t)(decoded_argb[i] >> 8);
    }
    if (filter) {
        for (uint32_t y = 0u; y < height; ++y) {
            for (uint32_t x = 0u; x < width; ++x) {
                size_t index = (size_t)y * width + x;
                int left = x ? alpha[index - 1u] : 0;
                int top = y ? alpha[index - width] : 0;
                int top_left = x && y ? alpha[index - width - 1u] : 0;
                int prediction;
                if (!x && !y) {
                    prediction = 0;
                } else if (!x) {
                    prediction = top;
                } else if (!y) {
                    prediction = left;
                } else if (filter == 1u) {
                    prediction = left;
                } else if (filter == 2u) {
                    prediction = top;
                } else if (filter == 3u) {
                    prediction = left + top - top_left;
                    if (prediction < 0) prediction = 0;
                    if (prediction > 255) prediction = 255;
                } else {
                    prediction = 0;
                }
                alpha[index] = (uint8_t)(alpha[index] + prediction);
            }
        }
    }
    *out_alpha = alpha;
    alpha = NULL;
    rc = QW_STATUS_OK;

done:
    free(alpha);
    free(decoded_argb);
    return rc;
}

int qw_webp_vp8l_decode(const uint8_t *data,
                       size_t size,
                       uint32_t expected_width,
                       uint32_t expected_height,
                       uint8_t **out_rgba)
{
    WebPLBitReader reader;
    uint32_t width;
    uint32_t height;
    uint32_t has_alpha_hint;
    uint32_t version;
    uint32_t *argb = NULL;
    uint8_t *rgba = NULL;
    size_t pixel_count;
    size_t rgba_size;
    int rc;

    if (!out_rgba) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_rgba = NULL;
    if (!data || size < 5u || data[0] != 0x2fu)
        return QW_STATUS_ERR_FORMAT;
    reader.data = data + 1u;
    reader.size = size - 1u;
    reader.byte_position = 0u;
    reader.bit_position = 0u;
    reader.failed = 0;
    width = webpl_read_bits(&reader, 14u) + 1u;
    height = webpl_read_bits(&reader, 14u) + 1u;
    has_alpha_hint = webpl_read_bits(&reader, 1u);
    version = webpl_read_bits(&reader, 3u);
    (void)has_alpha_hint;
    if (reader.failed || version != 0u || width != expected_width ||
        height != expected_height) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (!qw_image_dimensions_within_limits(width, height) ||
        !qw_checked_size_mul((size_t)width, (size_t)height, &pixel_count) ||
        pixel_count > SIZE_MAX / 4u ||
        !qw_checked_size_mul(pixel_count, 4u, &rgba_size)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    rc = webpl_decode_lossless_stream(&reader, width, height, &argb);
    if (rc != QW_STATUS_OK) return rc;
    rgba = (uint8_t *)malloc(rgba_size ? rgba_size : 1u);
    if (!rgba) {
        free(argb);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0u; i < pixel_count; ++i) {
        uint32_t pixel = argb[i];
        rgba[i * 4u] = (uint8_t)(pixel >> 16);
        rgba[i * 4u + 1u] = (uint8_t)(pixel >> 8);
        rgba[i * 4u + 2u] = (uint8_t)pixel;
        rgba[i * 4u + 3u] = (uint8_t)(pixel >> 24);
    }
    free(argb);
    *out_rgba = rgba;
    return QW_STATUS_OK;
}

int qw_webp_alpha_decode(const uint8_t *data,
                         size_t size,
                         uint32_t width,
                         uint32_t height,
                         uint8_t **out_alpha)
{
    return webpl_decode_alpha(data, size, width, height, out_alpha);
}
