#include "boot_guard.h"

int boot_guard_next_count(int current_count)
{
    if (current_count < 0) {
        current_count = 0;
    }
    return current_count + 1;
}

bool boot_guard_should_enter_safe_mode(int current_count, int max_failures)
{
    if (max_failures <= 0) {
        return false;
    }
    return boot_guard_next_count(current_count) >= max_failures;
}
