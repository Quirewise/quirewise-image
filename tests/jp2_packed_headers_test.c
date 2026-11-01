#include "jp2_packed_headers.h"

#include <stdio.h>
#include <string.h>

static int bytes_equal(const uint8_t *left,
                       const uint8_t *right,
                       size_t size)
{
    return memcmp(left, right, size) == 0;
}

static int test_ppt_concatenation(void)
{
    static const uint8_t first[] = {0x80u, 0xffu};
    static const uint8_t second[] = {0x7fu, 0x01u};
    static const uint8_t expected[] = {0x80u, 0xffu, 0x7fu, 0x01u};
    const QwJp2PackedHeaderSegment segments[] = {
        {0u, first, sizeof(first)},
        {1u, second, sizeof(second)}
    };
    uint8_t output[sizeof(expected)] = {0u};
    size_t output_size = 0u;

    if (qw_jp2_packed_header_collect(segments, 2u, output, sizeof(output),
                                     &output_size) !=
            QW_JP2_PACKED_HEADER_OK ||
        output_size != sizeof(expected) ||
        !bytes_equal(output, expected, sizeof(expected))) {
        fputs("PPT payload concatenation mismatch\n", stderr);
        return 0;
    }
    return 1;
}

static int test_ppm_records_across_segments(void)
{
    static const uint8_t first[] = {
        0x00u, 0x00u, 0x00u, 0x06u, 0xa0u, 0xa1u, 0xa2u
    };
    static const uint8_t second[] = {
        0xb0u, 0xb1u, 0xb2u, 0x00u, 0x00u, 0x00u, 0x01u, 0xc0u
    };
    static const uint8_t expected[] = {
        0x00u, 0x00u, 0x00u, 0x06u, 0xa0u, 0xa1u, 0xa2u,
        0xb0u, 0xb1u, 0xb2u, 0x00u, 0x00u, 0x00u, 0x01u, 0xc0u
    };
    static const uint8_t first_header[] = {
        0xa0u, 0xa1u, 0xa2u, 0xb0u, 0xb1u, 0xb2u
    };
    static const uint8_t second_header[] = {0xc0u};
    const QwJp2PackedHeaderSegment segments[] = {
        {0u, first, sizeof(first)},
        {1u, second, sizeof(second)},
    };
    uint8_t output[sizeof(expected)] = {0u};
    size_t output_size = 0u;
    QwJp2PacketHeaderSpan header = {NULL, 0u};

    if (qw_jp2_packed_header_collect(segments, 2u, output, sizeof(output),
                                     &output_size) !=
            QW_JP2_PACKED_HEADER_OK ||
        output_size != sizeof(expected) ||
        !bytes_equal(output, expected, sizeof(expected))) {
        fputs("PPM Nppm/Ippm concatenation mismatch\n", stderr);
        return 0;
    }
    if (qw_jp2_ppm_tile_part_header(output, output_size, 0u, &header) !=
            QW_JP2_PACKED_HEADER_OK ||
        header.size != sizeof(first_header) ||
        !bytes_equal(header.data, first_header, sizeof(first_header))) {
        fputs("PPM first tile-part header mismatch\n", stderr);
        return 0;
    }
    if (qw_jp2_ppm_tile_part_header(output, output_size, 1u, &header) !=
            QW_JP2_PACKED_HEADER_OK ||
        header.size != sizeof(second_header) ||
        !bytes_equal(header.data, second_header, sizeof(second_header))) {
        fputs("PPM second tile-part header mismatch\n", stderr);
        return 0;
    }
    return qw_jp2_ppm_tile_part_header(output, output_size, 2u, &header) ==
               QW_JP2_PACKED_HEADER_ERR_FORMAT &&
           qw_jp2_ppm_validate_tile_part_count(output, output_size, 2u) ==
               QW_JP2_PACKED_HEADER_OK &&
           qw_jp2_ppm_validate_tile_part_count(output, output_size, 1u) ==
               QW_JP2_PACKED_HEADER_ERR_FORMAT &&
           qw_jp2_ppm_validate_tile_part_count(output, output_size, 3u) ==
               QW_JP2_PACKED_HEADER_ERR_FORMAT;
}

static int test_transactional_errors_and_limits(void)
{
    static const uint8_t payload[] = {0x11u};
    static const uint8_t truncated_ppm[] = {
        0x00u, 0x00u, 0x00u, 0x02u, 0xaau
    };
    uint8_t output[4];
    uint8_t untouched[sizeof(output)];
    size_t output_size = 77u;
    QwJp2PacketHeaderSpan header = {payload, sizeof(payload)};
    QwJp2PackedHeaderSegment valid[] = {{0u, payload, sizeof(payload)}};
    QwJp2PackedHeaderSegment out_of_order[] = {
        {0u, payload, sizeof(payload)},
        {2u, payload, sizeof(payload)}
    };
    QwJp2PackedHeaderSegment oversized[] = {
        {0u, payload, (size_t)QW_JP2_PACKED_HEADER_MAX_BYTES + 1u}
    };

    memset(output, 0x5au, sizeof(output));
    memcpy(untouched, output, sizeof(output));
    if (qw_jp2_packed_header_collect(out_of_order, 2u, output,
                                     sizeof(output), &output_size) !=
            QW_JP2_PACKED_HEADER_ERR_FORMAT ||
        output_size != 77u || !bytes_equal(output, untouched, sizeof(output))) {
        fputs("out-of-order segment rejection was not transactional\n",
              stderr);
        return 0;
    }
    if (qw_jp2_packed_header_collect(valid, 1u, output, 0u, &output_size) !=
            QW_JP2_PACKED_HEADER_ERR_RESOURCE ||
        output_size != 77u || !bytes_equal(output, untouched, sizeof(output))) {
        fputs("small output capacity rejection was not transactional\n",
              stderr);
        return 0;
    }
    if (qw_jp2_packed_header_collect(oversized, 1u, output, sizeof(output),
                                     &output_size) !=
            QW_JP2_PACKED_HEADER_ERR_RESOURCE ||
        output_size != 77u || !bytes_equal(output, untouched, sizeof(output))) {
        fputs("packed-header size limit rejection mismatch\n", stderr);
        return 0;
    }
    if (qw_jp2_ppm_tile_part_header(truncated_ppm, sizeof(truncated_ppm), 0u,
                                    &header) !=
            QW_JP2_PACKED_HEADER_ERR_FORMAT ||
        header.data != payload || header.size != sizeof(payload)) {
        fputs("truncated PPM record rejection was not transactional\n",
              stderr);
        return 0;
    }
    if (qw_jp2_ppm_validate_tile_part_count(truncated_ppm,
                                            sizeof(truncated_ppm), 1u) !=
        QW_JP2_PACKED_HEADER_ERR_FORMAT) {
        fputs("truncated PPM count validation accepted an incomplete record\n",
              stderr);
        return 0;
    }
    return 1;
}

static int test_arguments(void)
{
    static const uint8_t payload[] = {0u};
    const QwJp2PackedHeaderSegment segment = {0u, payload, sizeof(payload)};
    uint8_t output[1];
    size_t output_size = 0u;

    return qw_jp2_packed_header_collect(NULL, 1u, output, sizeof(output),
                                        &output_size) ==
               QW_JP2_PACKED_HEADER_ERR_ARGUMENT &&
           qw_jp2_packed_header_collect(&segment, 0u, output, sizeof(output),
                                        &output_size) ==
               QW_JP2_PACKED_HEADER_ERR_ARGUMENT &&
           qw_jp2_ppm_tile_part_header(NULL, 1u, 0u, NULL) ==
               QW_JP2_PACKED_HEADER_ERR_ARGUMENT &&
           qw_jp2_ppm_validate_tile_part_count(NULL, 1u, 0u) ==
               QW_JP2_PACKED_HEADER_ERR_ARGUMENT;
}

int main(void)
{
    if (!test_ppt_concatenation() || !test_ppm_records_across_segments() ||
        !test_transactional_errors_and_limits() || !test_arguments()) {
        return 1;
    }
    puts("Quirewise JPEG 2000 packed-header tests passed");
    return 0;
}
