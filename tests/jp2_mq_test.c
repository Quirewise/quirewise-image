#include "jp2_mq.h"

#include <stdio.h>
#include <string.h>

static int test_initial_contexts(void)
{
    static const uint8_t payload[] = {0x12u, 0x34u, 0x56u, 0x78u};
    QwJp2MqDecoder decoder;

    if (qw_jp2_mq_init(&decoder, payload, sizeof(payload)) != QW_JP2_MQ_OK ||
        decoder.interval != 0x8000u || decoder.contexts[0].probability_state != 4u ||
        decoder.contexts[17].probability_state != 3u ||
        decoder.contexts[18].probability_state != 46u) {
        fputs("MQ initial context state mismatch\n", stderr);
        return 0;
    }
    for (size_t i = 0u; i < QW_JP2_MQ_CONTEXT_COUNT; ++i) {
        uint8_t expected = (i == 0u) ? 4u : (i == 17u) ? 3u :
                           (i == 18u) ? 46u : 0u;
        if (decoder.contexts[i].probability_state != expected ||
            decoder.contexts[i].most_probable_symbol != 0u) {
            fputs("MQ default probability context mismatch\n", stderr);
            return 0;
        }
    }
    return 1;
}

static int test_input_and_context_errors(void)
{
    static const uint8_t payload[] = {0u};
    QwJp2MqDecoder decoder;
    uint8_t symbol = 0xa5u;

    if (qw_jp2_mq_init(NULL, payload, sizeof(payload)) !=
            QW_JP2_MQ_ERR_ARGUMENT ||
        qw_jp2_mq_init(&decoder, NULL, sizeof(payload)) !=
            QW_JP2_MQ_ERR_ARGUMENT ||
        qw_jp2_mq_init(&decoder, payload, 0u) != QW_JP2_MQ_ERR_ARGUMENT ||
        qw_jp2_mq_init(&decoder, payload, sizeof(payload)) != QW_JP2_MQ_OK ||
        qw_jp2_mq_decode(&decoder, QW_JP2_MQ_CONTEXT_COUNT, &symbol) !=
            QW_JP2_MQ_ERR_ARGUMENT || symbol != 0xa5u) {
        fputs("MQ invalid input/context handling mismatch\n", stderr);
        return 0;
    }
    return 1;
}

static int test_context_reset_and_determinism(void)
{
    static const uint8_t payload[] = {
        0x00u, 0x80u, 0x7fu, 0x01u, 0x55u, 0xaau, 0x00u, 0xffu, 0x7fu
    };
    static const uint8_t expected[24] = {
        1u, 1u, 0u, 0u, 0u, 1u, 0u, 0u,
        1u, 0u, 0u, 1u, 0u, 0u, 1u, 0u,
        1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
    };
    QwJp2MqDecoder decoder;

    if (qw_jp2_mq_init(&decoder, payload, sizeof(payload)) != QW_JP2_MQ_OK) {
        fputs("MQ initialization failed for context vector\n", stderr);
        return 0;
    }
    for (size_t i = 0u; i < sizeof(expected); ++i) {
        uint8_t symbol = 0xffu;
        uint8_t context = (i % 3u == 0u) ? 0u :
                          (i % 3u == 1u) ? 17u : 18u;
        if (qw_jp2_mq_decode(&decoder, context, &symbol) != QW_JP2_MQ_OK ||
            symbol != expected[i]) {
            fprintf(stderr, "MQ decision mismatch at %zu (got=%u expected=%u)\n",
                    i, symbol, expected[i]);
            return 0;
        }
    }

    qw_jp2_mq_reset_contexts(&decoder);
    if (decoder.contexts[0].probability_state != 4u ||
        decoder.contexts[17].probability_state != 3u ||
        decoder.contexts[18].probability_state != 46u ||
        decoder.contexts[0].most_probable_symbol != 0u ||
        decoder.contexts[17].most_probable_symbol != 0u ||
        decoder.contexts[18].most_probable_symbol != 0u) {
        fputs("MQ arithmetic context reset failed\n", stderr);
        return 0;
    }
    return 1;
}

static int test_probability_state_mps_switch(void)
{
    static const uint8_t payload[] = {0xffu, 0xffu};
    QwJp2MqDecoder decoder;
    uint8_t symbol = 0u;

    if (qw_jp2_mq_init(&decoder, payload, sizeof(payload)) != QW_JP2_MQ_OK) {
        fputs("MQ initialization failed for MPS switch vector\n", stderr);
        return 0;
    }
    decoder.contexts[1].probability_state = 0u;
    decoder.interval = 0xa000u;
    decoder.code = UINT32_C(0x80000000);
    if (qw_jp2_mq_decode(&decoder, 1u, &symbol) != QW_JP2_MQ_OK ||
        symbol != 1u || decoder.contexts[1].probability_state != 1u ||
        decoder.contexts[1].most_probable_symbol != 1u) {
        fputs("MQ LPS transition did not switch the MPS state\n", stderr);
        return 0;
    }
    return 1;
}

static int test_mps_without_exchange_keeps_probability_state(void)
{
    static const uint8_t payload[] = {0x00u, 0x00u};
    QwJp2MqDecoder decoder;
    uint8_t symbol = 0xffu;

    if (qw_jp2_mq_init(&decoder, payload, sizeof(payload)) != QW_JP2_MQ_OK) {
        fputs("MQ initialization failed for non-exchange MPS vector\n", stderr);
        return 0;
    }
    decoder.contexts[2].probability_state = 0u;
    decoder.interval = 0xf000u;
    decoder.code = UINT32_C(0x80000000);
    if (qw_jp2_mq_decode(&decoder, 2u, &symbol) != QW_JP2_MQ_OK ||
        symbol != 0u || decoder.contexts[2].probability_state != 0u ||
        decoder.contexts[2].most_probable_symbol != 0u) {
        fputs("MQ non-exchange MPS incorrectly advanced its probability state\n",
              stderr);
        return 0;
    }
    return 1;
}

int main(void)
{
    if (!test_initial_contexts() || !test_input_and_context_errors() ||
        !test_context_reset_and_determinism() ||
        !test_probability_state_mps_switch() ||
        !test_mps_without_exchange_keeps_probability_state()) {
        return 1;
    }
    puts("Quirewise JPEG 2000 MQ decoder tests passed");
    return 0;
}
