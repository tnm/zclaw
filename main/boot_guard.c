#include "boot_guard.h"
#include "memory.h"
#include "nvs_keys.h"

#include <stdio.h>
#include <stdlib.h>

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

int boot_guard_get_persisted_count(void)
{
    char buf[16] = {0};
    if (memory_get(NVS_KEY_BOOT_COUNT, buf, sizeof(buf))) {
        return atoi(buf);
    }
    return 0;
}

esp_err_t boot_guard_set_persisted_count(int count)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", count);
    return memory_set(NVS_KEY_BOOT_COUNT, buf);
}
