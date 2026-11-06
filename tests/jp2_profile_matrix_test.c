#include <quirewise/image.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_JP2_FIXTURE_DIR
#define QUIREWISE_JP2_FIXTURE_DIR "tests/data/jp2"
#endif

typedef struct ReferenceCase {
    const char *encoded;
    const char *reference;
    uint32_t width;
    uint32_t height;
    int tolerance;
} ReferenceCase;

static int read_file(const char *name, uint8_t **out, size_t *out_size)
{
    char path[1024];
    FILE *file;
    long length;
    uint8_t *data;
    int written = snprintf(path, sizeof(path), "%s/%s", QUIREWISE_JP2_FIXTURE_DIR,
                           name);
    if (written < 0 || (size_t)written >= sizeof(path)) return 0;
    file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    data = (uint8_t *)malloc((size_t)length ? (size_t)length : 1u);
    if (!data || fread(data, 1u, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *out = data;
    *out_size = (size_t)length;
    return 1;
}

static int run_reference_case(const ReferenceCase *test)
{
    uint8_t *encoded = NULL;
    uint8_t *reference = NULL;
    size_t encoded_size = 0u;
    size_t reference_size = 0u;
    QwImage image = {0};
    size_t i;
    int status;
    int max_diff = 0;

    if (!read_file(test->encoded, &encoded, &encoded_size)) {
        fprintf(stderr, "missing fixture: %s\n", test->encoded);
        return 0;
    }
    if (!read_file(test->reference, &reference, &reference_size)) {
        fprintf(stderr, "missing reference: %s\n", test->reference);
        free(encoded);
        return 0;
    }
    status = qw_image_decode(encoded, encoded_size, &image);
    free(encoded);
    if (status != 0) {
        fprintf(stderr, "%s decode failed: %s\n", test->encoded,
                qw_image_status_string(status));
        free(reference);
        return 0;
    }
    if (image.width != test->width || image.height != test->height ||
        image.format != QW_IMAGE_PIXEL_GRAY8) {
        fprintf(stderr, "%s shape mismatch: %ux%u format=%d size=%zu\n",
                test->encoded, image.width, image.height, (int)image.format,
                image.size);
        qw_image_free(&image);
        free(reference);
        return 0;
    }
    if (image.size != reference_size) {
        fprintf(stderr, "%s size mismatch: %zu != %zu\n", test->encoded,
                image.size, reference_size);
        qw_image_free(&image);
        free(reference);
        return 0;
    }
    for (i = 0u; i < image.size; ++i) {
        int diff = (int)image.pixels[i] - (int)reference[i];
        if (diff < 0) diff = -diff;
        if (diff > max_diff) max_diff = diff;
    }
    qw_image_free(&image);
    free(reference);
    if (max_diff > test->tolerance) {
        fprintf(stderr, "%s tolerance exceeded: %d > %d\n", test->encoded,
                max_diff, test->tolerance);
        return 0;
    }
    printf("golden %s: %ux%u max-diff=%d (tolerance %d)\n", test->encoded,
           test->width, test->height, max_diff, test->tolerance);
    return 1;
}

static int run_decode_case(const char *name, uint32_t width, uint32_t height,
                           QwImagePixelFormat format)
{
    uint8_t *encoded = NULL;
    size_t encoded_size = 0u;
    QwImage image = {0};
    int status;
    if (!read_file(name, &encoded, &encoded_size)) {
        fprintf(stderr, "missing fixture: %s\n", name);
        return 0;
    }
    status = qw_image_decode(encoded, encoded_size, &image);
    free(encoded);
    if (status != 0) {
        fprintf(stderr, "%s decode failed: %s\n", name,
                qw_image_status_string(status));
        return 0;
    }
    if (image.width != width || image.height != height ||
        image.format != format) {
        fprintf(stderr, "%s shape mismatch: %ux%u format=%d\n", name,
                image.width, image.height, (int)image.format);
        qw_image_free(&image);
        return 0;
    }
    qw_image_free(&image);
    printf("decode %s: %ux%u ok\n", name, width, height);
    return 1;
}


static int run_rgb_reference_case(const char *encoded_name,
                                  const char *reference_name,
                                  uint32_t width, uint32_t height)
{
    uint8_t *encoded = NULL;
    uint8_t *reference = NULL;
    size_t encoded_size = 0u;
    size_t reference_size = 0u;
    QwImage image = {0};
    int status;
    if (!read_file(encoded_name, &encoded, &encoded_size)) {
        fprintf(stderr, "missing fixture: %s\n", encoded_name);
        return 0;
    }
    if (!read_file(reference_name, &reference, &reference_size)) {
        fprintf(stderr, "missing reference: %s\n", reference_name);
        free(encoded);
        return 0;
    }
    status = qw_image_decode(encoded, encoded_size, &image);
    free(encoded);
    if (status != 0) {
        fprintf(stderr, "%s decode failed: %s\n", encoded_name,
                qw_image_status_string(status));
        free(reference);
        return 0;
    }
    if (image.width != width || image.height != height ||
        image.format != QW_IMAGE_PIXEL_RGB24 ||
        image.size != reference_size ||
        memcmp(image.pixels, reference, reference_size) != 0) {
        fprintf(stderr, "%s RGB reference mismatch: %ux%u format=%d size=%zu/%zu\n",
                encoded_name, image.width, image.height, (int)image.format,
                image.size, reference_size);
        qw_image_free(&image);
        free(reference);
        return 0;
    }
    qw_image_free(&image);
    free(reference);
    printf("golden %s: %ux%u rgb24 byte-identical\n", encoded_name, width,
           height);
    return 1;
}

static int test_oversized_tiled_codestream(void)
{
    uint8_t *encoded = NULL;
    size_t encoded_size = 0u;
    size_t index;
    QwImage image = {0};
    int status;
    int patched = 0;
    if (!read_file("synthetic_gray8_16x16_tiles_8x8.j2k", &encoded,
                   &encoded_size))
        return 0;
    for (index = 0u; index + 42u < encoded_size; ++index) {
        if (encoded[index] == 0xffu && encoded[index + 1u] == 0x51u) {
            /* SIZ: Lsiz, Rsiz, Xsiz, Ysiz, XOsiz, YOsiz, XTsiz, YTsiz. */
            size_t xsiz = index + 6u;
            size_t ysiz = index + 10u;
            size_t xtsiz = index + 18u;
            size_t ytsiz = index + 22u;
            /* 200000x200000 pixels with 256x256 tiles: far beyond the
             * default tile/image limits but a structurally valid SIZ. */
            encoded[xsiz] = 0x00u;
            encoded[xsiz + 1u] = 0x03u;
            encoded[xsiz + 2u] = 0x0du;
            encoded[xsiz + 3u] = 0x40u;
            encoded[ysiz] = 0x00u;
            encoded[ysiz + 1u] = 0x03u;
            encoded[ysiz + 2u] = 0x0du;
            encoded[ysiz + 3u] = 0x40u;
            encoded[xtsiz] = 0x00u;
            encoded[xtsiz + 1u] = 0x00u;
            encoded[xtsiz + 2u] = 0x01u;
            encoded[xtsiz + 3u] = 0x00u;
            encoded[ytsiz] = 0x00u;
            encoded[ytsiz + 1u] = 0x00u;
            encoded[ytsiz + 2u] = 0x01u;
            encoded[ytsiz + 3u] = 0x00u;
            patched = 1;
            break;
        }
    }
    if (!patched) {
        free(encoded);
        fprintf(stderr, "no SIZ marker in the tiled fixture\n");
        return 0;
    }
    status = qw_image_decode(encoded, encoded_size, &image);
    free(encoded);
    if (status != QW_IMAGE_ERR_RESOURCE_LIMIT) {
        fprintf(stderr, "oversized tiled codestream status %d (%s), expected %d\n",
                status, qw_image_status_string(status),
                QW_IMAGE_ERR_RESOURCE_LIMIT);
        return 0;
    }
    if (image.pixels != NULL) {
        fputs("oversized tiled codestream published pixels\n", stderr);
        qw_image_free(&image);
        return 0;
    }
    puts("oversized tiled codestream rejected with RESOURCE_LIMIT");
    return 1;
}

int main(void)
{
    static const ReferenceCase references[] = {
        {"gray97_openjpeg_32x24.jp2", "gray97_openjpeg_32x24.gray", 32u, 24u, 1},
        {"gray97_openjpeg_rate20_32x24.jp2",
         "gray97_openjpeg_rate20_32x24.gray", 32u, 24u, 1},
        {"synthetic_gray8_16x16_tiles_8x8_97_no_roi.j2k",
         "synthetic_gray8_16x16_tiles_8x8_97_no_roi_reference.gray8", 16u, 16u, 1},
        {"synthetic_gray8_16x16_tiles_8x8_97_roi_u2.j2k",
         "synthetic_gray8_16x16_tiles_8x8_97_roi_u2_reference.gray8", 16u, 16u, 1},
        {"synthetic_gray8_16x16_tiles_8x8_97_roi_u4.j2k",
         "synthetic_gray8_16x16_tiles_8x8_97_roi_u4_reference.gray8", 16u, 16u, 1},
        {"synthetic_gray8_origin2x2_20x14_tiles8x8_97.j2k",
         "synthetic_gray8_origin2x2_20x14_tiles8x8_97_reference.gray8", 20u, 14u, 1},
        {"synthetic_gray8_subsampled_16x16_dx2x1_tiles8x8.j2k",
         "synthetic_gray8_subsampled_16x16_dx2x1_reference.gray8", 8u, 16u, 1},
        {"synthetic_gray8_subsampled_16x16_dx2x2_tiles8x8.j2k",
         "synthetic_gray8_subsampled_16x16_dx2x2_reference.gray8", 8u, 8u, 1},
        {"synthetic_gray8_subsampled_20x14_dx2x1_tiles8x8.j2k",
         "synthetic_gray8_subsampled_20x14_dx2x1_reference.gray8", 10u, 14u, 1},
    };
    size_t index;
    int failures = 0;

    for (index = 0u; index < sizeof(references) / sizeof(references[0]); ++index) {
        if (!run_reference_case(&references[index])) ++failures;
    }
    if (!run_decode_case("gray53_13x17.jp2", 13u, 17u, QW_IMAGE_PIXEL_GRAY8))
        ++failures;
    if (!run_decode_case("synthetic_rgb8_mct_ict_16x16.jp2", 16u, 16u,
                         QW_IMAGE_PIXEL_RGB24))
        ++failures;
    if (!run_decode_case("synthetic_rgb8_mct_rct_16x16.jp2", 16u, 16u,
                         QW_IMAGE_PIXEL_RGB24))
        ++failures;
    if (!run_decode_case("synthetic_rgba8_alpha_16x16.jp2", 16u, 16u,
                         QW_IMAGE_PIXEL_RGB24))
        ++failures;
    if (!run_decode_case("synthetic_rgba8_cdef_premultiplied_alpha_component2_16x16.jp2",
                         16u, 16u, QW_IMAGE_PIXEL_RGB24))
        ++failures;
    if (!run_decode_case("synthetic_gray8_palette_gray_cmap_13x17.jp2", 13u, 17u,
                         QW_IMAGE_PIXEL_GRAY8))
        ++failures;
    if (!run_decode_case("synthetic_rgb8_no_mct_420_16x16.jp2", 16u, 16u,
                         QW_IMAGE_PIXEL_RGB24))
        ++failures;
    if (!run_decode_case("t803_profile0_p0_09.j2k", 17u, 37u,
                         QW_IMAGE_PIXEL_GRAY8))
        ++failures;
    if (!run_rgb_reference_case(
            "synthetic_rgb8_sycc_enumcs18_16x16.jp2",
            "synthetic_rgb8_sycc_enumcs18_16x16.rgb24", 16u, 16u))
        ++failures;
    if (!test_oversized_tiled_codestream()) ++failures;
    if (failures) {
        fprintf(stderr, "%d JP2 profile/golden case(s) failed\n", failures);
        return 1;
    }
    puts("JP2 profile matrix golden cases passed");
    return 0;
}
