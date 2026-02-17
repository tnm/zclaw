#include "cron_utils.h"

bool cron_validate_periodic_interval(int interval_minutes)
{
    return interval_minutes >= 1 && interval_minutes <= 1440;
}

bool cron_validate_daily_time(int hour, int minute)
{
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

uint8_t cron_next_entry_id(const uint8_t *used_ids, size_t used_count)
{
    bool taken[256] = {0};

    if (used_ids) {
        for (size_t i = 0; i < used_count; i++) {
            if (used_ids[i] != 0) {
                taken[used_ids[i]] = true;
            }
        }
    }

    for (int id = 1; id <= 255; id++) {
        if (!taken[id]) {
            return (uint8_t)id;
        }
    }

    return 0;
}
