#ifndef MESSAGES_H
#define MESSAGES_H

#include "config.h"

// Shared queue payload for local channel and inbound agent messages.
typedef struct {
    char text[CHANNEL_RX_BUF_SIZE];
} channel_msg_t;

// Shared queue payload for outbound local channel responses.
typedef struct {
    char text[CHANNEL_TX_BUF_SIZE];
} channel_output_msg_t;

// Shared queue payload for outbound Telegram messages.
typedef enum {
    TELEGRAM_MSG_TEXT = 0,
    TELEGRAM_MSG_TYPING = 1,
} telegram_msg_kind_t;

typedef struct {
    telegram_msg_kind_t kind;
    char text[TELEGRAM_MAX_MSG_LEN];
} telegram_msg_t;

#endif // MESSAGES_H
