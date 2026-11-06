#include <quirewise/image.h>

#include "image_seeds.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FUZZ_SEED UINT32_C(0x51f0ff22)
#define FUZZ_RUNS 120u

static uint32_t rng_state = FUZZ_SEED;

static uint32_t next_random(void)
{
    uint32_t value = rng_state;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    rng_state = value;
    return value;
}

static size_t mutate(const unsigned char *seed, size_t seed_size,
                     unsigned char *buffer, size_t capacity)
{
    size_t length = seed_size > capacity ? capacity : seed_size;
    unsigned int flips = 1u + next_random() % 4u;
    size_t index;
    memcpy(buffer, seed, length);
    if (next_random() % 8u == 0u && length > 1u)
        length = 1u + next_random() % length;
    for (index = 0u; index < flips && length; ++index) {
        size_t position = next_random() % length;
        buffer[position] = (unsigned char)(buffer[position] ^
                                           (unsigned char)(1u << (next_random() % 8u)));
    }
    return length;
}

static void exercise(const unsigned char *seed, size_t seed_size, size_t *ok,
                     size_t *rejected)
{
    unsigned char buffer[8192];
    size_t run;
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        QwImage image;
        size_t size = mutate(seed, seed_size, buffer, sizeof(buffer));
        memset(&image, 0, sizeof(image));
        (void)qw_image_probe(buffer, size);
        if (qw_image_decode(buffer, size, &image) == QW_IMAGE_OK) {
            ++*ok;
            qw_image_free(&image);
        } else {
            ++*rejected;
        }
    }
}

int main(void)
{
    size_t ok = 0u;
    size_t rejected = 0u;
    exercise(png_seed, sizeof(png_seed), &ok, &rejected);
    exercise(jpg_seed, sizeof(jpg_seed), &ok, &rejected);
    exercise(gif_seed, sizeof(gif_seed), &ok, &rejected);
    exercise(bmp_seed, sizeof(bmp_seed), &ok, &rejected);
    exercise(webp_seed, sizeof(webp_seed), &ok, &rejected);
    printf("Image fuzz smoke: %zu ok, %zu rejected, seed=0x%08x runs=%u\n", ok,
           rejected, FUZZ_SEED, FUZZ_RUNS);
    return 0;
}
