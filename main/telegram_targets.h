#ifndef TELEGRAM_TARGETS_H
#define TELEGRAM_TARGETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void telegram_targets_clear(void);
bool telegram_targets_set_from_string(const char *input);
bool telegram_targets_has_any(void);
size_t telegram_targets_count(void);
int64_t telegram_targets_primary_chat_id(void);
bool telegram_targets_is_authorized(int64_t incoming_chat_id);
int64_t telegram_targets_resolve_target_chat_id(int64_t requested_chat_id);

#endif // TELEGRAM_TARGETS_H
