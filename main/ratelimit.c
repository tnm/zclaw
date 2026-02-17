#include "ratelimit.h"
#include "config.h"
#include "memory.h"
#include "nvs_keys.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "ratelimit";

// Simple sliding window using hour/day counters
static int s_requests_this_hour = 0;
static int s_requests_today = 0;
static int s_last_hour = -1;
static int s_last_day = -1;

void ratelimit_init(void)
{
    // Load persisted daily count
    char buf[16];
    if (memory_get(NVS_KEY_RL_DAILY, buf, sizeof(buf))) {
        s_requests_today = atoi(buf);
    }
    if (memory_get(NVS_KEY_RL_DAY, buf, sizeof(buf))) {
        s_last_day = atoi(buf);
    }

    ESP_LOGI(TAG, "Rate limiter initialized: %d requests today", s_requests_today);
}

static void update_time_window(void)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    int current_hour = timeinfo.tm_hour;
    int current_day = timeinfo.tm_yday;

    // Reset hourly counter if hour changed
    if (current_hour != s_last_hour) {
        s_requests_this_hour = 0;
        s_last_hour = current_hour;
    }

    // Reset daily counter if day changed
    if (current_day != s_last_day) {
        s_requests_today = 0;
        s_last_day = current_day;

        // Persist the new day
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", current_day);
        memory_set(NVS_KEY_RL_DAY, buf);
        memory_set(NVS_KEY_RL_DAILY, "0");

        ESP_LOGI(TAG, "Daily rate limit reset");
    }
}

bool ratelimit_check(char *reason, size_t reason_len)
{
#if !RATELIMIT_ENABLED
    return true;
#endif

    update_time_window();

    // Check hourly limit
    if (s_requests_this_hour >= RATELIMIT_MAX_PER_HOUR) {
        snprintf(reason, reason_len,
                 "Rate limited: %d/%d requests this hour. Try again later.",
                 s_requests_this_hour, RATELIMIT_MAX_PER_HOUR);
        ESP_LOGW(TAG, "Hourly rate limit exceeded");
        return false;
    }

    // Check daily limit
    if (s_requests_today >= RATELIMIT_MAX_PER_DAY) {
        snprintf(reason, reason_len,
                 "Daily limit reached: %d/%d requests today. Resets at midnight.",
                 s_requests_today, RATELIMIT_MAX_PER_DAY);
        ESP_LOGW(TAG, "Daily rate limit exceeded");
        return false;
    }

    return true;
}

void ratelimit_record_request(void)
{
    update_time_window();

    s_requests_this_hour++;
    s_requests_today++;

    // Persist daily count periodically
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s_requests_today);
    memory_set(NVS_KEY_RL_DAILY, buf);

    ESP_LOGD(TAG, "Request recorded: %d/hour, %d/day",
             s_requests_this_hour, s_requests_today);
}

int ratelimit_get_requests_today(void)
{
    return s_requests_today;
}

int ratelimit_get_requests_this_hour(void)
{
    return s_requests_this_hour;
}

void ratelimit_reset_daily(void)
{
    s_requests_today = 0;
    s_requests_this_hour = 0;
    memory_set(NVS_KEY_RL_DAILY, "0");
    ESP_LOGI(TAG, "Rate limits manually reset");
}
