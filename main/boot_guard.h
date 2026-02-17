#ifndef BOOT_GUARD_H
#define BOOT_GUARD_H

#include <stdbool.h>

// Returns the next persisted boot count for the current boot attempt.
int boot_guard_next_count(int current_count);

// Returns true when the current boot should enter safe mode.
bool boot_guard_should_enter_safe_mode(int current_count, int max_failures);

#endif // BOOT_GUARD_H
