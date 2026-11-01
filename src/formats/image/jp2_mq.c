#include "jp2_mq.h"

#include <string.h>

typedef struct QwJp2MqProbabilityState {
    uint16_t q_e;
    uint8_t next_mps;
    uint8_t next_lps;
    uint8_t switch_mps;
} QwJp2MqProbabilityState;

/* ITU-T T.800 Table C.2, represented as the 47-state probability machine. */
static const QwJp2MqProbabilityState qw_jp2_mq_probability_states[47] = {
    {0x5601u,  1u,  1u, 1u}, {0x3401u,  2u,  6u, 0u},
    {0x1801u,  3u,  9u, 0u}, {0x0ac1u,  4u, 12u, 0u},
    {0x0521u,  5u, 29u, 0u}, {0x0221u, 38u, 33u, 0u},
    {0x5601u,  7u,  6u, 1u}, {0x5401u,  8u, 14u, 0u},
    {0x4801u,  9u, 14u, 0u}, {0x3801u, 10u, 14u, 0u},
    {0x3001u, 11u, 17u, 0u}, {0x2401u, 12u, 18u, 0u},
    {0x1c01u, 13u, 20u, 0u}, {0x1601u, 29u, 21u, 0u},
    {0x5601u, 15u, 14u, 1u}, {0x5401u, 16u, 14u, 0u},
    {0x5101u, 17u, 15u, 0u}, {0x4801u, 18u, 16u, 0u},
    {0x3801u, 19u, 17u, 0u}, {0x3401u, 20u, 18u, 0u},
    {0x3001u, 21u, 19u, 0u}, {0x2801u, 22u, 19u, 0u},
    {0x2401u, 23u, 20u, 0u}, {0x2201u, 24u, 21u, 0u},
    {0x1c01u, 25u, 22u, 0u}, {0x1801u, 26u, 23u, 0u},
    {0x1601u, 27u, 24u, 0u}, {0x1401u, 28u, 25u, 0u},
    {0x1201u, 29u, 26u, 0u}, {0x1101u, 30u, 27u, 0u},
    {0x0ac1u, 31u, 28u, 0u}, {0x09c1u, 32u, 29u, 0u},
    {0x08a1u, 33u, 30u, 0u}, {0x0521u, 34u, 31u, 0u},
    {0x0441u, 35u, 32u, 0u}, {0x02a1u, 36u, 33u, 0u},
    {0x0221u, 37u, 34u, 0u}, {0x0141u, 38u, 35u, 0u},
    {0x0111u, 39u, 36u, 0u}, {0x0085u, 40u, 37u, 0u},
    {0x0049u, 41u, 38u, 0u}, {0x0025u, 42u, 39u, 0u},
    {0x0015u, 43u, 40u, 0u}, {0x0009u, 44u, 41u, 0u},
    {0x0005u, 45u, 42u, 0u}, {0x0001u, 45u, 43u, 0u},
    {0x5601u, 46u, 46u, 0u}
};

static void qw_jp2_mq_byte_in(QwJp2MqDecoder *decoder)
{
    uint8_t next;

    if (decoder->marker_seen) {
        decoder->code += UINT32_C(0x0000ff00);
        decoder->bits_left = 8u;
        return;
    }

    if (decoder->last_byte == 0xffu) {
        if (decoder->position < decoder->size) {
            next = decoder->data[decoder->position];
            if (next > 0x8fu) {
                decoder->marker_seen = 1u;
                decoder->code += UINT32_C(0x0000ff00);
                decoder->bits_left = 8u;
                return;
            }
            decoder->position++;
            decoder->last_byte = next;
            decoder->code += (uint32_t)next << 9;
            decoder->bits_left = 7u;
            return;
        }

        decoder->marker_seen = 1u;
        decoder->code += UINT32_C(0x0000ff00);
        decoder->bits_left = 8u;
        return;
    }

    if (decoder->position < decoder->size) {
        next = decoder->data[decoder->position++];
        decoder->last_byte = next;
    } else {
        /* T.800 permits the decoder to extend a terminated segment with 0xFF. */
        next = 0xffu;
        decoder->last_byte = next;
        decoder->marker_seen = 1u;
    }
    decoder->code += (uint32_t)next << 8;
    decoder->bits_left = 8u;
}

static void qw_jp2_mq_renormalize(QwJp2MqDecoder *decoder)
{
    while ((decoder->interval & 0x8000u) == 0u) {
        if (decoder->bits_left == 0u) qw_jp2_mq_byte_in(decoder);
        decoder->interval = (uint16_t)(decoder->interval << 1);
        decoder->code <<= 1;
        decoder->bits_left--;
    }
}

void qw_jp2_mq_reset_contexts(QwJp2MqDecoder *decoder)
{
    if (!decoder) return;
    memset(decoder->contexts, 0, sizeof(decoder->contexts));
    decoder->contexts[0].probability_state = 4u;
    decoder->contexts[17].probability_state = 3u;
    decoder->contexts[18].probability_state = 46u;
}

int qw_jp2_mq_init(QwJp2MqDecoder *decoder, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;

    if (!decoder || !bytes || size == 0u) return QW_JP2_MQ_ERR_ARGUMENT;

    memset(decoder, 0, sizeof(*decoder));
    decoder->data = bytes;
    decoder->size = size;
    decoder->interval = 0x8000u;
    decoder->position = 1u;
    decoder->last_byte = bytes[0];
    decoder->code = (uint32_t)bytes[0] << 16;
    qw_jp2_mq_byte_in(decoder);
    decoder->code <<= 7;
    decoder->bits_left = (uint8_t)(decoder->bits_left - 7u);
    qw_jp2_mq_reset_contexts(decoder);
    return QW_JP2_MQ_OK;
}

int qw_jp2_mq_init_segment(QwJp2MqDecoder *decoder,
                           const void *data,
                           size_t size,
                           int preserve_contexts)
{
    QwJp2MqContext contexts[QW_JP2_MQ_CONTEXT_COUNT];
    if (!decoder || (preserve_contexts != 0 && preserve_contexts != 1)) {
        return QW_JP2_MQ_ERR_ARGUMENT;
    }
    if (preserve_contexts) {
        memcpy(contexts, decoder->contexts, sizeof(contexts));
    }
    if (size == 0u) {
        memset(decoder, 0, sizeof(*decoder));
        decoder->data = (const uint8_t *)data;
        decoder->interval = 0x8000u;
        decoder->last_byte = 0xffu;
        decoder->code = UINT32_C(0x00ff0000);
        qw_jp2_mq_byte_in(decoder);
        decoder->code <<= 7;
        decoder->bits_left = (uint8_t)(decoder->bits_left - 7u);
        qw_jp2_mq_reset_contexts(decoder);
    } else if (qw_jp2_mq_init(decoder, data, size) != QW_JP2_MQ_OK) {
        return QW_JP2_MQ_ERR_ARGUMENT;
    }
    if (preserve_contexts) {
        memcpy(decoder->contexts, contexts, sizeof(contexts));
    }
    return QW_JP2_MQ_OK;
}

int qw_jp2_mq_decode(QwJp2MqDecoder *decoder,
                     uint8_t context,
                     uint8_t *symbol)
{
    QwJp2MqContext *state;
    const QwJp2MqProbabilityState *probability;
    uint16_t q_e;
    uint8_t decoded;
    uint16_t code_high;

    if (!decoder || !symbol || context >= QW_JP2_MQ_CONTEXT_COUNT) {
        return QW_JP2_MQ_ERR_ARGUMENT;
    }
    state = &decoder->contexts[context];
    if (state->probability_state >= 47u || state->most_probable_symbol > 1u) {
        return QW_JP2_MQ_ERR_STATE;
    }

    probability = &qw_jp2_mq_probability_states[state->probability_state];
    q_e = probability->q_e;
    decoder->interval = (uint16_t)(decoder->interval - q_e);
    code_high = (uint16_t)(decoder->code >> 16);

    if (code_high < q_e) {
        if (decoder->interval < q_e) {
            decoded = state->most_probable_symbol;
            state->probability_state = probability->next_mps;
        } else {
            decoded = (uint8_t)(1u - state->most_probable_symbol);
            if (probability->switch_mps) {
                state->most_probable_symbol ^= 1u;
            }
            state->probability_state = probability->next_lps;
        }
        decoder->interval = q_e;
        qw_jp2_mq_renormalize(decoder);
    } else {
        decoder->code -= (uint32_t)q_e << 16;
        if ((decoder->interval & 0x8000u) != 0u) {
            decoded = state->most_probable_symbol;
        } else if (decoder->interval < q_e) {
            decoded = (uint8_t)(1u - state->most_probable_symbol);
            if (probability->switch_mps) {
                state->most_probable_symbol ^= 1u;
            }
            state->probability_state = probability->next_lps;
            qw_jp2_mq_renormalize(decoder);
        } else {
            decoded = state->most_probable_symbol;
            state->probability_state = probability->next_mps;
            qw_jp2_mq_renormalize(decoder);
        }
    }

    *symbol = decoded;
    return QW_JP2_MQ_OK;
}
