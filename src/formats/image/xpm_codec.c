#include "image_internal.h"

#include "quirewise/status.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define XPM_MAX_PIXELS 20000000u
#define XPM_MAX_COLORS 65536u
#define XPM_MAX_CPP 8u

typedef struct QwXpmString {
    uint8_t *data;
    size_t size;
} QwXpmString;

typedef struct QwXpmColor {
    uint8_t key[XPM_MAX_CPP];
    uint8_t rgb[3];
    int used;
} QwXpmColor;

typedef struct QwXpmHeader {
    uint32_t width;
    uint32_t height;
    size_t color_count;
    size_t chars_per_pixel;
    size_t pixel_count;
} QwXpmHeader;

static int xpm_find_marker(const uint8_t *data, size_t size, size_t *position)
{
    static const uint8_t marker[] = "/* XPM */";
    if (!data || !position || size < sizeof(marker) - 1u) return 0;
    for (size_t i = 0u; i <= size - (sizeof(marker) - 1u); ++i) {
        if (memcmp(data + i, marker, sizeof(marker) - 1u) == 0) {
            *position = i + sizeof(marker) - 1u;
            return 1;
        }
    }
    return 0;
}

static int xpm_append_byte(uint8_t **buffer,
                           size_t *length,
                           size_t *capacity,
                           uint8_t value,
                           size_t maximum)
{
    uint8_t *grown;
    size_t next;
    if (*length >= maximum) return 0;
    if (*length == *capacity) {
        next = *capacity ? *capacity * 2u : 64u;
        if (next < *capacity || next > maximum) next = maximum;
        if (next <= *capacity) return 0;
        grown = (uint8_t *)realloc(*buffer, next);
        if (!grown) return 0;
        *buffer = grown;
        *capacity = next;
    }
    (*buffer)[(*length)++] = value;
    return 1;
}

static int xpm_skip_char_literal(const uint8_t *data,
                                 size_t size,
                                 size_t *position)
{
    ++*position;
    while (*position < size) {
        if (data[*position] == '\\') {
            *position += (*position + 1u < size) ? 2u : 1u;
        } else if (data[(*position)++] == '\'') {
            return 1;
        }
    }
    return 0;
}

static int xpm_next_string(const uint8_t *data,
                           size_t size,
                           size_t *position,
                           QwXpmString *out_string)
{
    uint8_t *buffer = NULL;
    size_t length = 0u;
    size_t capacity = 0u;
    int closed = 0;

    if (!data || !position || !out_string) return 0;
    memset(out_string, 0, sizeof(*out_string));
    while (*position < size) {
        uint8_t value = data[*position];
        if (qw_ascii_isspace(value)) {
            ++*position;
        } else if (value == '/' && *position + 1u < size &&
                   data[*position + 1u] == '/') {
            *position += 2u;
            while (*position < size && data[*position] != '\n' &&
                   data[*position] != '\r') ++*position;
        } else if (value == '/' && *position + 1u < size &&
                   data[*position + 1u] == '*') {
            *position += 2u;
            while (*position + 1u < size &&
                   !(data[*position] == '*' && data[*position + 1u] == '/')) {
                ++*position;
            }
            if (*position + 1u >= size) return 0;
            *position += 2u;
        } else if (value == '\'') {
            if (!xpm_skip_char_literal(data, size, position)) return 0;
        } else if (value == '"') {
            ++*position;
            buffer = NULL;
            length = 0u;
            capacity = 0u;
            while (*position < size) {
                value = data[(*position)++];
                if (value == '"') {
                    closed = 1;
                    break;
                }
                if (value == '\n' || value == '\r') goto failed;
                if (value == '\\') {
                    unsigned int parsed = 0u;
                    if (*position >= size) goto failed;
                    value = data[(*position)++];
                    switch (value) {
                    case '\\': case '"': case '\'': case '?':
                        break;
                    case 'a': value = '\a'; break;
                    case 'b': value = '\b'; break;
                    case 'f': value = '\f'; break;
                    case 'n': value = '\n'; break;
                    case 'r': value = '\r'; break;
                    case 't': value = '\t'; break;
                    case 'v': value = '\v'; break;
                    default:
                        if (value >= '0' && value <= '7') {
                            parsed = value - '0';
                            for (unsigned int digits = 1u;
                                 digits < 3u && *position < size &&
                                 data[*position] >= '0' &&
                                 data[*position] <= '7'; ++digits) {
                                parsed = parsed * 8u +
                                         (data[(*position)++] - '0');
                            }
                            if (parsed > 255u) goto failed;
                            value = (uint8_t)parsed;
                        } else if (value == 'x') {
                            unsigned int digits = 0u;
                            parsed = 0u;
                            while (*position < size) {
                                uint8_t hex = data[*position];
                                unsigned int digit;
                                if (hex >= '0' && hex <= '9') digit = hex - '0';
                                else if (hex >= 'a' && hex <= 'f') digit = hex - 'a' + 10u;
                                else if (hex >= 'A' && hex <= 'F') digit = hex - 'A' + 10u;
                                else break;
                                if (parsed > (255u - digit) / 16u) goto failed;
                                parsed = parsed * 16u + digit;
                                ++digits;
                                ++*position;
                            }
                            if (!digits) goto failed;
                            value = (uint8_t)parsed;
                        } else {
                            goto failed;
                        }
                        break;
                    }
                }
                if (!value || !xpm_append_byte(&buffer, &length, &capacity,
                                                value, size)) goto failed;
            }
            if (!closed || !length) goto failed;
            out_string->data = buffer;
            out_string->size = length;
            return 1;
        } else {
            ++*position;
        }
    }
    return 0;

failed:
    free(buffer);
    return 0;
}

static int xpm_next_number(const uint8_t *data,
                           size_t size,
                           size_t *position,
                           uint32_t *out_number)
{
    uint64_t value = 0u;
    size_t digits = 0u;
    while (*position < size && qw_ascii_isspace(data[*position])) ++*position;
    while (*position < size && data[*position] >= '0' &&
           data[*position] <= '9') {
        value = value * 10u + (data[(*position)++] - '0');
        if (value > UINT32_MAX) return 0;
        ++digits;
    }
    if (!digits) return 0;
    *out_number = (uint32_t)value;
    return 1;
}

static int xpm_parse_header_string(const QwXpmString *string,
                                  QwXpmHeader *header)
{
    size_t position = 0u;
    uint32_t colors;
    uint32_t cpp;
    if (!string || !header ||
        !xpm_next_number(string->data, string->size, &position,
                         &header->width) ||
        !xpm_next_number(string->data, string->size, &position,
                         &header->height) ||
        !xpm_next_number(string->data, string->size, &position, &colors) ||
        !xpm_next_number(string->data, string->size, &position, &cpp)) {
        return 0;
    }
    while (position < string->size && qw_ascii_isspace(string->data[position])) {
        ++position;
    }
    if (position != string->size || !header->width || !header->height ||
        !colors || colors > XPM_MAX_COLORS || !cpp || cpp > XPM_MAX_CPP ||
        (size_t)header->width > SIZE_MAX / header->height) {
        return 0;
    }
    header->color_count = colors;
    header->chars_per_pixel = cpp;
    header->pixel_count = (size_t)header->width * header->height;
    return header->pixel_count && header->pixel_count <= XPM_MAX_PIXELS;
}

static int xpm_read_header(const void *data,
                           size_t size,
                           size_t *out_position,
                           QwXpmHeader *out_header)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwXpmString string;
    size_t position;
    int ok;
    if (!bytes || !out_position || !out_header ||
        !xpm_find_marker(bytes, size, &position) ||
        !xpm_next_string(bytes, size, &position, &string)) {
        return 0;
    }
    ok = xpm_parse_header_string(&string, out_header);
    free(string.data);
    if (!ok) return 0;
    *out_position = position;
    return 1;
}

int qw_xpm_probe(const void *data, size_t size)
{
    QwXpmHeader header;
    size_t position;
    return xpm_read_header(data, size, &position, &header);
}

static uint32_t xpm_hash_key(const uint8_t *key, size_t size)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0u; i < size; ++i) {
        hash = (hash ^ key[i]) * UINT32_C(16777619);
    }
    return hash;
}

static int xpm_parse_color_value(const uint8_t *value,
                                 size_t length,
                                 uint8_t *out_rgb)
{
    if (length == 4u &&
        (value[0] == 'N' || value[0] == 'n') &&
        (value[1] == 'O' || value[1] == 'o') &&
        (value[2] == 'N' || value[2] == 'n') &&
        (value[3] == 'E' || value[3] == 'e')) {
        out_rgb[0] = out_rgb[1] = out_rgb[2] = 255u;
        return 1;
    }
    if (length < 4u || value[0] != '#' || (length - 1u) % 3u != 0u) {
        return 0;
    }
    {
        size_t digits_per_channel = (length - 1u) / 3u;
        if (!digits_per_channel || digits_per_channel > 6u) return 0;
        for (size_t channel = 0u; channel < 3u; ++channel) {
            uint32_t component = 0u;
            uint32_t maximum = 0u;
            size_t start = 1u + channel * digits_per_channel;
            for (size_t digit_index = 0u; digit_index < digits_per_channel;
                 ++digit_index) {
                uint8_t digit_char = value[start + digit_index];
                uint32_t digit;
                if (digit_char >= '0' && digit_char <= '9') digit = digit_char - '0';
                else if (digit_char >= 'a' && digit_char <= 'f') digit = digit_char - 'a' + 10u;
                else if (digit_char >= 'A' && digit_char <= 'F') digit = digit_char - 'A' + 10u;
                else return 0;
                component = component * 16u + digit;
                maximum = maximum * 16u + 15u;
            }
            out_rgb[channel] = (uint8_t)(((uint64_t)component * 255u +
                                          maximum / 2u) / maximum);
        }
    }
    return 1;
}

static size_t xpm_hash_slot(const uint8_t *key,
                            size_t key_size,
                            size_t capacity)
{
    return (size_t)xpm_hash_key(key, key_size) & (capacity - 1u);
}

static int xpm_add_color(QwXpmColor *colors,
                         size_t capacity,
                         size_t cpp,
                         const QwXpmString *line)
{
    static const struct {
        const char *name;
        unsigned int rank;
    } fields[] = { {"c", 4u}, {"g", 3u}, {"g4", 2u}, {"m", 1u} };
    uint8_t rgb[3];
    const uint8_t *selected = NULL;
    size_t selected_size = 0u;
    unsigned int selected_rank = 0u;
    size_t position = cpp;

    if (!line || line->size <= cpp) return 0;
    while (position < line->size) {
        size_t field_start;
        size_t field_size;
        size_t value_start;
        size_t value_size;
        unsigned int rank = 0u;
        while (position < line->size && qw_ascii_isspace(line->data[position])) ++position;
        if (position >= line->size) break;
        field_start = position;
        while (position < line->size && !qw_ascii_isspace(line->data[position])) ++position;
        field_size = position - field_start;
        while (position < line->size && qw_ascii_isspace(line->data[position])) ++position;
        value_start = position;
        while (position < line->size && !qw_ascii_isspace(line->data[position])) ++position;
        value_size = position - value_start;
        for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
            size_t name_size = strlen(fields[i].name);
            if (field_size == name_size &&
                memcmp(line->data + field_start, fields[i].name, name_size) == 0) {
                rank = fields[i].rank;
                break;
            }
        }
        if (rank && value_size && rank > selected_rank &&
            xpm_parse_color_value(line->data + value_start, value_size, rgb)) {
            selected = line->data + value_start;
            selected_size = value_size;
            selected_rank = rank;
        }
    }
    if (!selected || !xpm_parse_color_value(selected, selected_size, rgb)) return 0;
    {
        size_t slot = xpm_hash_slot(line->data, cpp, capacity);
        for (size_t attempts = 0u; attempts < capacity; ++attempts) {
            QwXpmColor *entry = &colors[slot];
            if (!entry->used) {
                memcpy(entry->key, line->data, cpp);
                memcpy(entry->rgb, rgb, sizeof(rgb));
                entry->used = 1;
                return 1;
            }
            if (memcmp(entry->key, line->data, cpp) == 0) return 0;
            slot = (slot + 1u) & (capacity - 1u);
        }
    }
    return 0;
}

static const QwXpmColor *xpm_find_color(const QwXpmColor *colors,
                                       size_t capacity,
                                       size_t cpp,
                                       const uint8_t *key)
{
    size_t slot = xpm_hash_slot(key, cpp, capacity);
    for (size_t attempts = 0u; attempts < capacity; ++attempts) {
        const QwXpmColor *entry = &colors[slot];
        if (!entry->used) return NULL;
        if (memcmp(entry->key, key, cpp) == 0) return entry;
        slot = (slot + 1u) & (capacity - 1u);
    }
    return NULL;
}

int qw_xpm_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwXpmHeader header;
    QwXpmColor *colors = NULL;
    uint8_t *pixels = NULL;
    size_t position;
    size_t capacity = 1u;
    size_t output_size;
    int rc = QW_STATUS_ERR_FORMAT;

    if (!out_page || !xpm_read_header(data, size, &position, &header)) {
        return QW_STATUS_ERR_FORMAT;
    }
    memset(out_page, 0, sizeof(*out_page));
    if (header.color_count > SIZE_MAX / 2u) return QW_STATUS_ERR_FORMAT;
    while (capacity < header.color_count * 2u) {
        if (capacity > SIZE_MAX / 2u) return QW_STATUS_ERR_FORMAT;
        capacity *= 2u;
    }
    if (capacity > SIZE_MAX / sizeof(*colors) ||
        header.pixel_count > SIZE_MAX / 3u) {
        return QW_STATUS_ERR_FORMAT;
    }
    colors = (QwXpmColor *)calloc(capacity, sizeof(*colors));
    output_size = header.pixel_count * 3u;
    pixels = (uint8_t *)malloc(output_size ? output_size : 1u);
    if (!colors || !pixels) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (size_t i = 0u; i < header.color_count; ++i) {
        QwXpmString line;
        if (!xpm_next_string(bytes, size, &position, &line)) goto cleanup;
        if (!xpm_add_color(colors, capacity, header.chars_per_pixel, &line)) {
            free(line.data);
            goto cleanup;
        }
        free(line.data);
    }
    for (uint32_t y = 0u; y < header.height; ++y) {
        QwXpmString line;
        size_t expected_line = (size_t)header.width * header.chars_per_pixel;
        if (!xpm_next_string(bytes, size, &position, &line)) goto cleanup;
        if (line.size != expected_line) {
            free(line.data);
            goto cleanup;
        }
        for (uint32_t x = 0u; x < header.width; ++x) {
            const QwXpmColor *color = xpm_find_color(
                colors, capacity, header.chars_per_pixel,
                line.data + (size_t)x * header.chars_per_pixel);
            size_t output = ((size_t)y * header.width + x) * 3u;
            if (!color) {
                free(line.data);
                goto cleanup;
            }
            memcpy(pixels + output, color->rgb, 3u);
        }
        free(line.data);
    }
    free(colors);
    out_page->data = pixels;
    out_page->size = output_size;
    out_page->kind = QW_PAGE_IMAGE_RGB24;
    out_page->width = header.width;
    out_page->height = header.height;
    return QW_STATUS_OK;

cleanup:
    free(colors);
    free(pixels);
    return rc;
}
