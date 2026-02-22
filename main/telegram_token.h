#ifndef TELEGRAM_TOKEN_H
#define TELEGRAM_TOKEN_H

#include <stdbool.h>
#include <stddef.h>

// Extracts the numeric bot ID prefix from a Telegram token ("<bot_id>:<secret>").
// Returns false if token format is invalid or output buffer is too small.
bool telegram_extract_bot_id(const char *token, char *out, size_t out_len);

#endif // TELEGRAM_TOKEN_H
