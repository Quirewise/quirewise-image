#include "image_internal.h"
#include "../../core/byte_utils.h"

#include "quirewise/status.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum XbmTokenKind {
    XBM_TOKEN_END,
    XBM_TOKEN_IDENTIFIER,
    XBM_TOKEN_NUMBER,
    XBM_TOKEN_PUNCTUATION,
    XBM_TOKEN_OTHER,
    XBM_TOKEN_INVALID
};

typedef struct XbmScanner {
    const uint8_t *data;
    size_t size;
    size_t position;
    int invalid;
} XbmScanner;

typedef struct XbmToken {
    const uint8_t *text;
    size_t length;
    enum XbmTokenKind kind;
    uint8_t punctuation;
} XbmToken;

typedef struct XbmDimensions {
    const uint8_t *prefix;
    size_t prefix_length;
    uint32_t width;
    uint32_t height;
    int have_width;
    int have_height;
} XbmDimensions;



static int xbm_is_identifier_start(uint8_t ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

static int xbm_is_identifier_continue(uint8_t ch)
{
    return xbm_is_identifier_start(ch) || (ch >= '0' && ch <= '9');
}

static int xbm_skip_space_and_comments(XbmScanner *scanner)
{
    while (scanner->position < scanner->size) {
        uint8_t ch = scanner->data[scanner->position];
        if (qw_ascii_isspace(ch)) {
            ++scanner->position;
            continue;
        }
        if (scanner->position + 1u < scanner->size && ch == '/' &&
            scanner->data[scanner->position + 1u] == '/') {
            scanner->position += 2u;
            while (scanner->position < scanner->size &&
                   scanner->data[scanner->position] != '\n') {
                ++scanner->position;
            }
            continue;
        }
        if (scanner->position + 1u < scanner->size && ch == '/' &&
            scanner->data[scanner->position + 1u] == '*') {
            scanner->position += 2u;
            while (scanner->position + 1u < scanner->size &&
                   !(scanner->data[scanner->position] == '*' &&
                     scanner->data[scanner->position + 1u] == '/')) {
                ++scanner->position;
            }
            if (scanner->position + 1u >= scanner->size) {
                scanner->invalid = 1;
                return 0;
            }
            scanner->position += 2u;
            continue;
        }
        break;
    }
    return !scanner->invalid;
}

static XbmToken xbm_next_token(XbmScanner *scanner)
{
    XbmToken token;
    memset(&token, 0, sizeof(token));
    if (!xbm_skip_space_and_comments(scanner)) {
        token.kind = XBM_TOKEN_INVALID;
        return token;
    }
    if (scanner->position == scanner->size) {
        token.kind = XBM_TOKEN_END;
        return token;
    }
    if (scanner->position == 0u && scanner->size >= 3u &&
        scanner->data[0] == 0xEFu && scanner->data[1] == 0xBBu &&
        scanner->data[2] == 0xBFu) {
        scanner->position = 3u;
        return xbm_next_token(scanner);
    }

    token.text = scanner->data + scanner->position;
    if (xbm_is_identifier_start(scanner->data[scanner->position])) {
        token.kind = XBM_TOKEN_IDENTIFIER;
        ++scanner->position;
        while (scanner->position < scanner->size &&
               xbm_is_identifier_continue(scanner->data[scanner->position])) {
            ++scanner->position;
        }
    } else if (scanner->data[scanner->position] >= '0' &&
               scanner->data[scanner->position] <= '9') {
        token.kind = XBM_TOKEN_NUMBER;
        ++scanner->position;
        while (scanner->position < scanner->size) {
            uint8_t ch = scanner->data[scanner->position];
            if (!xbm_is_identifier_continue(ch)) {
                break;
            }
            ++scanner->position;
        }
    } else if (scanner->data[scanner->position] == '"' ||
               scanner->data[scanner->position] == '\'') {
        uint8_t quote = scanner->data[scanner->position++];
        token.kind = XBM_TOKEN_OTHER;
        while (scanner->position < scanner->size) {
            uint8_t ch = scanner->data[scanner->position++];
            if (ch == '\\') {
                if (scanner->position == scanner->size) {
                    scanner->invalid = 1;
                    token.kind = XBM_TOKEN_INVALID;
                    return token;
                }
                ++scanner->position;
            } else if (ch == quote) {
                token.length = scanner->position - (size_t)(token.text - scanner->data);
                return token;
            }
        }
        scanner->invalid = 1;
        token.kind = XBM_TOKEN_INVALID;
        return token;
    } else {
        token.kind = XBM_TOKEN_PUNCTUATION;
        token.punctuation = scanner->data[scanner->position++];
    }
    token.length = scanner->position - (size_t)(token.text - scanner->data);
    return token;
}

static int xbm_token_is(const XbmToken *token, const char *text)
{
    size_t length = strlen(text);
    return token->kind == XBM_TOKEN_IDENTIFIER && token->length == length &&
           memcmp(token->text, text, length) == 0;
}

static int xbm_token_punctuation(const XbmToken *token, uint8_t punctuation)
{
    return token->kind == XBM_TOKEN_PUNCTUATION &&
           token->punctuation == punctuation;
}

static int xbm_token_integer(const XbmToken *token, uint64_t *out_value)
{
    size_t position = 0u;
    unsigned base = 10u;
    uint64_t value = 0u;
    size_t digits = 0u;
    if (!token || token->kind != XBM_TOKEN_NUMBER || !out_value) {
        return 0;
    }
    if (token->length >= 2u && token->text[0] == '0' &&
        (token->text[1] == 'x' || token->text[1] == 'X')) {
        base = 16u;
        position = 2u;
    } else if (token->length > 1u && token->text[0] == '0') {
        base = 8u;
        position = 1u;
        value = 0u;
        digits = 1u;
    }
    for (; position < token->length; ++position) {
        uint8_t ch = token->text[position];
        unsigned digit;
        if (ch >= '0' && ch <= '9') {
            digit = (unsigned)(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = 10u + (unsigned)(ch - 'a');
        } else if (ch >= 'A' && ch <= 'F') {
            digit = 10u + (unsigned)(ch - 'A');
        } else {
            break;
        }
        if (digit >= base || value > (UINT64_MAX - digit) / base) {
            return 0;
        }
        value = value * base + digit;
        ++digits;
    }
    if (!digits) {
        return 0;
    }
    for (; position < token->length; ++position) {
        uint8_t suffix = token->text[position];
        if (suffix != 'u' && suffix != 'U' && suffix != 'l' && suffix != 'L') {
            return 0;
        }
    }
    *out_value = value;
    return 1;
}

static int xbm_read_integer(XbmScanner *scanner, int64_t *out_value)
{
    XbmToken token = xbm_next_token(scanner);
    int negative = 0;
    uint64_t value;
    if (xbm_token_punctuation(&token, '-') || xbm_token_punctuation(&token, '+')) {
        negative = xbm_token_punctuation(&token, '-');
        token = xbm_next_token(scanner);
    }
    if (!xbm_token_integer(&token, &value) || value > (uint64_t)INT64_MAX) {
        return 0;
    }
    *out_value = negative ? -(int64_t)value : (int64_t)value;
    return 1;
}

static int xbm_dimension_macro(const XbmToken *name,
                               const char *suffix,
                               const uint8_t **out_prefix,
                               size_t *out_prefix_length)
{
    size_t suffix_length = strlen(suffix);
    if (name->kind != XBM_TOKEN_IDENTIFIER || name->length < suffix_length ||
        memcmp(name->text + name->length - suffix_length,
               suffix, suffix_length) != 0) {
        return 0;
    }
    *out_prefix = name->text;
    *out_prefix_length = name->length - suffix_length;
    return 1;
}

static int xbm_dimension_prefix_matches(const XbmDimensions *dimensions,
                                        const uint8_t *prefix,
                                        size_t prefix_length)
{
    return dimensions->prefix_length == prefix_length &&
           (!prefix_length || memcmp(dimensions->prefix, prefix, prefix_length) == 0);
}

static int xbm_set_dimension(XbmDimensions *dimensions,
                             int width,
                             const uint8_t *prefix,
                             size_t prefix_length,
                             uint64_t value)
{
    int *present = width ? &dimensions->have_width : &dimensions->have_height;
    uint32_t *destination = width ? &dimensions->width : &dimensions->height;
    if (!value || value > UINT32_MAX) {
        return 0;
    }
    if (*present) {
        return *destination == (uint32_t)value &&
               xbm_dimension_prefix_matches(dimensions, prefix, prefix_length);
    }
    if ((dimensions->have_width || dimensions->have_height) &&
        !xbm_dimension_prefix_matches(dimensions, prefix, prefix_length)) {
        return 0;
    }
    dimensions->prefix = prefix;
    dimensions->prefix_length = prefix_length;
    *destination = (uint32_t)value;
    *present = 1;
    return 1;
}

static int xbm_read_dimensions(const void *data,
                               size_t size,
                               XbmDimensions *out_dimensions)
{
    XbmScanner scanner;
    XbmToken token;
    memset(out_dimensions, 0, sizeof(*out_dimensions));
    scanner.data = (const uint8_t *)data;
    scanner.size = size;
    scanner.position = 0u;
    scanner.invalid = 0;
    while ((token = xbm_next_token(&scanner)).kind != XBM_TOKEN_END) {
        XbmToken define_token;
        XbmToken name_token;
        XbmToken value_token;
        const uint8_t *prefix = NULL;
        size_t prefix_length = 0u;
        uint64_t value;
        int is_width = 0;
        int is_height = 0;
        if (token.kind == XBM_TOKEN_INVALID) {
            return 0;
        }
        if (!xbm_token_punctuation(&token, '#')) {
            continue;
        }
        define_token = xbm_next_token(&scanner);
        if (!xbm_token_is(&define_token, "define")) {
            continue;
        }
        name_token = xbm_next_token(&scanner);
        is_width = xbm_dimension_macro(&name_token, "_width",
                                       &prefix, &prefix_length);
        if (!is_width && xbm_token_is(&name_token, "width")) {
            prefix = name_token.text;
            prefix_length = 0u;
            is_width = 1;
        }
        if (!is_width) {
            is_height = xbm_dimension_macro(&name_token, "_height",
                                            &prefix, &prefix_length);
            if (!is_height && xbm_token_is(&name_token, "height")) {
                prefix = name_token.text;
                prefix_length = 0u;
                is_height = 1;
            }
        }
        if (!is_width && !is_height) {
            continue;
        }
        value_token = xbm_next_token(&scanner);
        if (!xbm_token_integer(&value_token, &value) ||
            !xbm_set_dimension(out_dimensions, is_width, prefix,
                               prefix_length, value)) {
            return 0;
        }
    }
    return !scanner.invalid && out_dimensions->have_width &&
           out_dimensions->have_height;
}

static int xbm_array_name_matches(const XbmDimensions *dimensions,
                                  const XbmToken *name)
{
    const uint8_t *suffix;
    size_t suffix_length;
    size_t prefix_length;
    if (name->kind != XBM_TOKEN_IDENTIFIER) {
        return 0;
    }
    if (name->length >= 5u &&
        memcmp(name->text + name->length - 5u, "_bits", 5u) == 0) {
        suffix = name->text + name->length - 5u;
        suffix_length = 5u;
    } else if (name->length >= 7u &&
               memcmp(name->text + name->length - 7u, "_bitmap", 7u) == 0) {
        suffix = name->text + name->length - 7u;
        suffix_length = 7u;
    } else if (dimensions->prefix_length == 0u &&
               (xbm_token_is(name, "bits") || xbm_token_is(name, "bitmap"))) {
        return 1;
    } else {
        return 0;
    }
    prefix_length = (size_t)(suffix - name->text);
    return prefix_length == dimensions->prefix_length &&
           (!dimensions->prefix_length ||
            memcmp(name->text, dimensions->prefix, dimensions->prefix_length) == 0);
}

static int xbm_history_has_type(const XbmToken *history,
                                size_t history_count,
                                const char *type)
{
    size_t start = history_count > 6u ? history_count - 6u : 0u;
    for (size_t i = start; i < history_count; ++i) {
        if (xbm_token_is(&history[i], type)) {
            return 1;
        }
    }
    return 0;
}

static void xbm_history_push(XbmToken *history,
                             size_t *history_count,
                             const XbmToken *token)
{
    if (*history_count == 8u) {
        memmove(history, history + 1u, 7u * sizeof(*history));
        *history_count = 7u;
    }
    history[(*history_count)++] = *token;
}

static int xbm_parse_array(XbmScanner *scanner,
                           const XbmDimensions *dimensions,
                           unsigned unit_bits,
                           QwPageImage *out_page)
{
    XbmToken token = xbm_next_token(scanner);
    size_t row_units;
    size_t count;
    size_t pixel_count;
    size_t output_size;
    size_t padded_width;
    uint8_t *pixels = NULL;
    size_t declared_count = 0u;
    int has_declared_count = 0;
    if (!xbm_token_punctuation(&token, '[')) {
        return QW_STATUS_ERR_FORMAT;
    }
    token = xbm_next_token(scanner);
    if (!xbm_token_punctuation(&token, ']')) {
        uint64_t parsed_count;
        if (!xbm_token_integer(&token, &parsed_count)) {
            return QW_STATUS_ERR_FORMAT;
        }
        declared_count = (size_t)parsed_count;
        if ((uint64_t)declared_count != parsed_count) {
            return QW_STATUS_ERR_FORMAT;
        }
        has_declared_count = 1;
        token = xbm_next_token(scanner);
        if (!xbm_token_punctuation(&token, ']')) {
            return QW_STATUS_ERR_FORMAT;
        }
    }
    token = xbm_next_token(scanner);
    if (!xbm_token_punctuation(&token, '=')) {
        return QW_STATUS_ERR_FORMAT;
    }
    token = xbm_next_token(scanner);
    if (!xbm_token_punctuation(&token, '{')) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (!qw_checked_size_add((size_t)dimensions->width, unit_bits - 1u,
                          &padded_width)) {
        return QW_STATUS_ERR_FORMAT;
    }
    row_units = padded_width / unit_bits;
    if (!qw_checked_size_mul(row_units, dimensions->height, &count) ||
        count > scanner->size || (has_declared_count && declared_count != count) ||
        !qw_checked_size_mul(dimensions->width, dimensions->height,
                               &pixel_count)) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (out_page && !qw_image_dimensions_within_limits(
            dimensions->width, dimensions->height)) {
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    output_size = pixel_count;
    if (out_page) {
        pixels = (uint8_t *)malloc(output_size ? output_size : 1u);
        if (!pixels) {
            return QW_STATUS_ERR_OUT_OF_MEMORY;
        }
    }
    for (size_t i = 0u; i < count; ++i) {
        int64_t signed_value;
        uint64_t limit = unit_bits == 8u ? UINT8_MAX : UINT16_MAX;
        int64_t minimum = unit_bits == 8u ? INT8_MIN : INT16_MIN;
        XbmToken separator;
        if (!xbm_read_integer(scanner, &signed_value) ||
            signed_value < minimum || signed_value > (int64_t)limit) {
            free(pixels);
            return QW_STATUS_ERR_FORMAT;
        }
        if (pixels) {
            uint64_t value = (uint64_t)signed_value;
            size_t y = i / row_units;
            size_t unit = i % row_units;
            size_t first_x = unit * unit_bits;
            for (unsigned bit = 0u; bit < unit_bits; ++bit) {
                size_t x = first_x + bit;
                if (x < dimensions->width) {
                    pixels[y * dimensions->width + x] =
                        (value & (UINT64_C(1) << bit)) ? 0u : 255u;
                }
            }
        }
        separator = xbm_next_token(scanner);
        if (i + 1u < count) {
            if (!xbm_token_punctuation(&separator, ',')) {
                free(pixels);
                return QW_STATUS_ERR_FORMAT;
            }
        } else if (xbm_token_punctuation(&separator, ',')) {
            separator = xbm_next_token(scanner);
            if (!xbm_token_punctuation(&separator, '}')) {
                free(pixels);
                return QW_STATUS_ERR_FORMAT;
            }
        } else if (!xbm_token_punctuation(&separator, '}')) {
            free(pixels);
            return QW_STATUS_ERR_FORMAT;
        }
    }
    token = xbm_next_token(scanner);
    if (!xbm_token_punctuation(&token, ';')) {
        free(pixels);
        return QW_STATUS_ERR_FORMAT;
    }
    if (pixels) {
        out_page->data = pixels;
        out_page->size = output_size;
        out_page->kind = QW_PAGE_IMAGE_GRAY8;
        out_page->width = dimensions->width;
        out_page->height = dimensions->height;
    }
    return QW_STATUS_OK;
}

static int xbm_parse(const void *data, size_t size, QwPageImage *out_page)
{
    XbmDimensions dimensions;
    XbmScanner scanner;
    XbmToken token;
    XbmToken history[8];
    size_t history_count = 0u;
    if (out_page) {
        memset(out_page, 0, sizeof(*out_page));
    }
    if (!data || !size) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (!xbm_read_dimensions(data, size, &dimensions)) {
        return QW_STATUS_ERR_FORMAT;
    }
    scanner.data = (const uint8_t *)data;
    scanner.size = size;
    scanner.position = 0u;
    scanner.invalid = 0;
    while ((token = xbm_next_token(&scanner)).kind != XBM_TOKEN_END) {
        if (token.kind == XBM_TOKEN_INVALID) {
            return QW_STATUS_ERR_FORMAT;
        }
        if (xbm_array_name_matches(&dimensions, &token)) {
            unsigned unit_bits = xbm_history_has_type(history, history_count, "short")
                ? 16u
                : (xbm_history_has_type(history, history_count, "char") ? 8u : 0u);
            if (unit_bits) {
                return xbm_parse_array(&scanner, &dimensions, unit_bits, out_page);
            }
        }
        xbm_history_push(history, &history_count, &token);
    }
    return QW_STATUS_ERR_FORMAT;
}

int qw_xbm_probe(const void *data, size_t size)
{
    return xbm_parse(data, size, NULL) == QW_STATUS_OK;
}

int qw_xbm_decode_page(const void *data, size_t size, QwPageImage *out_page)
{
    if (!out_page) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    return xbm_parse(data, size, out_page);
}
