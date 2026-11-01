#ifndef QUIREWISE_JP2_MQ_H
#define QUIREWISE_JP2_MQ_H

#include <stddef.h>
#include <stdint.h>

enum {
    QW_JP2_MQ_CONTEXT_COUNT = 19,
    QW_JP2_MQ_OK = 0,
    QW_JP2_MQ_ERR_ARGUMENT = -1,
    QW_JP2_MQ_ERR_STATE = -2
};

typedef struct QwJp2MqContext {
    uint8_t probability_state;
    uint8_t most_probable_symbol;
} QwJp2MqContext;

typedef struct QwJp2MqDecoder {
    const uint8_t *data;
    size_t size;
    size_t position;
    uint32_t code;
    uint16_t interval;
    uint8_t bits_left;
    uint8_t last_byte;
    uint8_t marker_seen;
    QwJp2MqContext contexts[QW_JP2_MQ_CONTEXT_COUNT];
} QwJp2MqDecoder;

int qw_jp2_mq_init(QwJp2MqDecoder *decoder, const void *data, size_t size);
int qw_jp2_mq_init_segment(QwJp2MqDecoder *decoder,
                           const void *data,
                           size_t size,
                           int preserve_contexts);
void qw_jp2_mq_reset_contexts(QwJp2MqDecoder *decoder);
int qw_jp2_mq_decode(QwJp2MqDecoder *decoder,
                     uint8_t context,
                     uint8_t *symbol);

#endif
