#ifndef BOOT_GUARD_H
#define BOOT_GUARD_H

#include <stdbool.h>
#include "esp_err.h"

// Returns the next persisted boot count for the current boot attempt.
int boot_guard_next_count(int current_count);

// Returns true when the current boot should enter safe mode.
bool boot_guard_should_enter_safe_mode(int current_count, int max_failures);

// Reads boot counter from persistent storage. Returns 0 if unset/unreadable.
int boot_guard_get_persisted_count(void);

// Persists boot counter to storage.
esp_err_t boot_guard_set_persisted_count(int count);

#endif // BOOT_GUARD_H
