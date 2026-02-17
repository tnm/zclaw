#ifndef TELEGRAM_UPDATE_H
#define TELEGRAM_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

// Best-effort parser for recovering the max update_id from partially received JSON.
// Returns true and sets max_id_out when at least one non-negative update_id is found.
bool telegram_extract_max_update_id(const char *buf, int64_t *max_id_out);

#endif // TELEGRAM_UPDATE_H
