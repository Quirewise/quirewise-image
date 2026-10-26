#ifndef QUIREWISE_PLATFORM_UNICODE_H
#define QUIREWISE_PLATFORM_UNICODE_H

#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32)
#include <wchar.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Returns required code units/bytes including NUL; -1 invalid, -2 capacity, -3 overflow. */
ptrdiff_t qw_platform_utf16_to_utf8(const uint16_t *input,
                                    char *output,
                                    size_t output_capacity);
ptrdiff_t qw_platform_utf8_to_utf16(const char *input,
                                    uint16_t *output,
                                    size_t output_capacity);

#if defined(_WIN32)
/* Returned strings are malloc-owned; the caller releases them with free(). */
wchar_t *qw_platform_utf8_to_wide(const char *input);
char *qw_platform_wide_to_utf8(const wchar_t *input);
#endif

#ifdef __cplusplus
}
#endif

#endif
