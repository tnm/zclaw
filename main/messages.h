#ifndef MESSAGES_H
#define MESSAGES_H

#include "config.h"

// Shared queue payload for local channel and inbound agent messages.
typedef struct {
    char text[CHANNEL_RX_BUF_SIZE];
} channel_msg_t;

// Shared queue payload for outbound Telegram messages.
typedef struct {
    char text[TELEGRAM_MAX_MSG_LEN];
} telegram_msg_t;

#endif // MESSAGES_H
