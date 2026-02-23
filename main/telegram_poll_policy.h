#ifndef TELEGRAM_POLL_POLICY_H
#define TELEGRAM_POLL_POLICY_H

#include "config.h"

// Return Telegram long-poll timeout (seconds) for a given LLM backend.
int telegram_poll_timeout_for_backend(llm_backend_t backend);

#endif // TELEGRAM_POLL_POLICY_H
