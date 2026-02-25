#ifndef MESSAGES_H
#define MESSAGES_H

#include "config.h"
#include <stdint.h>

typedef enum {
    MSG_SOURCE_CHANNEL = 0,
    MSG_SOURCE_TELEGRAM = 1,
    MSG_SOURCE_CRON = 2,
    MSG_SOURCE_VOICE = 3,
} message_source_t;

// Shared queue payload for local channel and inbound agent messages.
typedef struct {
    char text[CHANNEL_RX_BUF_SIZE];
    message_source_t source;
    int64_t chat_id;
} channel_msg_t;

// Shared queue payload for outbound local channel responses.
typedef struct {
    char text[CHANNEL_TX_BUF_SIZE];
} channel_output_msg_t;

typedef struct {
    char text[TELEGRAM_MAX_MSG_LEN];
    int64_t chat_id;
} telegram_msg_t;

#endif // MESSAGES_H
