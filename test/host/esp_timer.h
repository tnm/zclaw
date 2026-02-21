#ifndef ESP_TIMER_H
#define ESP_TIMER_H

#include <stdint.h>
#include <stddef.h>
#include <sys/time.h>

static inline int64_t esp_timer_get_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000000LL) + (int64_t)tv.tv_usec;
}

#endif // ESP_TIMER_H
