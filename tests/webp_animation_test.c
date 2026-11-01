#include "image_internal.h"
#include "../src/formats/image/webp_vp8_probs.h"

#include <quirewise/image.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_WEBP_TEST_DATA_DIR
#define QUIREWISE_WEBP_TEST_DATA_DIR "tests/data/webp"
#endif

static int read_fixture(const char *name, uint8_t **out_bytes, size_t *out_size)
{
    char path[1024];
    FILE *file;
    long length;
    uint8_t *bytes;
    int written = snprintf(path, sizeof(path), "%s/%s",
                           QUIREWISE_WEBP_TEST_DATA_DIR, name);
    if (written < 0 || (size_t)written >= sizeof(path)) return 0;
    file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    bytes = (uint8_t *)malloc((size_t)length);
    if (!bytes) {
        fclose(file);
        return 0;
    }
    if (fread(bytes, 1u, (size_t)length, file) != (size_t)length) {
        free(bytes);
        fclose(file);
        return 0;
    }
    fclose(file);
    *out_bytes = bytes;
    *out_size = (size_t)length;
    return 1;
}

static uint64_t checksum(const uint8_t *bytes, size_t size)
{
    uint64_t value = UINT64_C(1469598103934665603);
    for (size_t i = 0u; i < size; ++i) {
        value ^= bytes[i];
        value *= UINT64_C(1099511628211);
    }
    return value;
}

static uint32_t test_read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void test_write_u32_le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static int expect_webp_parse_status(const char *case_name,
                                    const void *data,
                                    size_t size,
                                    int expected_status)
{
    QwWebPDocument document = {0};
    uint32_t width = 0u;
    uint32_t height = 0u;
    int has_animation = 0;
    int rc = qw_webp_parse(data, size, &width, &height, &has_animation);

    if (rc != expected_status) {
        fprintf(stderr,
                "WebP %s parse returned %d, expected %d\n",
                case_name, rc, expected_status);
        return 0;
    }
    rc = qw_webp_document_parse(data, size, &document);
    if (rc != expected_status || document.frames || document.frame_count ||
        document.width || document.height) {
        fprintf(stderr,
                "WebP %s document parse returned %d, expected %d\n",
                case_name, rc, expected_status);
        qw_webp_document_clear(&document);
        return 0;
    }
    qw_webp_document_clear(&document);
    return 1;
}

static int test_webp_malformed_inputs_and_limits(void)
{
    static const uint8_t bad_padding_webp[22] = {
        'R', 'I', 'F', 'F', 14u, 0u, 0u, 0u,
        'W', 'E', 'B', 'P', 'J', 'U', 'N', 'K',
        1u, 0u, 0u, 0u, 0x42u, 0x7fu
    };
    uint8_t *valid = NULL;
    uint8_t *mutated = NULL;
    uint8_t *animation = NULL;
    uint8_t *many_frames = NULL;
    size_t valid_size = 0u;
    size_t animation_size = 0u;
    size_t many_frames_size = 0u;
    int rc;

    if (!expect_webp_parse_status("null input", NULL, 0u,
                                  QW_STATUS_ERR_FORMAT) ||
        qw_webp_document_parse(NULL, 0u, NULL) !=
            QW_STATUS_ERR_INVALID_ARGUMENT ||
        !read_fixture("vp8_quantizer_q5_31x17.webp", &valid, &valid_size)) {
        return 0;
    }
    mutated = (uint8_t *)malloc(valid_size);
    if (!mutated) goto fail;

    if (!expect_webp_parse_status("empty payload", valid, 0u,
                                  QW_STATUS_ERR_FORMAT) ||
        !expect_webp_parse_status("truncated RIFF header", valid, 11u,
                                  QW_STATUS_ERR_FORMAT)) {
        goto fail;
    }

    memcpy(mutated, valid, valid_size);
    mutated[0] ^= 1u;
    if (!expect_webp_parse_status("bad RIFF signature", mutated, valid_size,
                                  QW_STATUS_ERR_FORMAT)) {
        goto fail;
    }
    memcpy(mutated, valid, valid_size);
    mutated[8] ^= 1u;
    if (!expect_webp_parse_status("bad WEBP signature", mutated, valid_size,
                                  QW_STATUS_ERR_FORMAT)) {
        goto fail;
    }
    memcpy(mutated, valid, valid_size);
    mutated[4] |= 1u;
    if (!expect_webp_parse_status("odd RIFF size", mutated, valid_size,
                                  QW_STATUS_ERR_FORMAT)) {
        goto fail;
    }
    memcpy(mutated, valid, valid_size);
    test_write_u32_le(mutated + 4u, UINT32_MAX);
    if (!expect_webp_parse_status("RIFF size beyond input", mutated,
                                  valid_size, QW_STATUS_ERR_FORMAT)) {
        goto fail;
    }
    memcpy(mutated, valid, valid_size);
    test_write_u32_le(mutated + 16u, UINT32_MAX);
    if (!expect_webp_parse_status("chunk size beyond RIFF", mutated,
                                  valid_size, QW_STATUS_ERR_FORMAT)) {
        goto fail;
    }
    memcpy(mutated, valid, valid_size);
    memcpy(mutated + 12u, "JUNK", 4u);
    if (!expect_webp_parse_status("unsupported first chunk", mutated,
                                  valid_size, QW_STATUS_ERR_FORMAT) ||
        !expect_webp_parse_status("nonzero odd-chunk padding",
                                  bad_padding_webp,
                                  sizeof(bad_padding_webp),
                                  QW_STATUS_ERR_FORMAT)) {
        goto fail;
    }

    memcpy(mutated, valid, valid_size);
    test_write_u32_le(mutated + 16u, UINT32_MAX);
    {
        QwImage image = {0};
        rc = qw_image_decode(mutated, valid_size, &image);
        if (rc != QW_IMAGE_ERR_FORMAT || image.pixels || image.size ||
            image.width || image.height) {
            fprintf(stderr,
                    "public WebP decode malformed-input result mismatch (%d)\n",
                    rc);
            qw_image_free(&image);
            goto fail;
        }
    }
    {
        QwWebPAnimation *result = NULL;
        size_t frame_count = 99u;
        rc = qw_webp_animation_create(mutated, valid_size, &result,
                                      &frame_count);
        if (rc != QW_STATUS_ERR_FORMAT || result || frame_count != 0u) {
            fprintf(stderr,
                    "public WebP animation malformed-input result mismatch (%d, %zu)\n",
                    rc, frame_count);
            qw_webp_animation_free(result);
            goto fail;
        }
    }

    if (!read_fixture("rgb_frames_u1.webp", &animation, &animation_size) ||
        animation_size < 30u || memcmp(animation + 12u, "VP8X", 4u) != 0) {
        fprintf(stderr, "WebP animation resource-limit fixture invalid\n");
        goto fail;
    }
    {
        uint8_t *resized = (uint8_t *)realloc(mutated, animation_size);
        if (!resized) goto fail;
        mutated = resized;
    }
    memcpy(mutated, animation, animation_size);
    mutated[24] = 0xffu;
    mutated[25] = 0xffu;
    mutated[26] = 0xffu;
    if (!expect_webp_parse_status("oversized animation canvas", mutated,
                                  animation_size,
                                  QW_STATUS_ERR_RESOURCE_LIMIT)) {
        goto fail;
    }

    {
        size_t riff_end = (size_t)test_read_u32_le(animation + 4u) + 8u;
        size_t position = 12u;
        size_t prefix_size = 0u;
        size_t frame_offset = 0u;
        size_t frame_chunk_size = 0u;
        const size_t frame_limit_plus_one = 4097u;

        while (position <= riff_end && riff_end - position >= 8u) {
            uint32_t payload_size = test_read_u32_le(animation + position + 4u);
            size_t padded_size;
            size_t chunk_size;
            if ((size_t)payload_size >
                SIZE_MAX - (size_t)(payload_size & 1u)) {
                goto fail;
            }
            padded_size = (size_t)payload_size +
                          (size_t)(payload_size & 1u);
            if (padded_size > SIZE_MAX - 8u) goto fail;
            chunk_size = 8u + padded_size;
            if (chunk_size > riff_end - position) goto fail;
            if (memcmp(animation + position, "ANMF", 4u) == 0) {
                prefix_size = position;
                frame_offset = position;
                frame_chunk_size = chunk_size;
                break;
            }
            position += chunk_size;
        }
        if (!frame_chunk_size ||
            frame_chunk_size >
                (SIZE_MAX - prefix_size) / frame_limit_plus_one) {
            fprintf(stderr, "WebP animation frame-limit source invalid\n");
            goto fail;
        }
        many_frames_size = prefix_size +
                           frame_chunk_size * frame_limit_plus_one;
        if (many_frames_size < 8u ||
            many_frames_size - 8u > UINT32_MAX) {
            fprintf(stderr, "WebP animation frame-limit size overflow\n");
            goto fail;
        }
        many_frames = (uint8_t *)malloc(many_frames_size);
        if (!many_frames) goto fail;
        memcpy(many_frames, animation, prefix_size);
        for (size_t i = 0u; i < frame_limit_plus_one; ++i) {
            memcpy(many_frames + prefix_size + i * frame_chunk_size,
                   animation + frame_offset, frame_chunk_size);
        }
        test_write_u32_le(many_frames + 4u,
                          (uint32_t)(many_frames_size - 8u));
        if (!expect_webp_parse_status("animation frame-count limit",
                                      many_frames, many_frames_size,
                                      QW_STATUS_ERR_RESOURCE_LIMIT)) {
            goto fail;
        }
    }

    fprintf(stderr,
            "WebP malformed-input and resource tests: RIFF/chunk failures, public decode errors, canvas cap, and 4096-frame cap passed\n");
    free(many_frames);
    free(animation);
    free(mutated);
    free(valid);
    return 1;

fail:
    free(many_frames);
    free(animation);
    free(mutated);
    free(valid);
    return 0;
}

static uint32_t test_mutation_next_u32(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static int test_webp_deterministic_mutations(void)
{
    const uint32_t initial_seed = UINT32_C(0x51a9f00d);
    uint32_t random_state = initial_seed;
    uint8_t *fixture = NULL;
    uint8_t *mutated = NULL;
    size_t fixture_size = 0u;
    size_t parsed_documents = 0u;
    size_t decode_attempts = 0u;
    const size_t iteration_count = 512u;
    int rc;

    if (!read_fixture("vp8_quantizer_q5_31x17.webp", &fixture,
                      &fixture_size)) {
        return 0;
    }
    mutated = (uint8_t *)malloc(fixture_size);
    if (!mutated) goto fail;

    for (size_t iteration = 0u; iteration < iteration_count; ++iteration) {
        QwWebPDocument document = {0};
        uint32_t width = 0u;
        uint32_t height = 0u;
        int has_animation = 0;
        size_t input_size = fixture_size;
        int parse_rc;
        int document_rc;

        memcpy(mutated, fixture, fixture_size);
        for (uint32_t edit = 0u;
             edit < 1u + test_mutation_next_u32(&random_state) % 8u;
             ++edit) {
            size_t offset = (size_t)(
                test_mutation_next_u32(&random_state) % fixture_size);
            uint8_t bit = (uint8_t)(1u << (
                test_mutation_next_u32(&random_state) % 8u));
            mutated[offset] ^= bit;
        }
        if ((iteration & 3u) == 0u) {
            input_size = (size_t)(
                test_mutation_next_u32(&random_state) % (fixture_size + 1u));
        }

        parse_rc = qw_webp_parse(mutated, input_size, &width, &height,
                                 &has_animation);
        document_rc = qw_webp_document_parse(mutated, input_size, &document);
        if (document_rc != parse_rc &&
            document_rc != QW_STATUS_ERR_OUT_OF_MEMORY) {
            fprintf(stderr,
                    "WebP mutation %zu parser/document status mismatch (%d, %d)\n",
                    iteration, parse_rc, document_rc);
            qw_webp_document_clear(&document);
            goto fail;
        }
        if (document_rc == QW_STATUS_OK) {
            QwImage image = {0};
            ++parsed_documents;
            if (!document.width || !document.height ||
                !document.frame_count || !document.frames ||
                document.frame_count > 4096u) {
                fprintf(stderr,
                        "WebP mutation %zu produced invalid document state\n",
                        iteration);
                qw_webp_document_clear(&document);
                goto fail;
            }
            ++decode_attempts;
            rc = qw_image_decode(mutated, input_size, &image);
            if (rc == QW_IMAGE_OK) {
                if (!image.pixels || !image.width || !image.height ||
                    !image.size || !image.stride) {
                    fprintf(stderr,
                            "WebP mutation %zu produced an invalid image\n",
                            iteration);
                    qw_image_free(&image);
                    qw_webp_document_clear(&document);
                    goto fail;
                }
            } else if (image.pixels || image.width || image.height ||
                       image.size || image.stride) {
                fprintf(stderr,
                        "WebP mutation %zu left partial image output (%d)\n",
                        iteration, rc);
                qw_image_free(&image);
                qw_webp_document_clear(&document);
                goto fail;
            }
            qw_image_free(&image);
        } else if (document.frames || document.frame_count ||
                   document.width || document.height) {
            fprintf(stderr,
                    "WebP mutation %zu left partial document output (%d)\n",
                    iteration, document_rc);
            qw_webp_document_clear(&document);
            goto fail;
        }
        qw_webp_document_clear(&document);
    }

    fprintf(stderr,
            "WebP deterministic mutation smoke: %zu cases seed=0x%08x, parsed=%zu, decode attempts=%zu\n",
            iteration_count, initial_seed, parsed_documents, decode_attempts);
    free(mutated);
    free(fixture);
    return 1;

fail:
    free(mutated);
    free(fixture);
    return 0;
}

static int test_still_image(void)
{
    uint8_t *bytes = NULL;
    size_t size = 0u;
    QwWebPDocument document = {0};
    QwImage image = {0};
    uint8_t *rgba = NULL;
    size_t mismatch_count = 0u;
    int rc;
    if (!read_fixture("rgb_u1.webp", &bytes, &size)) return 0;
    rc = qw_webp_document_parse(bytes, size, &document);
    if (rc != QW_STATUS_OK || document.width != 31u ||
        document.height != 32u || document.frame_count != 1u ||
        document.frames[0].codec != QW_WEBP_CODEC_VP8L) {
        fprintf(stderr, "still WebP metadata check failed (%d)\n", rc);
        qw_webp_document_clear(&document);
        free(bytes);
        return 0;
    }
    rc = qw_webp_vp8l_decode(document.frames[0].image_data,
                             document.frames[0].image_size,
                             document.width, document.height, &rgba);
    if (rc != QW_STATUS_OK || !rgba ||
        checksum(rgba, 31u * 32u * 4u) !=
            UINT64_C(14343047214590250468)) {
        fprintf(stderr, "internal VP8L decode/golden check failed (%d)\n", rc);
        free(rgba);
        qw_webp_document_clear(&document);
        free(bytes);
        return 0;
    }
    rc = qw_image_decode(bytes, size, &image);
    if (rc != QW_IMAGE_OK || image.width != 31u || image.height != 32u ||
        image.format != QW_IMAGE_PIXEL_RGB24 || image.stride != 31u * 3u ||
        image.size != 31u * 32u * 3u ||
        checksum(image.pixels, image.size) != UINT64_C(5470596801655083028)) {
        fprintf(stderr, "still WebP decode check failed (%d)\n", rc);
        free(rgba);
        qw_image_free(&image);
        qw_webp_document_clear(&document);
        free(bytes);
        return 0;
    }
    for (size_t i = 0u; i < 31u * 32u; ++i) {
        uint32_t alpha = rgba[i * 4u + 3u];
        for (size_t channel = 0u; channel < 3u; ++channel) {
            uint8_t expected = (uint8_t)(
                ((uint32_t)rgba[i * 4u + channel] * alpha +
                 255u * (255u - alpha) + 127u) / 255u);
            if (image.pixels[i * 3u + channel] != expected) {
                if (mismatch_count < 8u) {
                    fprintf(stderr,
                            "internal VP8L pixel mismatch at %zu/%zu: got=%u expected=%u rgba=%u,%u,%u,%u\n",
                            i, channel, image.pixels[i * 3u + channel], expected,
                            rgba[i * 4u], rgba[i * 4u + 1u],
                            rgba[i * 4u + 2u], rgba[i * 4u + 3u]);
                }
                ++mismatch_count;
            }
        }
    }
    if (mismatch_count) {
        fprintf(stderr, "internal VP8L total channel mismatches: %zu\n",
                mismatch_count);
        free(rgba);
        qw_image_free(&image);
        qw_webp_document_clear(&document);
        free(bytes);
        return 0;
    }
    printf("still WebP: %ux%u checksum=%llu\n", image.width, image.height,
           (unsigned long long)checksum(image.pixels, image.size));
    free(rgba);
    qw_image_free(&image);
    qw_webp_document_clear(&document);
    free(bytes);
    return 1;
}

static int test_animation(void)
{
    static const uint64_t expected_rgb_checksums[11] = {
        UINT64_C(5470596801655083028), UINT64_C(271011114156094671),
        UINT64_C(2682042060637973892), UINT64_C(5051291675727940161),
        UINT64_C(8892731812114302595), UINT64_C(12391003480602562399),
        UINT64_C(5815960801826482567), UINT64_C(15813960959045415402),
        UINT64_C(11722307100702831672), UINT64_C(2998413612456744324),
        UINT64_C(10866955581310141172)
    };
    uint8_t *bytes = NULL;
    size_t size = 0u;
    size_t frame_count = 0u;
    uint64_t first_checksum = 0u;
    uint64_t last_checksum = 0u;
    QwWebPDocument document = {0};
    QwWebPAnimation *animation = NULL;
    int rc;
    if (!read_fixture("rgb_frames_u1.webp", &bytes, &size)) return 0;
    rc = qw_webp_document_parse(bytes, size, &document);
    if (rc != QW_STATUS_OK || !document.has_animation ||
        document.width != 31u || document.height != 32u ||
        document.frame_count != 11u || document.loop_count != 0u) {
        fprintf(stderr, "lossless animation metadata failed (%d)\n", rc);
        qw_webp_document_clear(&document);
        free(bytes);
        return 0;
    }
    for (size_t i = 0u; i < document.frame_count; ++i) {
        const QwWebPFrame *frame = &document.frames[i];
        if (frame->codec != QW_WEBP_CODEC_VP8L || frame->x != 0u ||
            frame->y != 0u || frame->width != 31u || frame->height != 32u ||
            frame->duration_ms != 100u || frame->has_alpha) {
            fprintf(stderr, "lossless animation frame metadata failed at %zu\n",
                    i);
            qw_webp_document_clear(&document);
            free(bytes);
            return 0;
        }
    }
    qw_webp_document_clear(&document);
    rc = qw_webp_animation_create(bytes, size, &animation, &frame_count);
    free(bytes);
    if (rc != QW_STATUS_OK || frame_count != 11u) {
        fprintf(stderr, "animation create failed (%d, %zu frames)\n",
                rc, frame_count);
        qw_webp_animation_free(animation);
        return 0;
    }
    for (size_t i = 0u; i < frame_count; ++i) {
        QwPageImage page = {0};
        uint32_t duration_ms = 0u;
        rc = qw_webp_animation_frame_duration(animation, i, &duration_ms);
        if (rc != QW_STATUS_OK || duration_ms != 100u) {
            fprintf(stderr, "frame %zu duration check failed (%d, %u)\n",
                    i, rc, duration_ms);
            qw_webp_animation_free(animation);
            return 0;
        }
        rc = qw_webp_animation_read(animation, i, &page);
        if (rc != QW_STATUS_OK || page.width != 31u || page.height != 32u ||
            page.kind != QW_PAGE_IMAGE_RGB24 ||
            page.size != 31u * 32u * 3u || !page.data) {
            fprintf(stderr, "frame %zu decode check failed (%d)\n", i, rc);
            qw_image_release_page(&page);
            qw_webp_animation_free(animation);
            return 0;
        }
        {
            uint64_t actual_checksum = checksum(page.data, page.size);
            if (actual_checksum != expected_rgb_checksums[i]) {
                fprintf(stderr,
                        "lossless animation RGB golden failed at %zu: %llu\n",
                        i, (unsigned long long)actual_checksum);
                qw_image_release_page(&page);
                qw_webp_animation_free(animation);
                return 0;
            }
            if (i == 0u) first_checksum = actual_checksum;
            if (i + 1u == frame_count) last_checksum = actual_checksum;
        }
        qw_image_release_page(&page);
    }
    if (first_checksum == last_checksum) {
        fprintf(stderr, "animation fixture did not produce distinct frames\n");
        qw_webp_animation_free(animation);
        return 0;
    }
    {
        QwPageImage repeated = {0};
        rc = qw_webp_animation_read(animation, 0u, &repeated);
        if (rc != QW_STATUS_OK || checksum(repeated.data, repeated.size) !=
                                  expected_rgb_checksums[0]) {
            fprintf(stderr, "animation seek/reset check failed (%d)\n", rc);
            qw_image_release_page(&repeated);
            qw_webp_animation_free(animation);
            return 0;
        }
        qw_image_release_page(&repeated);
    }
    printf("animation WebP: %zu frames, duration=100ms, first=%llu last=%llu\n",
           frame_count, (unsigned long long)first_checksum,
           (unsigned long long)last_checksum);
    qw_webp_animation_free(animation);
    return 1;
}

static int test_lossy_vp8_animation(void)
{
    static const uint32_t expected_durations[3] = {100u, 120u, 140u};
    static const uint64_t expected_rgb_checksums[3] = {
        UINT64_C(12616517728337661522),
        UINT64_C(12738557293009468166),
        UINT64_C(10601102309909307404)
    };
    uint8_t *bytes = NULL;
    size_t size = 0u;
    size_t frame_count = 0u;
    QwWebPDocument document = {0};
    QwWebPAnimation *animation = NULL;
    int rc;

    if (!read_fixture("vp8_context_animation.webp", &bytes, &size)) return 0;
    rc = qw_webp_document_parse(bytes, size, &document);
    if (rc != QW_STATUS_OK || !document.has_animation ||
        document.width != 64u || document.height != 64u ||
        document.frame_count != 3u || document.loop_count != 2u) {
        fprintf(stderr, "lossy VP8 animation metadata failed (%d)\n", rc);
        goto fail;
    }
    for (size_t i = 0u; i < document.frame_count; ++i) {
        if (document.frames[i].codec != QW_WEBP_CODEC_VP8 ||
            document.frames[i].width != 64u ||
            document.frames[i].height != 64u ||
            document.frames[i].duration_ms != expected_durations[i]) {
            fprintf(stderr, "lossy VP8 animation frame metadata failed at %zu\n",
                    i);
            goto fail;
        }
    }
    rc = qw_webp_animation_create(bytes, size, &animation, &frame_count);
    qw_webp_document_clear(&document);
    if (rc != QW_STATUS_OK || frame_count != 3u || !animation) {
        fprintf(stderr, "lossy VP8 animation create failed (%d, %zu)\n",
                rc, frame_count);
        goto fail;
    }
    free(bytes);
    bytes = NULL;

    for (size_t i = 0u; i < frame_count; ++i) {
        QwPageImage page = {0};
        uint32_t duration = 0u;
        rc = qw_webp_animation_frame_duration(animation, i, &duration);
        if (rc != QW_STATUS_OK || duration != expected_durations[i]) {
            fprintf(stderr, "lossy VP8 animation duration failed at %zu (%d)\n",
                    i, rc);
            qw_webp_animation_free(animation);
            return 0;
        }
        rc = qw_webp_animation_read(animation, i, &page);
        if (rc != QW_STATUS_OK || page.width != 64u || page.height != 64u ||
            page.kind != QW_PAGE_IMAGE_RGB24 ||
            page.size != 64u * 64u * 3u || !page.data ||
            checksum(page.data, page.size) != expected_rgb_checksums[i]) {
            fprintf(stderr, "lossy VP8 animation RGB golden failed at %zu (%d)\n",
                    i, rc);
            qw_image_release_page(&page);
            qw_webp_animation_free(animation);
            return 0;
        }
        qw_image_release_page(&page);
    }
    {
        QwPageImage repeated = {0};
        rc = qw_webp_animation_read(animation, 0u, &repeated);
        if (rc != QW_STATUS_OK || checksum(repeated.data, repeated.size) !=
                                  expected_rgb_checksums[0]) {
            fprintf(stderr, "lossy VP8 animation reset check failed (%d)\n", rc);
            qw_image_release_page(&repeated);
            qw_webp_animation_free(animation);
            return 0;
        }
        qw_image_release_page(&repeated);
    }
    fprintf(stderr,
            "lossy VP8 animation: three internal VP8 frames, durations 100/120/140 ms, RGB goldens and reset passed\n");
    qw_webp_animation_free(animation);
    return 1;

fail:
    qw_webp_document_clear(&document);
    qw_webp_animation_free(animation);
    free(bytes);
    return 0;
}

static int test_vp8l_alpha_fixture(void)
{
    uint8_t *bytes = NULL;
    uint8_t *rgba = NULL;
    size_t size = 0u;
    QwWebPDocument document = {0};
    QwImage image = {0};
    int rc;

    if (!read_fixture("rgba_lossless.webp", &bytes, &size)) return 0;
    rc = qw_webp_document_parse(bytes, size, &document);
    if (rc != QW_STATUS_OK || document.frame_count != 1u ||
        document.width != 8u || document.height != 4u ||
        document.frames[0].codec != QW_WEBP_CODEC_VP8L ||
        !document.frames[0].has_alpha) {
        fprintf(stderr, "lossless alpha metadata check failed (%d)\n", rc);
        goto fail;
    }
    rc = qw_webp_vp8l_decode(document.frames[0].image_data,
                             document.frames[0].image_size,
                             document.width, document.height, &rgba);
    if (rc != QW_STATUS_OK || !rgba ||
        checksum(rgba, 8u * 4u * 4u) !=
            UINT64_C(9196594025185228323)) {
        fprintf(stderr, "lossless alpha decode/golden check failed (%d)\n", rc);
        goto fail;
    }
    rc = qw_image_decode(bytes, size, &image);
    if (rc != QW_IMAGE_OK || image.width != 8u || image.height != 4u ||
        image.stride != 8u * 3u || image.size != 8u * 4u * 3u ||
        checksum(image.pixels, image.size) != UINT64_C(3875992943014453876)) {
        fprintf(stderr, "lossless alpha public decode failed (%d)\n", rc);
        goto fail;
    }
    for (uint32_t y = 0u; y < 4u; ++y) {
        for (uint32_t x = 0u; x < 8u; ++x) {
            size_t i = (size_t)y * 8u + x;
            uint8_t source[4] = {
                (uint8_t)(x * 31u + y * 7u),
                (uint8_t)(y * 61u + x * 9u),
                (uint8_t)(x * 17u + y * 43u),
                (uint8_t)(x * 29u + y * 47u)
            };
            uint32_t alpha = source[3];
            for (size_t channel = 0u; channel < 4u; ++channel) {
                if (rgba[i * 4u + channel] != source[channel]) {
                    fprintf(stderr, "lossless alpha RGBA mismatch at %zu/%zu\n",
                            i, channel);
                    goto fail;
                }
            }
            for (size_t channel = 0u; channel < 3u; ++channel) {
                uint8_t expected = (uint8_t)(
                    ((uint32_t)source[channel] * alpha +
                     255u * (255u - alpha) + 127u) / 255u);
                if (image.pixels[i * 3u + channel] != expected) {
                    fprintf(stderr,
                            "lossless alpha composition mismatch at %zu/%zu\n",
                            i, channel);
                    goto fail;
                }
            }
        }
    }
    qw_image_free(&image);
    qw_webp_document_clear(&document);
    free(rgba);
    free(bytes);
    return 1;

fail:
    qw_image_free(&image);
    qw_webp_document_clear(&document);
    free(rgba);
    free(bytes);
    return 0;
}

static int test_alpha_chunk(void)
{
    static const uint8_t expected_alpha[32] = {
        0u, 29u, 58u, 87u, 116u, 145u, 174u, 203u,
        47u, 76u, 105u, 134u, 163u, 192u, 221u, 250u,
        94u, 123u, 152u, 181u, 210u, 239u, 12u, 41u,
        141u, 170u, 199u, 228u, 1u, 30u, 59u, 88u
    };
    uint8_t *bytes = NULL;
    uint8_t *internal_alpha = NULL;
    size_t size = 0u;
    QwWebPDocument document = {0};
    QwImage image = {0};
    int rc;

    if (!read_fixture("rgba_alpha.webp", &bytes, &size)) return 0;
    rc = qw_webp_document_parse(bytes, size, &document);
    if (rc != QW_STATUS_OK || document.frame_count != 1u ||
        document.frames[0].codec != QW_WEBP_CODEC_VP8 ||
        !document.frames[0].alpha_data) {
        fprintf(stderr, "VP8 alpha chunk metadata check failed (%d)\n", rc);
        goto fail;
    }
    rc = qw_webp_alpha_decode(document.frames[0].alpha_data,
                              document.frames[0].alpha_size,
                              document.width, document.height,
                              &internal_alpha);
    if (rc != QW_STATUS_OK || !internal_alpha || document.width != 8u ||
        document.height != 4u) {
        fprintf(stderr, "VP8 alpha chunk decode failed (%d)\n", rc);
        goto fail;
    }
    for (size_t i = 0u; i < sizeof(expected_alpha); ++i) {
        if (expected_alpha[i] != internal_alpha[i]) {
            fprintf(stderr, "VP8 alpha mismatch at %zu: got=%u expected=%u\n",
                    i, internal_alpha[i], expected_alpha[i]);
            goto fail;
        }
    }
    if (checksum(internal_alpha, sizeof(expected_alpha)) !=
        UINT64_C(3807058776228185547)) {
        fprintf(stderr, "VP8 alpha plane golden hash mismatch\n");
        goto fail;
    }
    rc = qw_image_decode(bytes, size, &image);
    if (rc != QW_IMAGE_OK || image.width != document.width ||
        image.height != document.height || image.stride != 8u * 3u ||
        image.size != 8u * 4u * 3u ||
        checksum(image.pixels, image.size) != UINT64_C(16152885979878361395)) {
        fprintf(stderr, "internal ALPH still decode failed (%d)\n", rc);
        goto fail;
    }
    printf("VP8 alpha chunk: %ux%u checksum=%llu\n", image.width,
           image.height, (unsigned long long)checksum(image.pixels, image.size));
    qw_image_free(&image);
    qw_webp_document_clear(&document);
    free(internal_alpha);
    free(bytes);
    return 1;

fail:
    qw_image_free(&image);
    qw_webp_document_clear(&document);
    free(internal_alpha);
    free(bytes);
    return 0;
}

typedef struct {
    int16_t u_dc[4];
    int16_t v_dc[4];
} TestWebPVP8ChromaDc;

static int test_vp8_probe_chroma_dc(
    void *context,
    size_t macroblock_x,
    size_t macroblock_y,
    const QwWebPVP8Macroblock *macroblock,
    const int16_t coefficients[25][16])
{
    TestWebPVP8ChromaDc *probe = (TestWebPVP8ChromaDc *)context;
    if (!probe || !macroblock || !coefficients ||
        macroblock_x != 0u || macroblock_y != 0u) {
        return QW_STATUS_ERR_FORMAT;
    }
    for (size_t block = 0u; block < 4u; ++block) {
        probe->u_dc[block] = coefficients[16u + block][0];
        probe->v_dc[block] = coefficients[20u + block][0];
    }
    return QW_STATUS_OK;
}

static int test_vp8_reconstruction_diagnostic(void)
{
    uint8_t *bytes = NULL;
    uint8_t *reconstructed = NULL;
    uint8_t *alpha = NULL;
    size_t size = 0u;
    size_t max_channel_error = 0u;
    size_t total_channel_error = 0u;
    size_t pixels;
    QwWebPVP8Header header = {0};
    QwWebPVP8Macroblock *macroblocks = NULL;
    size_t macroblock_count = 0u;
    TestWebPVP8ChromaDc chroma_dc = {{0}, {0}};
    QwWebPDocument document = {0};
    QwImage reference = {0};
    int rc;

    if (!read_fixture("rgba_alpha.webp", &bytes, &size)) return 0;
    rc = qw_webp_document_parse(bytes, size, &document);
    if (rc != QW_STATUS_OK || document.frame_count != 1u ||
        document.frames[0].codec != QW_WEBP_CODEC_VP8 ||
        !document.frames[0].alpha_data) {
        fprintf(stderr, "VP8 reconstruction fixture parse failed (%d)\n", rc);
        goto fail;
    }
    rc = qw_webp_vp8_parse_keyframe_header(
        document.frames[0].image_data, document.frames[0].image_size,
        document.width, document.height, &header);
    if (rc == QW_STATUS_OK)
        rc = qw_webp_vp8_decode_macroblock_modes(
            document.frames[0].image_data, document.frames[0].image_size,
            &header, &macroblocks, &macroblock_count);
    if (rc != QW_STATUS_OK || macroblock_count != 1u ||
        macroblocks[0].y_mode != 4u || macroblocks[0].uv_mode != 2u) {
        fprintf(stderr,
                "VP8 diagnostic header parse failed (%d, %zu, y=%u, uv=%u)\n",
                rc, macroblock_count,
                macroblocks ? macroblocks[0].y_mode : 255u,
                macroblocks ? macroblocks[0].uv_mode : 255u);
        goto fail;
    }
    rc = qw_webp_vp8_decode_coefficients(
        document.frames[0].image_data, document.frames[0].image_size,
        &header, macroblocks, macroblock_count,
        test_vp8_probe_chroma_dc, &chroma_dc);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "VP8 diagnostic coefficient decode failed (%d)\n", rc);
        goto fail;
    }
    {
        static const int16_t expected_v_dc[4] = {-5, 3, -8, -1};
        /* This fixture verifies that UV coefficient zero is decoded. */
        for (size_t block = 0u; block < 4u; ++block) {
            if (chroma_dc.u_dc[block] != 0 ||
                chroma_dc.v_dc[block] != expected_v_dc[block]) {
                fprintf(stderr,
                        "VP8 chroma DC coefficient check failed at block %zu: U=%d V=%d expected U=0 V=%d\n",
                        block, (int)chroma_dc.u_dc[block],
                        (int)chroma_dc.v_dc[block],
                        (int)expected_v_dc[block]);
                goto fail;
            }
        }
    }
    free(macroblocks);
    macroblocks = NULL;
    rc = qw_webp_vp8_decode_rgba(document.frames[0].image_data,
                                 document.frames[0].image_size,
                                 document.width, document.height,
                                 &reconstructed);
    if (rc != QW_STATUS_OK || !reconstructed) {
        fprintf(stderr, "internal VP8 reconstruction failed (%d)\n", rc);
        goto fail;
    }
    rc = qw_webp_alpha_decode(document.frames[0].alpha_data,
                              document.frames[0].alpha_size,
                              document.width, document.height, &alpha);
    if (rc != QW_STATUS_OK || !alpha) {
        fprintf(stderr, "internal VP8 alpha decode failed (%d)\n", rc);
        goto fail;
    }
    rc = qw_image_decode(bytes, size, &reference);
    pixels = (size_t)document.width * document.height;
    if (rc != QW_IMAGE_OK || reference.size != pixels * 3u) {
        fprintf(stderr, "VP8 reconstruction reference decode failed (%d)\n", rc);
        goto fail;
    }
    for (size_t i = 0u; i < pixels; ++i) {
        for (size_t channel = 0u; channel < 3u; ++channel) {
            uint8_t value = (uint8_t)(
                ((uint32_t)reconstructed[i * 4u + channel] * alpha[i] +
                 255u * (255u - alpha[i]) + 127u) / 255u);
            uint8_t expected = reference.pixels[i * 3u + channel];
            size_t error = value > expected ? value - expected : expected - value;
            if (error > max_channel_error) max_channel_error = error;
            total_channel_error += error;
        }
    }
    printf("VP8 reconstruction diagnostic: %ux%u, max RGB error=%zu, mean RGB error=%.3f\n",
           document.width, document.height, max_channel_error,
           (double)total_channel_error / (double)(pixels * 3u));
    qw_image_free(&reference);
    qw_webp_document_clear(&document);
    free(alpha);
    free(reconstructed);
    free(macroblocks);
    free(bytes);
    return 1;

fail:
    qw_image_free(&reference);
    qw_webp_document_clear(&document);
    free(alpha);
    free(reconstructed);
    free(macroblocks);
    free(bytes);
    return 0;
}

static int test_alpha_raw_filters(void)
{
    static const uint8_t expected[6] = {10u, 20u, 30u, 40u, 50u, 60u};
    static const uint8_t encoded[4][7] = {
        {0u, 10u, 20u, 30u, 40u, 50u, 60u},
        {4u, 10u, 10u, 10u, 30u, 10u, 10u},
        {8u, 10u, 10u, 10u, 30u, 30u, 30u},
        {12u, 10u, 10u, 10u, 30u, 0u, 0u}
    };
    for (size_t filter = 0u; filter < 4u; ++filter) {
        uint8_t *alpha = NULL;
        int rc = qw_webp_alpha_decode(encoded[filter], sizeof(encoded[filter]),
                                      3u, 2u, &alpha);
        if (rc != QW_STATUS_OK || !alpha ||
            memcmp(alpha, expected, sizeof(expected)) != 0) {
            fprintf(stderr, "raw ALPH filter %zu failed (%d)\n", filter, rc);
            free(alpha);
            return 0;
        }
        free(alpha);
    }
    return 1;
}

typedef struct TestVP8MacroblockConsumerState {
    size_t columns;
    size_t rows;
    size_t count;
} TestVP8MacroblockConsumerState;

static int test_vp8_macroblock_consumer(
    void *context,
    size_t macroblock_x,
    size_t macroblock_y,
    const QwWebPVP8Macroblock *macroblock,
    const int16_t coefficients[25][16])
{
    TestVP8MacroblockConsumerState *state =
        (TestVP8MacroblockConsumerState *)context;
    if (!state || !macroblock || !coefficients ||
        macroblock_x >= state->columns || macroblock_y >= state->rows) {
        return QW_STATUS_ERR_FORMAT;
    }
    if (macroblock->skip_coeff) {
        for (size_t block = 0u; block < 25u; ++block) {
            for (size_t coefficient = 0u; coefficient < 16u; ++coefficient) {
                if (coefficients[block][coefficient] != 0) {
                    return QW_STATUS_ERR_FORMAT;
                }
            }
        }
    }
    ++state->count;
    return QW_STATUS_OK;
}

static int test_vp8_keyframe_header(void)
{
    QwWebPDocument document = {0};
    QwWebPVP8Header header = {0};
    QwWebPVP8Macroblock *macroblocks = NULL;
    QwWebPVP8TokenPartition token_partitions[8] = {0};
    uint8_t *bytes = NULL;
    uint8_t *malformed = NULL;
    size_t size = 0u;
    size_t macroblock_count = 0u;
    size_t token_partition_count = 0u;
    TestVP8MacroblockConsumerState consumer_state = {0};
    const QwWebPFrame *frame;
    int rc;

    if (!read_fixture("rgba_alpha.webp", &bytes, &size)) return 0;
    rc = qw_webp_document_parse(bytes, size, &document);
    if (rc != QW_STATUS_OK || document.frame_count != 1u ||
        document.frames[0].codec != QW_WEBP_CODEC_VP8) {
        fprintf(stderr, "VP8 fixture parse failed (%d)\n", rc);
        goto fail;
    }
    frame = &document.frames[0];
    rc = qw_webp_vp8_parse_keyframe_header(
        frame->image_data, frame->image_size,
        frame->width, frame->height, &header);
    if (rc != QW_STATUS_OK || header.width != frame->width ||
        header.height != frame->height || header.version > 3u ||
        header.color_space != 0u || header.clamp_type > 1u ||
        header.loop_filter_level > 63u ||
        header.loop_filter_sharpness > 7u ||
        (header.token_partition_count != 1u &&
         header.token_partition_count != 2u &&
         header.token_partition_count != 4u &&
         header.token_partition_count != 8u) ||
        !header.first_partition_size ||
        header.first_partition_offset > frame->image_size ||
        header.first_partition_position < 2u ||
        header.first_partition_position > header.first_partition_size ||
        header.first_partition_range < 128u ||
        header.first_partition_range > 255u ||
        (!header.skip_coeff_enabled && header.skip_coeff_probability != 0u)) {
        fprintf(stderr, "VP8 keyframe header parse failed (%d)\n", rc);
        goto fail;
    }
    rc = qw_webp_vp8_decode_macroblock_modes(
        frame->image_data, frame->image_size, &header,
        &macroblocks, &macroblock_count);
    if (rc != QW_STATUS_OK || macroblock_count !=
            ((size_t)(frame->width + 15u) / 16u) *
            ((size_t)(frame->height + 15u) / 16u)) {
        fprintf(stderr, "VP8 macroblock mode parse failed (%d, %zu)\n",
                rc, macroblock_count);
        goto fail;
    }
    for (size_t i = 0u; i < macroblock_count; ++i) {
        const QwWebPVP8Macroblock *macroblock = &macroblocks[i];
        if (macroblock->segment_id > 3u || macroblock->skip_coeff > 1u ||
            macroblock->y_mode > 4u || macroblock->uv_mode > 3u) {
            fprintf(stderr, "VP8 macroblock mode value out of range\n");
            goto fail;
        }
        for (size_t block = 0u; block < 16u; ++block) {
            if (macroblock->b_mode[block] > 9u) {
                fprintf(stderr, "VP8 subblock mode value out of range\n");
                goto fail;
            }
        }
    }
    rc = qw_webp_vp8_parse_token_partitions(
        frame->image_data, frame->image_size, &header,
        token_partitions, &token_partition_count);
    if (rc != QW_STATUS_OK ||
        token_partition_count != header.token_partition_count) {
        fprintf(stderr, "VP8 token partition parse failed (%d, %zu)\n",
                rc, token_partition_count);
        goto fail;
    }
    for (size_t i = 0u; i < token_partition_count; ++i) {
        if (!token_partitions[i].data || token_partitions[i].size < 2u) {
            fprintf(stderr, "VP8 token partition range is invalid\n");
            goto fail;
        }
    }
    rc = qw_webp_vp8_decode_token_coefficients(
        frame->image_data, frame->image_size, &header,
        macroblocks, macroblock_count);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "VP8 coefficient token decode failed (%d)\n", rc);
        goto fail;
    }
    consumer_state.columns = ((size_t)frame->width + 15u) / 16u;
    consumer_state.rows = ((size_t)frame->height + 15u) / 16u;
    rc = qw_webp_vp8_decode_coefficients(
        frame->image_data, frame->image_size, &header,
        macroblocks, macroblock_count, test_vp8_macroblock_consumer,
        &consumer_state);
    if (rc != QW_STATUS_OK || consumer_state.count != macroblock_count) {
        fprintf(stderr, "VP8 coefficient consumer failed (%d, %zu)\n",
                rc, consumer_state.count);
        goto fail;
    }
    if (frame->image_size < 10u) goto fail;
    malformed = (uint8_t *)malloc(frame->image_size);
    if (!malformed) goto fail;
    memcpy(malformed, frame->image_data, frame->image_size);
    malformed[3] ^= 1u;
    rc = qw_webp_vp8_parse_keyframe_header(
        malformed, frame->image_size, frame->width, frame->height, &header);
    if (rc != QW_STATUS_ERR_FORMAT) {
        fprintf(stderr, "VP8 invalid start code was accepted (%d)\n", rc);
        goto fail;
    }
    rc = qw_webp_vp8_parse_keyframe_header(
        frame->image_data, 9u, frame->width, frame->height, &header);
    if (rc != QW_STATUS_ERR_FORMAT) {
        fprintf(stderr, "VP8 truncated keyframe header was accepted (%d)\n", rc);
        goto fail;
    }
    memcpy(malformed, frame->image_data, frame->image_size);
    malformed[0] = (uint8_t)((malformed[0] & 0x1fu) | 0x20u);
    malformed[1] = 0u;
    malformed[2] = 0u;
    rc = qw_webp_vp8_parse_keyframe_header(
        malformed, frame->image_size, frame->width, frame->height, &header);
    if (rc != QW_STATUS_ERR_FORMAT) {
        fprintf(stderr, "VP8 undersized first partition was accepted (%d)\n", rc);
        goto fail;
    }
    memcpy(malformed, frame->image_data, frame->image_size);
    malformed[0] = (uint8_t)((malformed[0] & 0x1fu) | 0x40u);
    malformed[1] = 0u;
    malformed[2] = 0u;
    rc = qw_webp_vp8_parse_keyframe_header(
        malformed, frame->image_size, frame->width, frame->height, &header);
    if (rc != QW_STATUS_ERR_FORMAT) {
        fprintf(stderr, "truncated VP8 coded frame header was accepted (%d)\n", rc);
        goto fail;
    }

    free(macroblocks);
    free(malformed);
    qw_webp_document_clear(&document);
    free(bytes);
    return 1;

fail:
    free(macroblocks);
    free(malformed);
    qw_webp_document_clear(&document);
    free(bytes);
    return 0;
}

static int test_vp8_bpred_above_right(void)
{
    uint8_t plane[48u * 32u];
    uint8_t samples[4];
    static const uint8_t expected_middle[4] = {107u, 108u, 109u, 110u};
    for (size_t y = 0u; y < 32u; ++y) {
        for (size_t x = 0u; x < 48u; ++x)
            plane[y * 48u + x] = (uint8_t)(y * 5u + x);
    }
    for (size_t i = 0u; i < 4u; ++i)
        plane[19u * 48u + 32u + i] = (uint8_t)(240u + i);

    if (qw_webp_vp8_bpred_above_right(
            plane, 48u, 3u, 1u, 1u, samples) != QW_STATUS_OK ||
        memcmp(samples, expected_middle, sizeof(expected_middle)) != 0) {
        fprintf(stderr, "VP8 B_PRED above-right copy-down check failed\n");
        return 0;
    }
    if (qw_webp_vp8_bpred_above_right(
            plane, 48u, 3u, 2u, 1u, samples) != QW_STATUS_OK ||
        samples[0] != 122u || samples[1] != 122u ||
        samples[2] != 122u || samples[3] != 122u) {
        fprintf(stderr, "VP8 B_PRED frame-right edge check failed\n");
        return 0;
    }
    if (qw_webp_vp8_bpred_above_right(
            plane, 48u, 3u, 1u, 0u, samples) != QW_STATUS_OK ||
        samples[0] != 127u || samples[1] != 127u ||
        samples[2] != 127u || samples[3] != 127u) {
        fprintf(stderr, "VP8 B_PRED frame-top edge check failed\n");
        return 0;
    }
    if (qw_webp_vp8_bpred_above_right(
            plane, 48u, 3u, 3u, 1u, samples) !=
        QW_STATUS_ERR_INVALID_ARGUMENT) {
        fprintf(stderr, "VP8 B_PRED invalid macroblock was accepted\n");
        return 0;
    }
    return 1;
}

typedef struct {
    size_t nonzero_coefficients[6][4];
    size_t nonzero_per_block[6][25];
    int16_t first_macroblock_coefficients[25][16];
    uint64_t coefficient_checksum;
} TestWebPVP8CoeffCounts;

static int test_vp8_count_coefficients(
    void *context,
    size_t macroblock_x,
    size_t macroblock_y,
    const QwWebPVP8Macroblock *macroblock,
    const int16_t coefficients[25][16])
{
    TestWebPVP8CoeffCounts *counts = (TestWebPVP8CoeffCounts *)context;
    size_t macroblock_index = macroblock_y * 3u + macroblock_x;
    if (!counts || !macroblock || !coefficients ||
        macroblock_x >= 3u || macroblock_y >= 2u || macroblock_index >= 6u) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (macroblock_index == 0u) {
        memcpy(counts->first_macroblock_coefficients, coefficients,
               sizeof(counts->first_macroblock_coefficients));
    }
    for (size_t block = 0u; block < 25u; ++block) {
        size_t plane = block < 16u ? 0u :
                       (block < 20u ? 1u :
                        (block < 24u ? 2u : 3u));
        for (size_t coefficient = 0u; coefficient < 16u; ++coefficient) {
            uint16_t encoded = (uint16_t)coefficients[block][coefficient];
            counts->coefficient_checksum ^= (uint8_t)encoded;
            counts->coefficient_checksum *= UINT64_C(1099511628211);
            counts->coefficient_checksum ^= (uint8_t)(encoded >> 8u);
            counts->coefficient_checksum *= UINT64_C(1099511628211);
            if (coefficients[block][coefficient] != 0) {
                ++counts->nonzero_coefficients[macroblock_index][plane];
                ++counts->nonzero_per_block[macroblock_index][block];
            }
        }
    }
    return QW_STATUS_OK;
}

typedef struct {
    size_t macroblock_columns;
    uint8_t token_partition_count;
    uint8_t *above_context;
    uint8_t left_context[8][9];
    size_t token_context_counts[4][3];
} TestVP8CoefficientContextProbe;

typedef struct {
    size_t token_context_counts[4][3];
    uint32_t b_mode_context_mask[4];
    size_t b_mode_context_count;
    size_t exact_pixel_vectors;
} TestVP8ProbabilityContextCoverage;

static int test_vp8_probe_coefficient_block(
    TestVP8CoefficientContextProbe *probe,
    uint8_t left_context[9],
    uint8_t above_context[9],
    size_t token_type,
    size_t block,
    const int16_t coefficients[16])
{
    static const uint8_t left_context_index[25] = {
        0u, 0u, 0u, 0u, 1u, 1u, 1u, 1u,
        2u, 2u, 2u, 2u, 3u, 3u, 3u, 3u,
        4u, 4u, 5u, 5u, 6u, 6u, 7u, 7u, 8u
    };
    static const uint8_t above_context_index[25] = {
        0u, 1u, 2u, 3u, 0u, 1u, 2u, 3u,
        0u, 1u, 2u, 3u, 0u, 1u, 2u, 3u,
        4u, 5u, 4u, 5u, 6u, 7u, 6u, 7u, 8u
    };
    size_t context;
    int has_coefficients = 0;

    if (!probe || !left_context || !above_context || !coefficients ||
        token_type >= 4u || block >= 25u) {
        return 0;
    }
    context = (size_t)left_context[left_context_index[block]] +
              above_context[above_context_index[block]];
    if (context >= 3u) return 0;
    ++probe->token_context_counts[token_type][context];
    for (size_t coefficient = 0u; coefficient < 16u; ++coefficient) {
        if (coefficients[coefficient] != 0) {
            has_coefficients = 1;
            break;
        }
    }
    left_context[left_context_index[block]] = (uint8_t)has_coefficients;
    above_context[above_context_index[block]] = (uint8_t)has_coefficients;
    return 1;
}

static int test_vp8_probe_coefficient_contexts(
    void *context,
    size_t macroblock_x,
    size_t macroblock_y,
    const QwWebPVP8Macroblock *macroblock,
    const int16_t coefficients[25][16])
{
    TestVP8CoefficientContextProbe *probe =
        (TestVP8CoefficientContextProbe *)context;
    uint8_t *left;
    uint8_t *above;
    size_t partition;

    if (!probe || !macroblock || !coefficients ||
        macroblock_x >= probe->macroblock_columns ||
        probe->token_partition_count == 0u ||
        probe->token_partition_count > 8u || macroblock->y_mode > 4u) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    partition = macroblock_y % probe->token_partition_count;
    left = probe->left_context[partition];
    above = probe->above_context + macroblock_x * 9u;
    if (macroblock_x == 0u) memset(left, 0, 9u);
    if (macroblock->skip_coeff) {
        memset(left, 0, 8u);
        memset(above, 0, 8u);
        if (macroblock->y_mode != 4u) {
            left[8] = 0u;
            above[8] = 0u;
        }
        return QW_STATUS_OK;
    }
    if (macroblock->y_mode != 4u &&
        !test_vp8_probe_coefficient_block(
            probe, left, above, 1u, 24u, coefficients[24])) {
        return QW_STATUS_ERR_FORMAT;
    }
    for (size_t block = 0u; block < 16u; ++block) {
        size_t token_type = macroblock->y_mode == 4u ? 3u : 0u;
        if (!test_vp8_probe_coefficient_block(
                probe, left, above, token_type, block, coefficients[block])) {
            return QW_STATUS_ERR_FORMAT;
        }
    }
    for (size_t block = 16u; block < 24u; ++block) {
        if (!test_vp8_probe_coefficient_block(
                probe, left, above, 2u, block, coefficients[block])) {
            return QW_STATUS_ERR_FORMAT;
        }
    }
    return QW_STATUS_OK;
}

static int test_vp8_collect_b_mode_contexts(
    const QwWebPVP8Macroblock *macroblocks,
    size_t macroblock_columns,
    size_t macroblock_rows,
    TestVP8ProbabilityContextCoverage *coverage)
{
    static const uint8_t coarse_mode_to_b_mode[4] = {0u, 2u, 3u, 1u};
    uint8_t *above_modes;
    size_t above_count;

    if (!macroblocks || !coverage || !macroblock_columns ||
        !macroblock_rows || macroblock_columns > SIZE_MAX / 4u) {
        return 0;
    }
    above_count = macroblock_columns * 4u;
    above_modes = (uint8_t *)calloc(above_count, sizeof(*above_modes));
    if (!above_modes) return 0;

    for (size_t mb_y = 0u; mb_y < macroblock_rows; ++mb_y) {
        uint8_t left_modes[4] = {0u, 0u, 0u, 0u};
        for (size_t mb_x = 0u; mb_x < macroblock_columns; ++mb_x) {
            const QwWebPVP8Macroblock *macroblock =
                &macroblocks[mb_y * macroblock_columns + mb_x];
            if (macroblock->y_mode > 4u) {
                free(above_modes);
                return 0;
            }
            if (macroblock->y_mode == 4u) {
                for (size_t block_y = 0u; block_y < 4u; ++block_y) {
                    for (size_t block_x = 0u; block_x < 4u; ++block_x) {
                        size_t block = block_y * 4u + block_x;
                        uint8_t above_mode = block_y
                            ? macroblock->b_mode[block - 4u]
                            : above_modes[mb_x * 4u + block_x];
                        uint8_t left_mode = block_x
                            ? macroblock->b_mode[block - 1u]
                            : left_modes[block_y];
                        size_t pair_index;
                        if (above_mode >= 10u || left_mode >= 10u) {
                            free(above_modes);
                            return 0;
                        }
                        pair_index = (size_t)above_mode * 10u + left_mode;
                        if (!(coverage->b_mode_context_mask[pair_index / 32u] &
                              (UINT32_C(1) << (pair_index % 32u)))) {
                            ++coverage->b_mode_context_count;
                            coverage->b_mode_context_mask[pair_index / 32u] |=
                                UINT32_C(1) << (pair_index % 32u);
                        }
                    }
                }
                for (size_t block_x = 0u; block_x < 4u; ++block_x) {
                    above_modes[mb_x * 4u + block_x] =
                        macroblock->b_mode[12u + block_x];
                }
                for (size_t block_y = 0u; block_y < 4u; ++block_y) {
                    left_modes[block_y] = macroblock->b_mode[block_y * 4u + 3u];
                }
            } else {
                uint8_t block_mode =
                    coarse_mode_to_b_mode[macroblock->y_mode];
                for (size_t block_x = 0u; block_x < 4u; ++block_x) {
                    above_modes[mb_x * 4u + block_x] = block_mode;
                }
                for (size_t block_y = 0u; block_y < 4u; ++block_y) {
                    left_modes[block_y] = block_mode;
                }
            }
        }
    }
    free(above_modes);
    return 1;
}

static int test_vp8_probability_context_coverage(void)
{
    static const char *const fixture_names[] = {
        "rgba_alpha.webp",
        "multi_mb_bpred.webp",
        "odd_vp8_31x17.webp",
        "vp8_filter_level15_q70_31x17.webp",
        "vp8_filter_level27_31x17.webp",
        "vp8_filter_strength100_q127_31x17.webp",
        "vp8_quantizer_q0_31x17.webp",
        "vp8_quantizer_q5_31x17.webp",
        "vp8_quantizer_q127_31x17.webp",
        "vp8_segmentation_q70_31x17.webp",
        "vp8_segmentation_delta_q70_31x17.webp",
        "vp8_simple_filter_q55_31x17.webp",
        "vp8_filter_sharpness7_q70_31x17.webp",
        "vp8_loop_filter_delta_q70_31x17.webp",
        "vp8_multipartition_p1_128x128.webp",
        "vp8_multipartition_p2_128x128.webp",
        "vp8_multipartition_p3_128x128.webp",
        "vp8_prediction_yramp_q70_31x17.webp",
        "vp8_prediction_xramp_q70_31x17.webp",
        "vp8_prediction_diagonal_q70_31x17.webp",
        "vp8_context_blocks_q35.webp",
        "vp8_context_blocks_q55.webp",
        "vp8_context_blocks_q75.webp",
        "vp8_context_checker_q35.webp",
        "vp8_context_checker_q55.webp",
        "vp8_context_checker_q75.webp",
        "vp8_context_noise_q35.webp",
        "vp8_context_noise_q55.webp",
        "vp8_context_noise_q75.webp",
        "vp8_context_ramps_q35.webp",
        "vp8_context_ramps_q55.webp",
        "vp8_context_ramps_q75.webp",
        "vp8_context_stripes_q35.webp",
        "vp8_context_stripes_q55.webp",
        "vp8_context_stripes_q75.webp",
        "vp8_context_random_rgb_seed000_q35_pair_86.webp",
        "vp8_context_random_rgb_seed000_q55_pair_66.webp"
    };
    static const uint64_t expected_context_rgb_checksums[17] = {
        UINT64_C(12616517728337661522), UINT64_C(1858007197893321748),
        UINT64_C(13070676152755274337), UINT64_C(12009927220139722471),
        UINT64_C(5607397932434198737), UINT64_C(9612038741701586629),
        UINT64_C(2971277250452558581), UINT64_C(10601102309909307404),
        UINT64_C(11272938509298435628), UINT64_C(13740190718371798985),
        UINT64_C(12972017961049866761), UINT64_C(12738557293009468166),
        UINT64_C(3183261253189695424), UINT64_C(3786868313083002318),
        UINT64_C(4890746599539190157), UINT64_C(2233326664694499281),
        UINT64_C(868025976122733904)
    };
    TestVP8ProbabilityContextCoverage coverage = {0};
    int has_full_coefficient_context_coverage = 1;
    size_t context_vector_index = 0u;

    for (size_t fixture_index = 0u;
         fixture_index < sizeof(fixture_names) / sizeof(fixture_names[0]);
         ++fixture_index) {
        uint8_t *bytes = NULL;
        size_t size = 0u;
        QwWebPDocument document = {0};
        QwWebPVP8Header header = {0};
        QwWebPVP8Macroblock *macroblocks = NULL;
        size_t macroblock_count = 0u;
        size_t columns;
        size_t rows;
        TestVP8CoefficientContextProbe probe = {0};
        size_t mode_contexts_before;
        int rc;

        if (!read_fixture(fixture_names[fixture_index], &bytes, &size)) {
            fprintf(stderr, "VP8 context fixture read failed: %s\n",
                    fixture_names[fixture_index]);
            goto fixture_fail;
        }
        rc = qw_webp_document_parse(bytes, size, &document);
        if (rc != QW_STATUS_OK || document.frame_count != 1u ||
            document.frames[0].codec != QW_WEBP_CODEC_VP8) {
            fprintf(stderr, "VP8 context fixture parse failed: %s (%d)\n",
                    fixture_names[fixture_index], rc);
            goto fixture_fail;
        }
        rc = qw_webp_vp8_parse_keyframe_header(
            document.frames[0].image_data, document.frames[0].image_size,
            document.width, document.height, &header);
        if (rc != QW_STATUS_OK) goto fixture_fail;
        rc = qw_webp_vp8_decode_macroblock_modes(
            document.frames[0].image_data, document.frames[0].image_size,
            &header, &macroblocks, &macroblock_count);
        if (rc != QW_STATUS_OK) goto fixture_fail;
        columns = ((size_t)document.width + 15u) / 16u;
        rows = ((size_t)document.height + 15u) / 16u;
        mode_contexts_before = coverage.b_mode_context_count;
        if (macroblock_count != columns * rows ||
            !test_vp8_collect_b_mode_contexts(
                macroblocks, columns, rows, &coverage) ||
            columns > SIZE_MAX / 9u) {
            fprintf(stderr, "VP8 mode-context inspection failed: %s\n",
                    fixture_names[fixture_index]);
            goto fixture_fail;
        }
        probe.macroblock_columns = columns;
        probe.token_partition_count = header.token_partition_count;
        probe.above_context = (uint8_t *)calloc(columns, 9u);
        if (!probe.above_context) goto fixture_fail;
        rc = qw_webp_vp8_decode_coefficients(
            document.frames[0].image_data, document.frames[0].image_size,
            &header, macroblocks, macroblock_count,
            test_vp8_probe_coefficient_contexts, &probe);
        if (rc != QW_STATUS_OK) {
            fprintf(stderr, "VP8 coefficient-context probe failed: %s (%d)\n",
                    fixture_names[fixture_index], rc);
            goto fixture_fail;
        }
        if (strncmp(fixture_names[fixture_index], "vp8_context_", 12u) == 0) {
            uint8_t *rgba = NULL;
            QwImage reference = {0};
            rc = qw_webp_vp8_decode_rgba(
                document.frames[0].image_data,
                document.frames[0].image_size,
                document.width, document.height, &rgba);
            if (rc != QW_STATUS_OK || !rgba ||
                qw_image_decode(bytes, size, &reference) != QW_IMAGE_OK ||
                reference.width != document.width ||
                reference.height != document.height ||
                reference.size != (size_t)document.width *
                                  document.height * 3u) {
                fprintf(stderr, "VP8 context pixel oracle setup failed: %s\n",
                        fixture_names[fixture_index]);
                free(rgba);
                qw_image_free(&reference);
                goto fixture_fail;
            }
            if (context_vector_index >=
                    sizeof(expected_context_rgb_checksums) /
                        sizeof(expected_context_rgb_checksums[0]) ||
                checksum(reference.pixels, reference.size) !=
                    expected_context_rgb_checksums[context_vector_index]) {
                fprintf(stderr, "VP8 context RGB golden failed: %s\n",
                        fixture_names[fixture_index]);
                free(rgba);
                qw_image_free(&reference);
                goto fixture_fail;
            }
            fprintf(stderr, "  %s pinned RGB checksum: %llu\n",
                    fixture_names[fixture_index],
                    (unsigned long long)expected_context_rgb_checksums[
                        context_vector_index]);
            for (size_t pixel = 0u;
                 pixel < (size_t)document.width * document.height; ++pixel) {
                for (size_t channel = 0u; channel < 3u; ++channel) {
                    if (rgba[pixel * 4u + channel] !=
                        reference.pixels[pixel * 3u + channel]) {
                        fprintf(stderr,
                                "VP8 context vector pixel mismatch: %s pixel=%zu channel=%zu\n",
                                fixture_names[fixture_index], pixel, channel);
                        free(rgba);
                        qw_image_free(&reference);
                        goto fixture_fail;
                    }
                }
            }
            ++coverage.exact_pixel_vectors;
            ++context_vector_index;
            free(rgba);
            qw_image_free(&reference);
        }
        for (size_t type = 0u; type < 4u; ++type) {
            for (size_t context = 0u; context < 3u; ++context) {
                coverage.token_context_counts[type][context] +=
                    probe.token_context_counts[type][context];
            }
        }
        if (strncmp(fixture_names[fixture_index], "vp8_context_", 12u) == 0) {
            fprintf(stderr, "  %s coefficient contexts:",
                    fixture_names[fixture_index]);
            for (size_t type = 0u; type < 4u; ++type) {
                unsigned mask = 0u;
                for (size_t context = 0u; context < 3u; ++context) {
                    if (probe.token_context_counts[type][context])
                        mask |= 1u << context;
                }
                fprintf(stderr, " type%zu=%x", type, mask);
            }
            fprintf(stderr, "; new B_PRED pairs=%zu\n",
                    coverage.b_mode_context_count - mode_contexts_before);
        }
        free(probe.above_context);
        free(macroblocks);
        qw_webp_document_clear(&document);
        free(bytes);
        continue;

fixture_fail:
        free(probe.above_context);
        free(macroblocks);
        qw_webp_document_clear(&document);
        free(bytes);
        return 0;
    }

    fprintf(stderr, "VP8 coefficient probability contexts:");
    for (size_t type = 0u; type < 4u; ++type) {
        unsigned mask = 0u;
        for (size_t context = 0u; context < 3u; ++context) {
            if (coverage.token_context_counts[type][context])
                mask |= 1u << context;
        }
        fprintf(stderr, " type%zu=%x", type, mask);
        if (mask != 7u) {
            fprintf(stderr, " (counts %zu,%zu,%zu)",
                    coverage.token_context_counts[type][0],
                    coverage.token_context_counts[type][1],
                    coverage.token_context_counts[type][2]);
            has_full_coefficient_context_coverage = 0;
        }
    }
    fprintf(stderr, "\nVP8 B_PRED above/left mode-probability contexts: %zu of 100 pairs\n",
            coverage.b_mode_context_count);
    if (coverage.b_mode_context_count < 100u) {
        fprintf(stderr, "  missing B_PRED pairs:");
        for (size_t above_mode = 0u; above_mode < 10u; ++above_mode) {
            for (size_t left_mode = 0u; left_mode < 10u; ++left_mode) {
                size_t pair_index = above_mode * 10u + left_mode;
                if (!(coverage.b_mode_context_mask[pair_index / 32u] &
                      (UINT32_C(1) << (pair_index % 32u)))) {
                    fprintf(stderr, " (%zu,%zu)", above_mode, left_mode);
                }
            }
        }
        fputc('\n', stderr);
    }
    fprintf(stderr, "VP8 context vectors matching pinned RGB goldens: %zu\n",
            coverage.exact_pixel_vectors);
    if (!has_full_coefficient_context_coverage ||
        coverage.b_mode_context_count < 98u ||
        coverage.exact_pixel_vectors != 17u || context_vector_index != 17u) {
        fprintf(stderr, "VP8 B_PRED probability-context coverage regressed\n");
        return 0;
    }
    return 1;
}

static int test_vp8_multimb_bpred_fixture(void)
{
    static const uint8_t expected_uv_modes[6] = {2u, 1u, 1u, 2u, 0u, 0u};
    static const uint8_t expected_b_modes[6][16] = {
        {0u, 9u, 3u, 0u, 0u, 0u, 0u, 9u, 1u, 7u, 2u, 4u, 0u, 0u, 0u, 7u},
        {0u, 4u, 4u, 4u, 1u, 9u, 0u, 4u, 4u, 4u, 0u, 0u, 6u, 3u, 0u, 9u},
        {4u, 0u, 0u, 0u, 7u, 1u, 0u, 0u, 9u, 7u, 0u, 0u, 5u, 0u, 0u, 2u},
        {6u, 6u, 7u, 0u, 5u, 0u, 0u, 0u, 2u, 0u, 9u, 0u, 2u, 0u, 2u, 0u},
        {8u, 2u, 0u, 9u, 0u, 0u, 0u, 9u, 0u, 9u, 0u, 0u, 0u, 4u, 0u, 7u},
        {3u, 0u, 0u, 0u, 3u, 5u, 0u, 0u, 0u, 3u, 0u, 0u, 0u, 0u, 0u, 8u}
    };
    uint8_t *bytes = NULL;
    uint8_t *reconstructed = NULL;
    size_t size = 0u;
    size_t macroblock_count = 0u;
    uint64_t total_error = 0u;
    uint64_t channel_error[3] = {0u, 0u, 0u};
    uint64_t internal_channel_sum[3] = {0u, 0u, 0u};
    uint64_t reference_channel_sum[3] = {0u, 0u, 0u};
    int64_t signed_channel_error[3] = {0, 0, 0};
    uint64_t macroblock_error[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    uint64_t target_block_error = 0u;
    size_t maximum_error = 0u;
    size_t maximum_error_pixel = 0u;
    size_t maximum_error_macroblock = 0u;
    size_t target_block_maximum_error = 0u;
    size_t non_gray_reference_pixels = 0u;
    TestWebPVP8CoeffCounts coefficient_counts = {0};
    QwWebPDocument document = {0};
    QwWebPVP8Header header = {0};
    QwWebPVP8Macroblock *macroblocks = NULL;
    QwImage reference = {0};
    int rc;

    if (!read_fixture("multi_mb_bpred.webp", &bytes, &size)) return 0;
    rc = qw_webp_document_parse(bytes, size, &document);
    if (rc != QW_STATUS_OK || document.frame_count != 1u ||
        document.frames[0].codec != QW_WEBP_CODEC_VP8) {
        fprintf(stderr, "multi-macroblock VP8 fixture parse failed (%d)\n", rc);
        goto fail;
    }
    rc = qw_webp_vp8_parse_keyframe_header(
        document.frames[0].image_data, document.frames[0].image_size,
        document.width, document.height, &header);
    if (rc == QW_STATUS_OK) {
        rc = qw_webp_vp8_decode_macroblock_modes(
            document.frames[0].image_data, document.frames[0].image_size,
            &header, &macroblocks, &macroblock_count);
    }
    if (rc != QW_STATUS_OK || document.width != 48u ||
        document.height != 32u || macroblock_count != 6u || !macroblocks) {
        fprintf(stderr, "multi-macroblock VP8 mode parse failed (%d, %zu)\n",
                rc, macroblock_count);
        goto fail;
    }
    for (size_t mb = 0u; mb < macroblock_count; ++mb) {
        if (macroblocks[mb].y_mode != 4u ||
            macroblocks[mb].uv_mode != expected_uv_modes[mb]) {
            fprintf(stderr,
                    "multi-macroblock VP8 mode mismatch at MB %zu (y=%u uv=%u expected y=4 uv=%u)\n",
                    mb, macroblocks[mb].y_mode, macroblocks[mb].uv_mode,
                    expected_uv_modes[mb]);
            goto fail;
        }
    }
    {
        unsigned b_mode_mask = 0u;
        for (size_t mb = 0u; mb < macroblock_count; ++mb) {
            if (memcmp(macroblocks[mb].b_mode, expected_b_modes[mb],
                       sizeof(expected_b_modes[mb])) != 0) {
                fprintf(stderr,
                        "multi-macroblock VP8 B_PRED subblock-mode golden failed at MB %zu\n",
                        mb);
                goto fail;
            }
            for (size_t block = 0u; block < 16u; ++block)
                b_mode_mask |= 1u << macroblocks[mb].b_mode[block];
        }
        if (b_mode_mask != ((1u << 10u) - 1u)) {
            fprintf(stderr,
                    "multi-macroblock VP8 B_PRED subblock modes do not cover IDs 0..9 (%03x)\n",
                    b_mode_mask);
            goto fail;
        }
    }
    fprintf(stderr,
            "multi-macroblock VP8 B_PRED subblock-mode golden: 96 entries cover IDs 0..9\n");
    fprintf(stderr,
            "multi-macroblock VP8 header: base_q=%u token_partitions=%u segmentation=%u feature_mode=%u qdelta={%d,%d,%d,%d,%d} uv_prob={%u,%u,%u}\n",
            header.base_quantizer, header.token_partition_count,
            header.segmentation_enabled, header.segmentation_feature_mode,
            header.quantizer_delta[0], header.quantizer_delta[1],
            header.quantizer_delta[2], header.quantizer_delta[3],
            header.quantizer_delta[4],
            header.coefficient_probabilities[2][0][0][0],
            header.coefficient_probabilities[2][0][1][0],
            header.coefficient_probabilities[2][0][2][0]);
    {
        size_t changed_probabilities = 0u;
        for (size_t type = 0u; type < 4u; ++type) {
            for (size_t band = 0u; band < 8u; ++band) {
                for (size_t context = 0u; context < 3u; ++context) {
                    for (size_t token = 0u; token < 11u; ++token) {
                        if (header.coefficient_probabilities[type][band][context][token] !=
                            qw_webp_vp8_coeff_default_probability[type][band][context][token]) {
                            ++changed_probabilities;
                            fprintf(stderr,
                                    "  coeff_prob[%zu][%zu][%zu][%zu]=%u (default %u)\n",
                                    type, band, context, token,
                                    header.coefficient_probabilities[type][band][context][token],
                                    qw_webp_vp8_coeff_default_probability[type][band][context][token]);
                        }
                    }
                }
            }
        }
        fprintf(stderr, "multi-macroblock VP8 coefficient probability updates: %zu\n",
                changed_probabilities);
    }
    coefficient_counts.coefficient_checksum = UINT64_C(1469598103934665603);
    for (size_t mb = 0u; mb < macroblock_count; ++mb) {
        fprintf(stderr, "  MB %zu mode={y:%u uv:%u skip:%u seg:%u}\n", mb,
                macroblocks[mb].y_mode, macroblocks[mb].uv_mode,
                macroblocks[mb].skip_coeff, macroblocks[mb].segment_id);
    }
    rc = qw_webp_vp8_decode_coefficients(
        document.frames[0].image_data, document.frames[0].image_size,
        &header, macroblocks, macroblock_count,
        test_vp8_count_coefficients, &coefficient_counts);
    if (rc != QW_STATUS_OK) goto fail;
    /* FNV-1a over decoded coefficients in MB/block order as little-endian int16. */
    if (coefficient_counts.coefficient_checksum !=
        UINT64_C(12265257313731425460)) {
        fprintf(stderr, "multi-macroblock VP8 decoded coefficient golden failed\n");
        goto fail;
    }
    fprintf(stderr, "  decoded coefficient checksum: %llu\n",
            (unsigned long long)coefficient_counts.coefficient_checksum);
    for (size_t mb = 0u; mb < macroblock_count; ++mb) {
        fprintf(stderr, "  MB %zu nonzero tokens={Y:%zu U:%zu V:%zu Y2:%zu}\n",
                mb, coefficient_counts.nonzero_coefficients[mb][0],
                coefficient_counts.nonzero_coefficients[mb][1],
                coefficient_counts.nonzero_coefficients[mb][2],
                coefficient_counts.nonzero_coefficients[mb][3]);
    }
    fprintf(stderr, "  MB 0 coefficient counts by block:");
    for (size_t block = 0u; block < 25u; ++block) {
        fprintf(stderr, " %zu:%zu", block,
                coefficient_counts.nonzero_per_block[0][block]);
    }
    fputc('\n', stderr);
    fprintf(stderr, "  MB 0 Y2 coefficients:");
    for (size_t coefficient = 0u; coefficient < 16u; ++coefficient) {
        int16_t value = coefficient_counts.first_macroblock_coefficients
            [24u][coefficient];
        if (value != 0)
            fprintf(stderr, " %zu:%d", coefficient, (int)value);
    }
    fputc('\n', stderr);
    rc = qw_webp_vp8_decode_rgba(document.frames[0].image_data,
                                 document.frames[0].image_size,
                                 document.width, document.height,
                                 &reconstructed);
    if (rc != QW_STATUS_OK || !reconstructed) goto fail;
    /* Matches libwebp 1.6.0 dwebp's default fancy chroma upsampling. */
    if (checksum(reconstructed, 48u * 32u * 4u) !=
        UINT64_C(17026864780688414871)) {
        fprintf(stderr, "multi-macroblock VP8 RGBA golden failed\n");
        goto fail;
    }
    rc = qw_image_decode(bytes, size, &reference);
    if (rc != QW_IMAGE_OK || reference.width != 48u ||
        reference.height != 32u || reference.size !=
            (size_t)document.width * document.height * 3u ||
        !reference.pixels ||
        checksum(reference.pixels, reference.size) !=
            UINT64_C(16066714284342509845)) {
        fprintf(stderr, "multi-macroblock WebP reference golden failed (%d)\n",
                rc);
        goto fail;
    }
    for (size_t y = 0u; y < document.height; ++y) {
        for (size_t x = 0u; x < document.width; ++x) {
            size_t pixel = y * (size_t)document.width + x;
            size_t macroblock = (y / 16u) * 3u + x / 16u;
            if (reference.pixels[pixel * 3u] != reference.pixels[pixel * 3u + 1u] ||
                reference.pixels[pixel * 3u + 1u] != reference.pixels[pixel * 3u + 2u]) {
                ++non_gray_reference_pixels;
            }
            for (size_t channel = 0u; channel < 3u; ++channel) {
                size_t reference_offset = pixel * 3u + channel;
                size_t internal_offset = pixel * 4u + channel;
                int difference = (int)reconstructed[internal_offset] -
                                 (int)reference.pixels[reference_offset];
                size_t error = (size_t)(difference < 0 ? -difference : difference);
                total_error += error;
                channel_error[channel] += error;
                internal_channel_sum[channel] += reconstructed[internal_offset];
                reference_channel_sum[channel] += reference.pixels[reference_offset];
                signed_channel_error[channel] += difference;
                macroblock_error[macroblock] += error;
                if (error > maximum_error) {
                    maximum_error = error;
                    maximum_error_pixel = pixel;
                    maximum_error_macroblock = macroblock;
                }
                if (x >= 20u && x < 24u && y >= 28u && y < 32u) {
                    target_block_error += error;
                    if (error > target_block_maximum_error)
                        target_block_maximum_error = error;
                }
            }
        }
    }
    fprintf(stderr,
            "multi-macroblock VP8 reconstruction diagnostic: max=%zu at pixel %zu (MB %zu), mean=%.3f, abs RGB means={%.3f,%.3f,%.3f}, internal RGB means={%.3f,%.3f,%.3f}, reference RGB means={%.3f,%.3f,%.3f}, signed means={%.3f,%.3f,%.3f}, non-gray reference pixels=%zu; B_LD block max=%zu mean=%.3f\n",
            maximum_error, maximum_error_pixel, maximum_error_macroblock,
            (double)total_error / (48.0 * 32.0 * 3.0),
            (double)channel_error[0] / (48.0 * 32.0),
            (double)channel_error[1] / (48.0 * 32.0),
            (double)channel_error[2] / (48.0 * 32.0),
            (double)internal_channel_sum[0] / (48.0 * 32.0),
            (double)internal_channel_sum[1] / (48.0 * 32.0),
            (double)internal_channel_sum[2] / (48.0 * 32.0),
            (double)reference_channel_sum[0] / (48.0 * 32.0),
            (double)reference_channel_sum[1] / (48.0 * 32.0),
            (double)reference_channel_sum[2] / (48.0 * 32.0),
            (double)signed_channel_error[0] / (48.0 * 32.0),
            (double)signed_channel_error[1] / (48.0 * 32.0),
            (double)signed_channel_error[2] / (48.0 * 32.0),
            non_gray_reference_pixels,
            target_block_maximum_error,
            (double)target_block_error / (4.0 * 4.0 * 3.0));
    for (size_t macroblock = 0u; macroblock < 6u; ++macroblock) {
        fprintf(stderr, "  MB %zu mean RGB abs error=%.3f\n", macroblock,
                (double)macroblock_error[macroblock] / (16.0 * 16.0 * 3.0));
    }
    for (size_t i = 0u; i <
         (size_t)document.width * document.height; ++i) {
        if (reconstructed[i * 4u + 3u] != 255u) {
            fprintf(stderr, "multi-macroblock VP8 alpha mismatch at %zu\n", i);
            goto fail;
        }
    }
    qw_image_free(&reference);
    free(reconstructed);
    free(macroblocks);
    qw_webp_document_clear(&document);
    free(bytes);
    return 1;

fail:
    qw_image_free(&reference);
    free(reconstructed);
    free(macroblocks);
    qw_webp_document_clear(&document);
    free(bytes);
    return 0;
}
static int test_vp8_odd_dimensions(void)
{
    uint8_t *bytes = NULL;
    uint8_t *reconstructed = NULL;
    size_t size = 0u;
    size_t pixels;
    size_t maximum_error = 0u;
    uint64_t total_error = 0u;
    QwWebPDocument document = {0};
    QwWebPVP8Header header = {0};
    QwImage reference = {0};
    int rc;

    if (!read_fixture("odd_vp8_31x17.webp", &bytes, &size)) return 0;
    rc = qw_webp_document_parse(bytes, size, &document);
    if (rc != QW_STATUS_OK || document.frame_count != 1u ||
        document.frames[0].codec != QW_WEBP_CODEC_VP8 ||
        document.width != 31u || document.height != 17u ||
        document.frames[0].has_alpha || (document.width & 1u) == 0u ||
        (document.height & 1u) == 0u) {
        fprintf(stderr, "odd-dimension VP8 metadata check failed (%d)\n", rc);
        goto fail;
    }
    rc = qw_webp_vp8_parse_keyframe_header(
        document.frames[0].image_data, document.frames[0].image_size,
        document.width, document.height, &header);
    if (rc != QW_STATUS_OK || header.loop_filter_level != 0u) {
        fprintf(stderr, "odd-dimension VP8 header check failed (%d, filter=%u)\n",
                rc, header.loop_filter_level);
        goto fail;
    }
    rc = qw_webp_vp8_decode_rgba(document.frames[0].image_data,
                                 document.frames[0].image_size,
                                 document.width, document.height,
                                 &reconstructed);
    if (rc != QW_STATUS_OK || !reconstructed) {
        fprintf(stderr, "odd-dimension internal VP8 reconstruction failed (%d)\n", rc);
        goto fail;
    }
    rc = qw_image_decode(bytes, size, &reference);
    pixels = (size_t)document.width * document.height;
    if (rc != QW_IMAGE_OK || reference.width != 31u ||
        reference.height != 17u || reference.format != QW_IMAGE_PIXEL_RGB24 ||
        reference.size != pixels * 3u || !reference.pixels) {
        fprintf(stderr, "odd-dimension VP8 reference decode failed (%d)\n", rc);
        goto fail;
    }
    for (size_t i = 0u; i < pixels; ++i) {
        if (reconstructed[i * 4u + 3u] != 255u) {
            fprintf(stderr, "odd-dimension VP8 alpha mismatch at pixel %zu\n", i);
            goto fail;
        }
        for (size_t channel = 0u; channel < 3u; ++channel) {
            int difference = (int)reconstructed[i * 4u + channel] -
                             (int)reference.pixels[i * 3u + channel];
            size_t error = (size_t)(difference < 0 ? -difference : difference);
            if (error > maximum_error) maximum_error = error;
            total_error += error;
        }
    }
    printf("odd-dimension VP8: %ux%u, max RGB error=%zu, mean RGB error=%.3f, RGBA checksum=%llu, public RGB checksum=%llu\n",
           document.width, document.height, maximum_error,
           (double)total_error / (double)(pixels * 3u),
           (unsigned long long)checksum(reconstructed, pixels * 4u),
           (unsigned long long)checksum(reference.pixels, reference.size));
    if (maximum_error != 0u ||
        checksum(reconstructed, pixels * 4u) !=
            UINT64_C(18407873696181568052) ||
        checksum(reference.pixels, reference.size) !=
            UINT64_C(11495621268615560771)) {
        fprintf(stderr, "odd-dimension VP8 output golden failed\n");
        goto fail;
    }
    qw_image_free(&reference);
    qw_webp_document_clear(&document);
    free(reconstructed);
    free(bytes);
    return 1;

fail:
    qw_image_free(&reference);
    qw_webp_document_clear(&document);
    free(reconstructed);
    free(bytes);
    return 0;
}

static int test_vp8_nonzero_loop_filter(void)
{
    uint8_t *bytes = NULL;
    uint8_t *reconstructed = NULL;
    size_t size = 0u;
    size_t macroblock_count = 0u;
    size_t maximum_error = 0u;
    uint64_t total_error = 0u;
    QwWebPDocument document = {0};
    QwWebPVP8Header header = {0};
    QwWebPVP8Macroblock *macroblocks = NULL;
    QwImage reference = {0};
    int rc;

    if (!read_fixture("vp8_filter_level27_31x17.webp", &bytes, &size)) return 0;
    rc = qw_webp_document_parse(bytes, size, &document);
    if (rc != QW_STATUS_OK || document.frame_count != 1u ||
        document.frames[0].codec != QW_WEBP_CODEC_VP8 ||
        document.width != 31u || document.height != 17u ||
        document.frames[0].has_alpha) {
        fprintf(stderr, "nonzero-filter VP8 metadata check failed (%d)\n", rc);
        goto fail;
    }
    rc = qw_webp_vp8_parse_keyframe_header(
        document.frames[0].image_data, document.frames[0].image_size,
        document.width, document.height, &header);
    if (rc != QW_STATUS_OK || header.loop_filter_type != 0u ||
        header.loop_filter_level != 27u) {
        fprintf(stderr,
                "nonzero-filter VP8 header check failed (%d, type=%u, level=%u)\n",
                rc, header.loop_filter_type, header.loop_filter_level);
        goto fail;
    }
    rc = qw_webp_vp8_decode_macroblock_modes(
        document.frames[0].image_data, document.frames[0].image_size,
        &header, &macroblocks, &macroblock_count);
    if (rc != QW_STATUS_OK || macroblock_count != 4u || !macroblocks) {
        fprintf(stderr, "nonzero-filter VP8 mode decode failed (%d, %zu)\n",
                rc, macroblock_count);
        goto fail;
    }
    for (size_t i = 0u; i < macroblock_count; ++i) {
        if (macroblocks[i].y_mode != 4u) {
            fprintf(stderr, "nonzero-filter VP8 expected B_PRED at MB %zu\n", i);
            goto fail;
        }
    }
    rc = qw_webp_vp8_decode_rgba(document.frames[0].image_data,
                                 document.frames[0].image_size,
                                 document.width, document.height,
                                 &reconstructed);
    if (rc != QW_STATUS_OK || !reconstructed) {
        fprintf(stderr, "nonzero-filter internal VP8 reconstruction failed (%d)\n",
                rc);
        goto fail;
    }
    rc = qw_image_decode(bytes, size, &reference);
    if (rc != QW_IMAGE_OK || reference.width != 31u ||
        reference.height != 17u || reference.format != QW_IMAGE_PIXEL_RGB24 ||
        reference.size != 31u * 17u * 3u || !reference.pixels) {
        fprintf(stderr, "nonzero-filter VP8 reference decode failed (%d)\n", rc);
        goto fail;
    }
    for (size_t i = 0u; i < 31u * 17u; ++i) {
        if (reconstructed[i * 4u + 3u] != 255u) {
            fprintf(stderr, "nonzero-filter VP8 alpha mismatch at pixel %zu\n", i);
            goto fail;
        }
        for (size_t channel = 0u; channel < 3u; ++channel) {
            int difference = (int)reconstructed[i * 4u + channel] -
                             (int)reference.pixels[i * 3u + channel];
            size_t error = (size_t)(difference < 0 ? -difference : difference);
            if (error > maximum_error) maximum_error = error;
            total_error += error;
        }
    }
    printf("nonzero-filter VP8: %ux%u, type=%u level=%u, max RGB error=%zu, mean RGB error=%.3f, RGBA checksum=%llu, public RGB checksum=%llu\n",
           document.width, document.height, header.loop_filter_type,
           header.loop_filter_level, maximum_error,
           (double)total_error / (31.0 * 17.0 * 3.0),
           (unsigned long long)checksum(reconstructed, 31u * 17u * 4u),
           (unsigned long long)checksum(reference.pixels, reference.size));
    if (maximum_error != 0u ||
        checksum(reconstructed, 31u * 17u * 4u) !=
            UINT64_C(9053251504440866157) ||
        checksum(reference.pixels, reference.size) !=
            UINT64_C(12435336577776293184)) {
        fprintf(stderr, "nonzero-filter VP8 output golden failed\n");
        goto fail;
    }
    qw_image_free(&reference);
    qw_webp_document_clear(&document);
    free(reconstructed);
    free(macroblocks);
    free(bytes);
    return 1;

fail:
    qw_image_free(&reference);
    qw_webp_document_clear(&document);
    free(reconstructed);
    free(macroblocks);
    free(bytes);
    return 0;
}

typedef struct TestVP8CoverageGolden {
    const char *fixture_name;
    uint8_t segmentation_enabled;
    uint8_t segmentation_map_updated;
    uint8_t segmentation_data_updated;
    uint8_t segmentation_feature_mode;
    uint8_t base_quantizer;
    int8_t quantizer_delta[5];
    int8_t segment_quantizer_delta[4];
    int8_t segment_filter_delta[4];
    uint8_t macroblock_segments[4];
    uint8_t macroblock_y_modes[4];
    uint8_t loop_filter_type;
    uint8_t loop_filter_level;
    uint8_t loop_filter_sharpness;
    uint8_t loop_filter_delta_enabled;
    int8_t reference_filter_delta[4];
    int8_t mode_filter_delta[4];
    uint8_t token_partition_count;
    uint64_t rgba_checksum;
    uint64_t public_rgb_checksum;
} TestVP8CoverageGolden;

static int test_vp8_coverage_vector(const TestVP8CoverageGolden *golden)
{
    uint8_t *bytes = NULL;
    uint8_t *reconstructed = NULL;
    size_t size = 0u;
    size_t macroblock_count = 0u;
    size_t maximum_error = 0u;
    uint64_t total_error = 0u;
    QwWebPDocument document = {0};
    QwWebPVP8Header header = {0};
    QwWebPVP8Macroblock *macroblocks = NULL;
    QwImage reference = {0};
    const size_t pixels = 31u * 17u;
    int rc;

    if (!golden || !golden->fixture_name ||
        !read_fixture(golden->fixture_name, &bytes, &size)) return 0;
    rc = qw_webp_document_parse(bytes, size, &document);
    if (rc != QW_STATUS_OK || document.frame_count != 1u ||
        document.frames[0].codec != QW_WEBP_CODEC_VP8 ||
        document.width != 31u || document.height != 17u ||
        document.frames[0].has_alpha) {
        fprintf(stderr, "VP8 coverage metadata check failed for %s (%d)\n",
                golden->fixture_name, rc);
        goto fail;
    }
    rc = qw_webp_vp8_parse_keyframe_header(
        document.frames[0].image_data, document.frames[0].image_size,
        document.width, document.height, &header);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "VP8 coverage header check failed for %s (%d)\n",
                golden->fixture_name, rc);
        goto fail;
    }
    if (header.segmentation_enabled != golden->segmentation_enabled ||
        header.segmentation_map_updated != golden->segmentation_map_updated ||
        header.segmentation_data_updated != golden->segmentation_data_updated ||
        header.segmentation_feature_mode != golden->segmentation_feature_mode ||
        header.base_quantizer != golden->base_quantizer ||
        memcmp(header.quantizer_delta, golden->quantizer_delta,
               sizeof(golden->quantizer_delta)) != 0 ||
        memcmp(header.segment_quantizer_delta, golden->segment_quantizer_delta,
               sizeof(golden->segment_quantizer_delta)) != 0 ||
        memcmp(header.segment_filter_delta, golden->segment_filter_delta,
               sizeof(golden->segment_filter_delta)) != 0 ||
        header.loop_filter_type != golden->loop_filter_type ||
        header.loop_filter_level != golden->loop_filter_level ||
        header.loop_filter_sharpness != golden->loop_filter_sharpness ||
        header.loop_filter_delta_enabled != golden->loop_filter_delta_enabled ||
        memcmp(header.reference_filter_delta, golden->reference_filter_delta,
               sizeof(golden->reference_filter_delta)) != 0 ||
        memcmp(header.mode_filter_delta, golden->mode_filter_delta,
               sizeof(golden->mode_filter_delta)) != 0 ||
        header.token_partition_count != golden->token_partition_count) {
        fprintf(stderr,
                "VP8 coverage header golden failed for %s: segmentation=%u map=%u data=%u mode=%u base_q=%u qdelta={%d,%d,%d,%d,%d} segment_q={%d,%d,%d,%d} segment_filter={%d,%d,%d,%d} filter={%u,%u,%u,%u} ref_delta={%d,%d,%d,%d} mode_delta={%d,%d,%d,%d} partitions=%u\n",
                golden->fixture_name, header.segmentation_enabled,
                header.segmentation_map_updated,
                header.segmentation_data_updated,
                header.segmentation_feature_mode, header.base_quantizer,
                header.quantizer_delta[0], header.quantizer_delta[1],
                header.quantizer_delta[2], header.quantizer_delta[3],
                header.quantizer_delta[4], header.segment_quantizer_delta[0],
                header.segment_quantizer_delta[1],
                header.segment_quantizer_delta[2],
                header.segment_quantizer_delta[3], header.segment_filter_delta[0],
                header.segment_filter_delta[1], header.segment_filter_delta[2],
                header.segment_filter_delta[3], header.loop_filter_type,
                header.loop_filter_level, header.loop_filter_sharpness,
                header.loop_filter_delta_enabled,
                header.reference_filter_delta[0],
                header.reference_filter_delta[1],
                header.reference_filter_delta[2],
                header.reference_filter_delta[3],
                header.mode_filter_delta[0], header.mode_filter_delta[1],
                header.mode_filter_delta[2], header.mode_filter_delta[3],
                header.token_partition_count);
        goto fail;
    }
    rc = qw_webp_vp8_decode_macroblock_modes(
        document.frames[0].image_data, document.frames[0].image_size,
        &header, &macroblocks, &macroblock_count);
    if (rc != QW_STATUS_OK || macroblock_count != 4u || !macroblocks) {
        fprintf(stderr, "VP8 coverage mode decode failed for %s (%d, %zu)\n",
                golden->fixture_name, rc, macroblock_count);
        goto fail;
    }
    for (size_t i = 0u; i < macroblock_count; ++i) {
        if (macroblocks[i].y_mode != golden->macroblock_y_modes[i]) {
            fprintf(stderr,
                    "VP8 coverage y-mode golden failed for %s: expected={%u,%u,%u,%u} actual={%u,%u,%u,%u}\n",
                    golden->fixture_name,
                    (unsigned)golden->macroblock_y_modes[0],
                    (unsigned)golden->macroblock_y_modes[1],
                    (unsigned)golden->macroblock_y_modes[2],
                    (unsigned)golden->macroblock_y_modes[3],
                    (unsigned)macroblocks[0].y_mode,
                    (unsigned)macroblocks[1].y_mode,
                    (unsigned)macroblocks[2].y_mode,
                    (unsigned)macroblocks[3].y_mode);
            goto fail;
        }
        if (macroblocks[i].segment_id != golden->macroblock_segments[i]) {
            fprintf(stderr, "VP8 coverage segment golden failed for %s MB %zu\n",
                    golden->fixture_name, i);
            goto fail;
        }
    }
    rc = qw_webp_vp8_decode_rgba(document.frames[0].image_data,
                                 document.frames[0].image_size,
                                 document.width, document.height,
                                 &reconstructed);
    if (rc != QW_STATUS_OK || !reconstructed) {
        fprintf(stderr, "VP8 coverage internal decode failed for %s (%d)\n",
                golden->fixture_name, rc);
        goto fail;
    }
    rc = qw_image_decode(bytes, size, &reference);
    if (rc != QW_IMAGE_OK || reference.width != document.width ||
        reference.height != document.height ||
        reference.format != QW_IMAGE_PIXEL_RGB24 ||
        reference.size != pixels * 3u || !reference.pixels) {
        fprintf(stderr, "VP8 coverage reference decode failed for %s (%d)\n",
                golden->fixture_name, rc);
        goto fail;
    }
    for (size_t i = 0u; i < pixels; ++i) {
        if (reconstructed[i * 4u + 3u] != 255u) {
            fprintf(stderr, "VP8 coverage alpha mismatch in %s at %zu\n",
                    golden->fixture_name, i);
            goto fail;
        }
        for (size_t channel = 0u; channel < 3u; ++channel) {
            int difference = (int)reconstructed[i * 4u + channel] -
                             (int)reference.pixels[i * 3u + channel];
            size_t error = (size_t)(difference < 0 ? -difference : difference);
            if (error > maximum_error) maximum_error = error;
            total_error += error;
        }
    }
    printf("VP8 coverage %s: segmentation=%u map=%u data=%u mode=%u "
           "base_q=%u qdelta={%d,%d,%d,%d,%d} segment_q={%d,%d,%d,%d} "
           "segment_filter={%d,%d,%d,%d} mb_segments={%u,%u,%u,%u} "
           "y_modes={%u,%u,%u,%u} "
           "filter_type=%u level=%u sharpness=%u lf_delta=%u "
           "ref_delta={%d,%d,%d,%d} mode_delta={%d,%d,%d,%d} "
           "partitions=%u max RGB error=%zu "
           "mean RGB error=%.3f RGBA checksum=%llu public RGB checksum=%llu\n",
           golden->fixture_name, header.segmentation_enabled,
           header.segmentation_map_updated, header.segmentation_data_updated,
           header.segmentation_feature_mode, header.base_quantizer,
           header.quantizer_delta[0], header.quantizer_delta[1],
           header.quantizer_delta[2], header.quantizer_delta[3],
           header.quantizer_delta[4], header.segment_quantizer_delta[0],
           header.segment_quantizer_delta[1], header.segment_quantizer_delta[2],
           header.segment_quantizer_delta[3], header.segment_filter_delta[0],
           header.segment_filter_delta[1], header.segment_filter_delta[2],
           header.segment_filter_delta[3], macroblocks[0].segment_id,
           macroblocks[1].segment_id, macroblocks[2].segment_id,
           macroblocks[3].segment_id, (unsigned)macroblocks[0].y_mode,
           (unsigned)macroblocks[1].y_mode, (unsigned)macroblocks[2].y_mode,
           (unsigned)macroblocks[3].y_mode, header.loop_filter_type,
           header.loop_filter_level, header.loop_filter_sharpness,
           header.loop_filter_delta_enabled,
           header.reference_filter_delta[0], header.reference_filter_delta[1],
           header.reference_filter_delta[2], header.reference_filter_delta[3],
           header.mode_filter_delta[0], header.mode_filter_delta[1],
           header.mode_filter_delta[2], header.mode_filter_delta[3],
           header.token_partition_count,
           maximum_error, (double)total_error / (double)(pixels * 3u),
           (unsigned long long)checksum(reconstructed, pixels * 4u),
           (unsigned long long)checksum(reference.pixels, reference.size));
    if (maximum_error != 0u ||
        checksum(reconstructed, pixels * 4u) != golden->rgba_checksum ||
        checksum(reference.pixels, reference.size) !=
            golden->public_rgb_checksum) {
        fprintf(stderr, "VP8 coverage output golden failed for %s\n",
                golden->fixture_name);
        goto fail;
    }
    qw_image_free(&reference);
    qw_webp_document_clear(&document);
    free(reconstructed);
    free(macroblocks);
    free(bytes);
    return 1;

fail:
    qw_image_free(&reference);
    qw_webp_document_clear(&document);
    free(reconstructed);
    free(macroblocks);
    free(bytes);
    return 0;
}

static int test_vp8_coverage_vectors(void)
{
    static const TestVP8CoverageGolden vectors[] = {
        {
            "vp8_quantizer_q5_31x17.webp", 0u, 0u, 0u, 0u, 86u,
            {0, 0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
            {0u, 0u, 0u, 0u}, {4u, 4u, 4u, 4u}, 0u, 0u, 0u, 0u,
            {0, 0, 0, 0}, {0, 0, 0, 0}, 1u,
            UINT64_C(2737824953850109429),
            UINT64_C(5990651332064846346)
        },
        {
            "vp8_segmentation_q70_31x17.webp", 1u, 1u, 1u, 1u, 48u,
            {0, 0, 0, -4, 6}, {48, 34, 23, 3}, {20, 10, 5, 0},
            {0u, 0u, 3u, 3u}, {4u, 4u, 4u, 4u}, 0u, 20u, 0u, 0u,
            {0, 0, 0, 0}, {0, 0, 0, 0}, 1u,
            UINT64_C(9565816437008354788),
            UINT64_C(14406943485765067399)
        },
        {
            "vp8_segmentation_delta_q70_31x17.webp", 1u, 1u, 1u, 0u, 48u,
            {0, 0, 0, -4, 6}, {0, -14, -25, -45}, {0, -10, -15, -20},
            {0u, 0u, 3u, 3u}, {4u, 4u, 4u, 4u}, 0u, 20u, 0u, 0u,
            {0, 0, 0, 0}, {0, 0, 0, 0}, 1u,
            UINT64_C(9565816437008354788),
            UINT64_C(14406943485765067399)
        },
        {
            "vp8_simple_filter_q55_31x17.webp", 1u, 1u, 1u, 1u, 55u,
            {0, 0, 0, -3, 6}, {55, 41, 30, 11}, {21, 11, 7, 2},
            {0u, 0u, 3u, 3u}, {4u, 4u, 4u, 4u}, 1u, 21u, 0u, 0u,
            {0, 0, 0, 0}, {0, 0, 0, 0}, 1u,
            UINT64_C(3419381511653061459),
            UINT64_C(247891350788762726)
        },
        {
            "vp8_filter_sharpness7_q70_31x17.webp", 1u, 1u, 1u, 1u, 48u,
            {0, 0, 0, -4, 6}, {48, 34, 23, 3}, {20, 10, 5, 0},
            {0u, 0u, 3u, 3u}, {4u, 4u, 4u, 4u}, 0u, 20u, 7u, 0u,
            {0, 0, 0, 0}, {0, 0, 0, 0}, 1u,
            UINT64_C(2793395324913313161),
            UINT64_C(14911510901652674150)
        },
        {
            "vp8_loop_filter_delta_q70_31x17.webp", 1u, 1u, 1u, 1u, 48u,
            {0, 0, 0, -4, 6}, {48, 34, 23, 3}, {20, 10, 5, 0},
            {0u, 0u, 3u, 3u}, {4u, 4u, 4u, 4u}, 0u, 20u, 0u, 1u,
            {-4, 0, 0, 0}, {12, 0, 0, 0}, 1u,
            UINT64_C(3176877635953420243),
            UINT64_C(15655573039128738070)
        },
        {
            "vp8_filter_level15_q70_31x17.webp", 0u, 0u, 0u, 0u, 28u,
            {0, 0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
            {0u, 0u, 0u, 0u}, {4u, 4u, 4u, 4u}, 0u, 15u, 0u, 0u,
            {0, 0, 0, 0}, {0, 0, 0, 0}, 1u,
            UINT64_C(2570188311262057201),
            UINT64_C(17703928071573273120)
        },
        {
            "vp8_filter_strength100_q127_31x17.webp", 0u, 0u, 0u, 0u, 127u,
            {0, 0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
            {0u, 0u, 0u, 0u}, {0u, 1u, 0u, 4u}, 0u, 63u, 0u, 0u,
            {0, 0, 0, 0}, {0, 0, 0, 0}, 1u,
            UINT64_C(1863813111785769350),
            UINT64_C(17986317700632372173)
        },
        {
            "vp8_prediction_yramp_q70_31x17.webp", 0u, 0u, 0u, 0u, 28u,
            {0, 0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
            {0u, 0u, 0u, 0u}, {4u, 3u, 0u, 3u}, 0u, 0u, 0u, 0u,
            {0, 0, 0, 0}, {0, 0, 0, 0}, 1u,
            UINT64_C(6607089990693730189),
            UINT64_C(11374254562819007934)
        },
        {
            "vp8_prediction_xramp_q70_31x17.webp", 0u, 0u, 0u, 0u, 28u,
            {0, 0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
            {0u, 0u, 0u, 0u}, {4u, 4u, 1u, 4u}, 0u, 0u, 0u, 0u,
            {0, 0, 0, 0}, {0, 0, 0, 0}, 1u,
            UINT64_C(8558977961149033210),
            UINT64_C(17354824860435175973)
        },
        {
            "vp8_prediction_diagonal_q70_31x17.webp", 0u, 0u, 0u, 0u, 28u,
            {0, 0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
            {0u, 0u, 0u, 0u}, {4u, 4u, 1u, 3u}, 0u, 0u, 0u, 0u,
            {0, 0, 0, 0}, {0, 0, 0, 0}, 1u,
            UINT64_C(17551270454603076868),
            UINT64_C(18256653708610392587)
        },
        {
            "vp8_quantizer_q0_31x17.webp", 0u, 0u, 0u, 0u, 0u,
            {0, 0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
            {0u, 0u, 0u, 0u}, {4u, 4u, 4u, 4u}, 0u, 0u, 0u, 0u,
            {0, 0, 0, 0}, {0, 0, 0, 0}, 1u,
            UINT64_C(9077694782508346185),
            UINT64_C(15444632536717323742)
        },
        {
            "vp8_quantizer_q127_31x17.webp", 0u, 0u, 0u, 0u, 127u,
            {0, 0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
            {0u, 0u, 0u, 0u}, {0u, 1u, 0u, 4u}, 0u, 0u, 0u, 0u,
            {0, 0, 0, 0}, {0, 0, 0, 0}, 1u,
            UINT64_C(17456644456919064160),
            UINT64_C(5088848010108501383)
        }
    };

    for (size_t i = 0u; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        if (!test_vp8_coverage_vector(&vectors[i])) return 0;
    }
    return 1;
}

static int test_vp8_multipartition_fixture(void)
{
    uint8_t *bytes = NULL;
    uint8_t *reconstructed = NULL;
    size_t size = 0u;
    size_t macroblock_count = 0u;
    size_t partition_count = 0u;
    size_t maximum_error = 0u;
    uint64_t total_error = 0u;
    QwWebPDocument document = {0};
    QwWebPVP8Header header = {0};
    QwWebPVP8Macroblock *macroblocks = NULL;
    QwWebPVP8TokenPartition partitions[8] = {{0}};
    QwImage reference = {0};
    const size_t pixels = 128u * 128u;
    static const size_t expected_partition_sizes[4] = {
        1963u, 1859u, 1898u, 1923u
    };
    int rc;

    if (!read_fixture("vp8_multipartition_p2_128x128.webp", &bytes, &size))
        return 0;
    rc = qw_webp_document_parse(bytes, size, &document);
    if (rc != QW_STATUS_OK || document.frame_count != 1u ||
        document.frames[0].codec != QW_WEBP_CODEC_VP8 ||
        document.width != 128u || document.height != 128u ||
        document.frames[0].has_alpha) {
        fprintf(stderr, "multi-partition VP8 metadata check failed (%d)\n", rc);
        goto fail;
    }
    rc = qw_webp_vp8_parse_keyframe_header(
        document.frames[0].image_data, document.frames[0].image_size,
        document.width, document.height, &header);
    if (rc != QW_STATUS_OK || header.token_partition_count != 4u) {
        fprintf(stderr,
                "multi-partition VP8 header check failed (%d, count=%u)\n",
                rc, header.token_partition_count);
        goto fail;
    }
    rc = qw_webp_vp8_parse_token_partitions(
        document.frames[0].image_data, document.frames[0].image_size,
        &header, partitions, &partition_count);
    if (rc != QW_STATUS_OK || partition_count != 4u) {
        fprintf(stderr,
                "multi-partition VP8 partition table failed (%d, %zu)\n",
                rc, partition_count);
        goto fail;
    }
    for (size_t i = 0u; i < partition_count; ++i) {
        if (!partitions[i].data ||
            partitions[i].size != expected_partition_sizes[i]) {
            fprintf(stderr,
                    "multi-partition VP8 partition %zu size mismatch (%zu)\n",
                    i, partitions[i].size);
            goto fail;
        }
    }
    rc = qw_webp_vp8_decode_macroblock_modes(
        document.frames[0].image_data, document.frames[0].image_size,
        &header, &macroblocks, &macroblock_count);
    if (rc != QW_STATUS_OK || macroblock_count != 64u || !macroblocks) {
        fprintf(stderr, "multi-partition VP8 modes failed (%d, %zu)\n",
                rc, macroblock_count);
        goto fail;
    }
    rc = qw_webp_vp8_decode_rgba(document.frames[0].image_data,
                                 document.frames[0].image_size,
                                 document.width, document.height,
                                 &reconstructed);
    if (rc != QW_STATUS_OK || !reconstructed) {
        fprintf(stderr, "multi-partition internal decode failed (%d)\n", rc);
        goto fail;
    }
    rc = qw_image_decode(bytes, size, &reference);
    if (rc != QW_IMAGE_OK || reference.width != document.width ||
        reference.height != document.height ||
        reference.format != QW_IMAGE_PIXEL_RGB24 ||
        reference.size != pixels * 3u || !reference.pixels) {
        fprintf(stderr, "multi-partition reference decode failed (%d)\n", rc);
        goto fail;
    }
    for (size_t i = 0u; i < pixels; ++i) {
        if (reconstructed[i * 4u + 3u] != 255u) {
            fprintf(stderr, "multi-partition alpha mismatch at %zu\n", i);
            goto fail;
        }
        for (size_t channel = 0u; channel < 3u; ++channel) {
            int difference = (int)reconstructed[i * 4u + channel] -
                             (int)reference.pixels[i * 3u + channel];
            size_t error = (size_t)(difference < 0 ? -difference : difference);
            if (error > maximum_error) maximum_error = error;
            total_error += error;
        }
    }
    printf("multi-partition VP8: %ux%u token partitions=%zu sizes=",
           document.width, document.height, partition_count);
    for (size_t i = 0u; i < partition_count; ++i) {
        printf("%s%zu", i ? "," : "", partitions[i].size);
    }
    printf(" max RGB error=%zu mean RGB error=%.3f RGBA checksum=%llu "
           "public RGB checksum=%llu\n", maximum_error,
           (double)total_error / (double)(pixels * 3u),
           (unsigned long long)checksum(reconstructed, pixels * 4u),
           (unsigned long long)checksum(reference.pixels, reference.size));
    if (maximum_error != 0u ||
        checksum(reconstructed, pixels * 4u) !=
            UINT64_C(12875626219371720984) ||
        checksum(reference.pixels, reference.size) !=
            UINT64_C(13854725974748926434)) {
        fprintf(stderr, "multi-partition VP8 output golden failed\n");
        goto fail;
    }
    qw_image_free(&reference);
    qw_webp_document_clear(&document);
    free(reconstructed);
    free(macroblocks);
    free(bytes);
    return 1;

fail:
    qw_image_free(&reference);
    qw_webp_document_clear(&document);
    free(reconstructed);
    free(macroblocks);
    free(bytes);
    return 0;
}

static int test_vp8_partition_count_vector(const char *fixture_name,
                                           size_t expected_partition_count,
                                           const size_t *expected_sizes,
                                           uint64_t expected_rgba_checksum,
                                           uint64_t expected_public_checksum)
{
    uint8_t *bytes = NULL;
    uint8_t *reconstructed = NULL;
    size_t size = 0u;
    size_t partition_count = 0u;
    size_t maximum_error = 0u;
    uint64_t total_error = 0u;
    QwWebPDocument document = {0};
    QwWebPVP8Header header = {0};
    QwWebPVP8TokenPartition partitions[8] = {{0}};
    QwImage reference = {0};
    const size_t pixels = 128u * 128u;
    int rc;

    if (!read_fixture(fixture_name, &bytes, &size)) return 0;
    rc = qw_webp_document_parse(bytes, size, &document);
    if (rc != QW_STATUS_OK || document.frame_count != 1u ||
        document.frames[0].codec != QW_WEBP_CODEC_VP8 ||
        document.width != 128u || document.height != 128u ||
        document.frames[0].has_alpha) {
        fprintf(stderr, "VP8 partition-count metadata failed for %s (%d)\n",
                fixture_name, rc);
        goto fail;
    }
    rc = qw_webp_vp8_parse_keyframe_header(
        document.frames[0].image_data, document.frames[0].image_size,
        document.width, document.height, &header);
    if (rc != QW_STATUS_OK ||
        header.token_partition_count != expected_partition_count) {
        fprintf(stderr,
                "VP8 partition-count header failed for %s (%d, %u)\n",
                fixture_name, rc, header.token_partition_count);
        goto fail;
    }
    rc = qw_webp_vp8_parse_token_partitions(
        document.frames[0].image_data, document.frames[0].image_size,
        &header, partitions, &partition_count);
    if (rc != QW_STATUS_OK || partition_count != expected_partition_count) {
        fprintf(stderr,
                "VP8 partition table failed for %s (%d, %zu)\n",
                fixture_name, rc, partition_count);
        goto fail;
    }
    for (size_t i = 0u; i < partition_count; ++i) {
        if (!partitions[i].data || partitions[i].size < 2u ||
            (expected_sizes && expected_sizes[i] != 0u &&
             partitions[i].size != expected_sizes[i])) {
            fprintf(stderr,
                    "VP8 partition size check failed for %s partition %zu (%zu)\n",
                    fixture_name, i, partitions[i].size);
            goto fail;
        }
    }
    rc = qw_webp_vp8_decode_rgba(document.frames[0].image_data,
                                 document.frames[0].image_size,
                                 document.width, document.height,
                                 &reconstructed);
    if (rc != QW_STATUS_OK || !reconstructed) {
        fprintf(stderr, "VP8 partition-count internal decode failed for %s (%d)\n",
                fixture_name, rc);
        goto fail;
    }
    rc = qw_image_decode(bytes, size, &reference);
    if (rc != QW_IMAGE_OK || reference.width != document.width ||
        reference.height != document.height ||
        reference.format != QW_IMAGE_PIXEL_RGB24 ||
        reference.size != pixels * 3u || !reference.pixels) {
        fprintf(stderr, "VP8 partition-count reference decode failed for %s (%d)\n",
                fixture_name, rc);
        goto fail;
    }
    for (size_t i = 0u; i < pixels; ++i) {
        if (reconstructed[i * 4u + 3u] != 255u) {
            fprintf(stderr, "VP8 partition-count alpha mismatch in %s\n",
                    fixture_name);
            goto fail;
        }
        for (size_t channel = 0u; channel < 3u; ++channel) {
            int difference = (int)reconstructed[i * 4u + channel] -
                             (int)reference.pixels[i * 3u + channel];
            size_t error = (size_t)(difference < 0 ? -difference : difference);
            if (error > maximum_error) maximum_error = error;
            total_error += error;
        }
    }
    printf("VP8 partition-count %s: count=%zu sizes=", fixture_name,
           partition_count);
    for (size_t i = 0u; i < partition_count; ++i) {
        printf("%s%zu", i ? "," : "", partitions[i].size);
    }
    printf(" max RGB error=%zu mean RGB error=%.3f RGBA checksum=%llu "
           "public RGB checksum=%llu\n", maximum_error,
           (double)total_error / (double)(pixels * 3u),
           (unsigned long long)checksum(reconstructed, pixels * 4u),
           (unsigned long long)checksum(reference.pixels, reference.size));
    if (maximum_error != 0u ||
        (expected_rgba_checksum != 0u &&
         checksum(reconstructed, pixels * 4u) != expected_rgba_checksum) ||
        (expected_public_checksum != 0u &&
         checksum(reference.pixels, reference.size) != expected_public_checksum)) {
        fprintf(stderr, "VP8 partition-count output golden failed for %s\n",
                fixture_name);
        goto fail;
    }
    qw_image_free(&reference);
    qw_webp_document_clear(&document);
    free(reconstructed);
    free(bytes);
    return 1;

fail:
    qw_image_free(&reference);
    qw_webp_document_clear(&document);
    free(reconstructed);
    free(bytes);
    return 0;
}

static int test_vp8_additional_partition_counts(void)
{
    static const size_t two_partition_sizes[2] = {3859u, 3780u};
    static const size_t eight_partition_sizes[8] = {
        1010u, 899u, 928u, 938u, 955u, 963u, 972u, 988u
    };
    return test_vp8_partition_count_vector(
               "vp8_multipartition_p1_128x128.webp", 2u,
               two_partition_sizes,
               UINT64_C(12875626219371720984),
               UINT64_C(13854725974748926434)) &&
           test_vp8_partition_count_vector(
               "vp8_multipartition_p3_128x128.webp", 8u,
               eight_partition_sizes,
               UINT64_C(12875626219371720984),
               UINT64_C(13854725974748926434));
}

int main(void)
{
    if (!test_webp_malformed_inputs_and_limits()) return 1;
    if (!test_webp_deterministic_mutations()) return 1;
    if (!test_still_image()) return 1;
    if (!test_vp8l_alpha_fixture()) return 1;
    if (!test_alpha_chunk()) return 1;
    if (!test_vp8_reconstruction_diagnostic()) return 1;
    if (!test_alpha_raw_filters()) return 1;
    if (!test_vp8_bpred_above_right()) return 1;
    if (!test_vp8_probability_context_coverage()) return 1;
    if (!test_vp8_multimb_bpred_fixture()) return 1;
    if (!test_vp8_odd_dimensions()) return 1;
    if (!test_vp8_nonzero_loop_filter()) return 1;
    if (!test_vp8_coverage_vectors()) return 1;
    if (!test_vp8_multipartition_fixture()) return 1;
    if (!test_vp8_additional_partition_counts()) return 1;
    if (!test_vp8_keyframe_header()) return 1;
    if (!test_animation()) return 1;
    if (!test_lossy_vp8_animation()) return 1;
    return 0;
}
