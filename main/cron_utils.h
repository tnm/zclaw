#ifndef CRON_UTILS_H
#define CRON_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool cron_validate_periodic_interval(int interval_minutes);
bool cron_validate_daily_time(int hour, int minute);
uint8_t cron_next_entry_id(const uint8_t *used_ids, size_t used_count);

#endif // CRON_UTILS_H
