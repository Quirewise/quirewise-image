#include "image_internal.h"

#if defined(QW_JP2_FUZZ_PUBLIC_DISPATCH)
#include <quirewise/image.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QW_JP2_FUZZ_MAP_SIZE (1u << 16u)
#define QW_JP2_FUZZ_MAX_INPUT_SIZE (1u << 20u)
#define QW_JP2_FUZZ_MAX_CORPUS 128u
#ifndef QW_JP2_FUZZ_DEFAULT_RUNS
#if defined(QW_JP2_FUZZ_PUBLIC_DISPATCH)
#define QW_JP2_FUZZ_DEFAULT_RUNS 1000u
#else
#define QW_JP2_FUZZ_DEFAULT_RUNS 50000u
#endif
#endif
#if defined(QW_JP2_FUZZ_PUBLIC_DISPATCH)
#define QW_JP2_FUZZ_MAX_RUNS 10000ul
#define QW_JP2_FUZZ_TARGET_NAME "public_dispatch"
#else
#define QW_JP2_FUZZ_MAX_RUNS 10000000ul
#define QW_JP2_FUZZ_TARGET_NAME "internal"
#endif
#define QW_JP2_FUZZ_RNG_SEED UINT32_C(0x51574a32)

typedef struct QwJp2FuzzInput {
    uint8_t *data;
    size_t size;
} QwJp2FuzzInput;

typedef struct QwJp2FuzzStats {
    size_t structure_valid;
    size_t decode_success;
    size_t decode_unsupported;
    size_t decode_other;
} QwJp2FuzzStats;

static uint8_t qw_jp2_fuzz_current_coverage[QW_JP2_FUZZ_MAP_SIZE];
static uint8_t qw_jp2_fuzz_seen_coverage[QW_JP2_FUZZ_MAP_SIZE];
static uint8_t qw_jp2_fuzz_candidate[QW_JP2_FUZZ_MAX_INPUT_SIZE];
static QwJp2FuzzInput qw_jp2_fuzz_corpus[QW_JP2_FUZZ_MAX_CORPUS];
static size_t qw_jp2_fuzz_corpus_count;
static size_t qw_jp2_fuzz_coverage_slots;
static uint32_t qw_jp2_fuzz_previous_location;
static uint32_t qw_jp2_fuzz_rng = QW_JP2_FUZZ_RNG_SEED;
static QwJp2FuzzStats qw_jp2_fuzz_stats;

void __sanitizer_cov_trace_pc(void)
{
    uintptr_t pc = (uintptr_t)__builtin_return_address(0);
    uint32_t location = (uint32_t)((pc >> 4u) ^ (pc >> 17u) ^ (pc >> 29u));
    uint32_t edge = (location ^ qw_jp2_fuzz_previous_location) &
                    (QW_JP2_FUZZ_MAP_SIZE - 1u);

    qw_jp2_fuzz_current_coverage[edge] = 1u;
    qw_jp2_fuzz_previous_location = location >> 1u;
}

static uint32_t qw_jp2_fuzz_random(void)
{
    uint32_t value = qw_jp2_fuzz_rng;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    qw_jp2_fuzz_rng = value;
    return value;
}

static size_t qw_jp2_fuzz_random_below(size_t limit)
{
    return limit ? (size_t)(qw_jp2_fuzz_random() % limit) : 0u;
}

static int qw_jp2_fuzz_load_file(const char *path,
                                 uint8_t **out_data,
                                 size_t *out_size)
{
    FILE *file;
    long length;
    uint8_t *data;

    if (!path || !out_data || !out_size) return 0;
    *out_data = NULL;
    *out_size = 0u;
    file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0L, SEEK_END) != 0 ||
        (length = ftell(file)) < 0 ||
        fseek(file, 0L, SEEK_SET) != 0 ||
        (unsigned long)length > QW_JP2_FUZZ_MAX_INPUT_SIZE) {
        fclose(file);
        return 0;
    }
    data = (uint8_t *)malloc(length ? (size_t)length : 1u);
    if (!data) {
        fclose(file);
        return 0;
    }
    if (fread(data, 1u, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *out_data = data;
    *out_size = (size_t)length;
    return 1;
}

static void qw_jp2_fuzz_run_input(const uint8_t *data,
                                 size_t size,
                                 int collect_stats)
{
#if defined(QW_JP2_FUZZ_PUBLIC_DISPATCH)
    QwImage image = {0};
#else
    QwPageImage page = {0};
#endif
    int structure_rc;
    int decode_rc;
    int invalid_output = 0;

    qw_jp2_fuzz_previous_location = 0u;
    structure_rc = qw_jp2_validate_structure(data, size);
    if (collect_stats && structure_rc == QW_STATUS_OK) {
        ++qw_jp2_fuzz_stats.structure_valid;
    }
#if defined(QW_JP2_FUZZ_PUBLIC_DISPATCH)
    decode_rc = qw_image_decode(data, size, &image);
    if (decode_rc == QW_IMAGE_OK) {
        size_t channels = image.format == QW_IMAGE_PIXEL_GRAY8 ? 1u :
                          image.format == QW_IMAGE_PIXEL_RGB24 ? 3u : 0u;
        size_t expected_stride = 0u;
        size_t expected_size = 0u;
        if (!image.pixels || !channels || !image.width || !image.height ||
            (size_t)image.width > SIZE_MAX / channels) {
            invalid_output = 1;
        } else {
            expected_stride = (size_t)image.width * channels;
            if ((size_t)image.height > SIZE_MAX / expected_stride) {
                invalid_output = 1;
            } else {
                expected_size = expected_stride * (size_t)image.height;
                if (image.stride != expected_stride ||
                    image.size != expected_size) {
                    invalid_output = 1;
                }
            }
        }
        if (collect_stats) ++qw_jp2_fuzz_stats.decode_success;
    } else if (decode_rc == QW_IMAGE_ERR_UNSUPPORTED) {
        if (collect_stats) ++qw_jp2_fuzz_stats.decode_unsupported;
        if (image.pixels || image.size || image.stride || image.width ||
            image.height || image.format) {
            invalid_output = 1;
        }
    } else if (collect_stats) {
        ++qw_jp2_fuzz_stats.decode_other;
    }
    if (decode_rc != QW_IMAGE_OK &&
        (image.pixels || image.size || image.stride || image.width ||
         image.height || image.format)) {
        invalid_output = 1;
    }
    qw_image_free(&image);
#else
    decode_rc = qw_jp2_decode_internal_gray8(data, size, &page);
    if (decode_rc == QW_STATUS_OK) {
        size_t channels = page.kind == QW_PAGE_IMAGE_GRAY8 ? 1u :
                          page.kind == QW_PAGE_IMAGE_RGB24 ? 3u : 0u;
        size_t pixels = 0u;
        if (!page.data || !channels || !page.width || !page.height ||
            (size_t)page.width > SIZE_MAX / (size_t)page.height) {
            invalid_output = 1;
        } else {
            pixels = (size_t)page.width * (size_t)page.height;
            if (pixels > SIZE_MAX / channels ||
                page.size != pixels * channels) {
                invalid_output = 1;
            }
        }
        if (collect_stats) ++qw_jp2_fuzz_stats.decode_success;
    } else if (decode_rc == QW_STATUS_ERR_UNSUPPORTED) {
        if (collect_stats) ++qw_jp2_fuzz_stats.decode_unsupported;
        if (page.data || page.size || page.kind || page.width || page.height) {
            invalid_output = 1;
        }
    } else if (collect_stats) {
        ++qw_jp2_fuzz_stats.decode_other;
    }
    if (decode_rc != QW_STATUS_OK &&
        (page.data || page.size || page.kind || page.width || page.height)) {
        invalid_output = 1;
    }
    free((void *)page.data);
#endif
    if (invalid_output) abort();
}

static size_t qw_jp2_fuzz_merge_coverage(void)
{
    size_t added = 0u;
    for (size_t i = 0u; i < QW_JP2_FUZZ_MAP_SIZE; ++i) {
        if (qw_jp2_fuzz_current_coverage[i] &&
            !qw_jp2_fuzz_seen_coverage[i]) {
            qw_jp2_fuzz_seen_coverage[i] = 1u;
            ++added;
        }
    }
    qw_jp2_fuzz_coverage_slots += added;
    return added;
}

static int qw_jp2_fuzz_add_corpus(const uint8_t *data, size_t size)
{
    uint8_t *copy;
    if (!data || size == 0u || size > QW_JP2_FUZZ_MAX_INPUT_SIZE ||
        qw_jp2_fuzz_corpus_count >= QW_JP2_FUZZ_MAX_CORPUS) {
        return 0;
    }
    copy = (uint8_t *)malloc(size);
    if (!copy) return 0;
    memcpy(copy, data, size);
    qw_jp2_fuzz_corpus[qw_jp2_fuzz_corpus_count].data = copy;
    qw_jp2_fuzz_corpus[qw_jp2_fuzz_corpus_count].size = size;
    ++qw_jp2_fuzz_corpus_count;
    return 1;
}

static void qw_jp2_fuzz_mutate(uint8_t *output,
                               size_t *output_size,
                               const QwJp2FuzzInput *parent)
{
    static const uint8_t interesting_bytes[] = {
        0x00u, 0x01u, 0x7fu, 0x80u, 0xfeu, 0xffu,
        0x4fu, 0x51u, 0x52u, 0x5fu, 0x90u, 0xd9u
    };
    size_t size = parent->size;
    unsigned operation_count = 1u + (unsigned)qw_jp2_fuzz_random_below(5u);

    memcpy(output, parent->data, size);
    for (unsigned operation = 0u; operation < operation_count; ++operation) {
        unsigned choice = (unsigned)qw_jp2_fuzz_random_below(6u);
        if (choice == 0u && size) {
            size_t offset = qw_jp2_fuzz_random_below(size);
            output[offset] ^= (uint8_t)(1u <<
                qw_jp2_fuzz_random_below(8u));
        } else if (choice == 1u && size) {
            size_t offset = qw_jp2_fuzz_random_below(size);
            output[offset] = interesting_bytes[
                qw_jp2_fuzz_random_below(sizeof(interesting_bytes))];
        } else if (choice == 2u && size) {
            size_t offset = qw_jp2_fuzz_random_below(size);
            size_t count = 1u + qw_jp2_fuzz_random_below(
                size - offset < 16u ? size - offset : 16u);
            for (size_t i = 0u; i < count; ++i) {
                output[offset + i] = (uint8_t)qw_jp2_fuzz_random();
            }
        } else if (choice == 3u && size < QW_JP2_FUZZ_MAX_INPUT_SIZE) {
            size_t available = QW_JP2_FUZZ_MAX_INPUT_SIZE - size;
            size_t count = 1u + qw_jp2_fuzz_random_below(
                available < 32u ? available : 32u);
            size_t offset = qw_jp2_fuzz_random_below(size + 1u);
            memmove(output + offset + count, output + offset, size - offset);
            for (size_t i = 0u; i < count; ++i) {
                output[offset + i] = (uint8_t)qw_jp2_fuzz_random();
            }
            size += count;
        } else if (choice == 4u && size) {
            size_t offset = qw_jp2_fuzz_random_below(size);
            size_t count = 1u + qw_jp2_fuzz_random_below(
                size - offset < 32u ? size - offset : 32u);
            memmove(output + offset, output + offset + count,
                    size - offset - count);
            size -= count;
        } else if (choice == 5u && size && qw_jp2_fuzz_corpus_count > 1u) {
            const QwJp2FuzzInput *donor = &qw_jp2_fuzz_corpus[
                qw_jp2_fuzz_random_below(qw_jp2_fuzz_corpus_count)];
            size_t source_offset = qw_jp2_fuzz_random_below(donor->size);
            size_t target_offset = qw_jp2_fuzz_random_below(size);
            size_t count = donor->size - source_offset;
            if (count > size - target_offset) count = size - target_offset;
            if (count > 64u) count = 64u;
            memcpy(output + target_offset, donor->data + source_offset, count);
        }
    }
    *output_size = size;
}

static int qw_jp2_fuzz_parse_runs(const char *text, size_t *out_runs)
{
    char *end = NULL;
    unsigned long value;
    if (!text || !out_runs || !text[0]) return 0;
    value = strtoul(text, &end, 10);
    if (!end || *end || value == 0ul || value > QW_JP2_FUZZ_MAX_RUNS) return 0;
    *out_runs = (size_t)value;
    return 1;
}

int main(int argc, char **argv)
{
    size_t runs = QW_JP2_FUZZ_DEFAULT_RUNS;
    size_t first_seed = 1u;
    size_t completed = 0u;

    if (argc > 1 && strcmp(argv[1], "--runs") == 0) {
        if (argc < 4 || !qw_jp2_fuzz_parse_runs(argv[2], &runs)) {
            fprintf(stderr,
                    "Usage: %s [--runs COUNT] SEED_FILE [SEED_FILE ...]\n",
                    argv[0]);
            return 2;
        }
        first_seed = 3u;
    } else if (argc < 2) {
        fprintf(stderr,
                "Usage: %s [--runs COUNT] SEED_FILE [SEED_FILE ...]\n",
                argv[0]);
        return 2;
    }

    for (size_t i = first_seed; i < (size_t)argc; ++i) {
        uint8_t *data = NULL;
        size_t size = 0u;
        if (!qw_jp2_fuzz_load_file(argv[i], &data, &size)) {
            fprintf(stderr, "Unable to load bounded seed input: %s\n", argv[i]);
            return 2;
        }
        if (!qw_jp2_fuzz_add_corpus(data, size)) {
            fprintf(stderr, "Could not add seed to bounded corpus: %s\n",
                    argv[i]);
            free(data);
            return 2;
        }
        qw_jp2_fuzz_run_input(data, size, 1);
        (void)qw_jp2_fuzz_merge_coverage();
        free(data);
    }
    if (!qw_jp2_fuzz_corpus_count) {
        fputs("Coverage fuzzer requires at least one non-empty seed\n", stderr);
        return 2;
    }

    for (size_t iteration = 0u; iteration < runs; ++iteration) {
        const QwJp2FuzzInput *parent = &qw_jp2_fuzz_corpus[
            qw_jp2_fuzz_random_below(qw_jp2_fuzz_corpus_count)];
        size_t candidate_size = 0u;
        size_t new_slots;

        memset(qw_jp2_fuzz_current_coverage, 0,
               sizeof(qw_jp2_fuzz_current_coverage));
        qw_jp2_fuzz_mutate(qw_jp2_fuzz_candidate, &candidate_size, parent);
        qw_jp2_fuzz_run_input(qw_jp2_fuzz_candidate, candidate_size, 1);
        ++completed;
        new_slots = qw_jp2_fuzz_merge_coverage();
        if (new_slots && qw_jp2_fuzz_add_corpus(
                qw_jp2_fuzz_candidate, candidate_size)) {
            printf("new_coverage iteration=%zu slots=%zu total_slots=%zu "
                   "corpus=%zu input_size=%zu\n",
                   completed, new_slots, qw_jp2_fuzz_coverage_slots,
                   qw_jp2_fuzz_corpus_count, candidate_size);
        }
        if (completed % 10000u == 0u) {
            printf("progress runs=%zu/%zu coverage_slots=%zu corpus=%zu\n",
                   completed, runs, qw_jp2_fuzz_coverage_slots,
                   qw_jp2_fuzz_corpus_count);
            fflush(stdout);
        }
    }

    printf("coverage_fuzz_summary target=%s runs=%zu run_cap=%lu "
           "seeds=%zu corpus=%zu "
           "edge_map_slots=%zu structure_valid=%zu decode_success=%zu "
           "decode_unsupported=%zu decode_other=%zu rng_seed=0x%08x "
           "max_input_size=%u\n",
           QW_JP2_FUZZ_TARGET_NAME, completed, QW_JP2_FUZZ_MAX_RUNS,
           (size_t)argc - first_seed,
           qw_jp2_fuzz_corpus_count, qw_jp2_fuzz_coverage_slots,
           qw_jp2_fuzz_stats.structure_valid,
           qw_jp2_fuzz_stats.decode_success,
           qw_jp2_fuzz_stats.decode_unsupported,
           qw_jp2_fuzz_stats.decode_other,
           QW_JP2_FUZZ_RNG_SEED, QW_JP2_FUZZ_MAX_INPUT_SIZE);
    for (size_t i = 0u; i < qw_jp2_fuzz_corpus_count; ++i) {
        free(qw_jp2_fuzz_corpus[i].data);
    }
    return 0;
}
