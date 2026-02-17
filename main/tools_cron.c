#include "tools_handlers.h"
#include "cron.h"
#include "cron_utils.h"
#include "config.h"
#include "tools_common.h"
#include <stdio.h>
#include <string.h>

bool tools_cron_set_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *type_json = cJSON_GetObjectItem(input, "type");
    cJSON *action_json = cJSON_GetObjectItem(input, "action");

    if (!type_json || !cJSON_IsString(type_json)) {
        snprintf(result, result_len, "Error: 'type' required (periodic/daily)");
        return false;
    }
    if (!action_json || !cJSON_IsString(action_json)) {
        snprintf(result, result_len, "Error: 'action' required (what to do)");
        return false;
    }

    const char *type_str = type_json->valuestring;
    const char *action = action_json->valuestring;

    // Validate action string
    if (!tools_validate_string_input(action, CRON_MAX_ACTION_LEN, result, result_len)) {
        return false;
    }

    cron_type_t type;
    uint16_t interval_or_hour = 0;
    uint8_t minute = 0;

    if (strcmp(type_str, "periodic") == 0) {
        type = CRON_TYPE_PERIODIC;
        cJSON *interval = cJSON_GetObjectItem(input, "interval_minutes");
        if (!interval || !cJSON_IsNumber(interval)) {
            snprintf(result, result_len, "Error: 'interval_minutes' required for periodic");
            return false;
        }
        if (!cron_validate_periodic_interval(interval->valueint)) {
            snprintf(result, result_len, "Error: interval_minutes must be 1-1440");
            return false;
        }
        interval_or_hour = interval->valueint;
    } else if (strcmp(type_str, "daily") == 0) {
        type = CRON_TYPE_DAILY;
        cJSON *hour_json = cJSON_GetObjectItem(input, "hour");
        cJSON *min_json = cJSON_GetObjectItem(input, "minute");
        if (!hour_json || !cJSON_IsNumber(hour_json)) {
            snprintf(result, result_len, "Error: 'hour' required for daily (0-23)");
            return false;
        }
        if (min_json && !cJSON_IsNumber(min_json)) {
            snprintf(result, result_len, "Error: 'minute' must be a number (0-59)");
            return false;
        }
        int hour = hour_json->valueint;
        int minute_int = min_json ? min_json->valueint : 0;
        if (!cron_validate_daily_time(hour, minute_int)) {
            snprintf(result, result_len, "Error: daily time must be hour 0-23 and minute 0-59");
            return false;
        }
        interval_or_hour = (uint16_t)hour;
        minute = (uint8_t)minute_int;
    } else {
        snprintf(result, result_len, "Error: type must be 'periodic' or 'daily'");
        return false;
    }

    uint8_t id = cron_set(type, interval_or_hour, minute, action);
    if (id > 0) {
        if (type == CRON_TYPE_PERIODIC) {
            snprintf(result, result_len, "Created schedule #%d: every %d min → %s",
                     id, interval_or_hour, action);
        } else {
            snprintf(result, result_len, "Created schedule #%d: daily at %02d:%02d → %s",
                     id, interval_or_hour, minute, action);
        }
        return true;
    }
    snprintf(result, result_len, "Error: no free schedule slots");
    return false;
}

bool tools_cron_list_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input;
    cron_list(result, result_len);
    return true;
}

bool tools_cron_delete_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *id_json = cJSON_GetObjectItem(input, "id");

    if (!id_json || !cJSON_IsNumber(id_json)) {
        snprintf(result, result_len, "Error: 'id' required (number)");
        return false;
    }

    if (cron_delete(id_json->valueint)) {
        snprintf(result, result_len, "Deleted schedule #%d", id_json->valueint);
        return true;
    }
    snprintf(result, result_len, "Schedule #%d not found", id_json->valueint);
    return true;
}

bool tools_get_time_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input;
    if (cron_is_time_synced()) {
        cron_get_time_str(result, result_len);
    } else {
        snprintf(result, result_len, "Time not synced (no NTP)");
    }
    return true;
}
