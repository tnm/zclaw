#include "tools_handlers.h"
#include "cron.h"
#include "cron_utils.h"
#include "config.h"
#include "tools_common.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *alias;
    const char *posix;
} timezone_alias_t;

static const timezone_alias_t TZ_ALIASES[] = {
    {"UTC", "UTC0"},
    {"Etc/UTC", "UTC0"},
    {"GMT", "UTC0"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"US/Pacific", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"PST", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"PDT", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"PT", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"America/Denver", "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"US/Mountain", "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"MST", "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"MDT", "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"MT", "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"America/Chicago", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"US/Central", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"CST", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"CDT", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"CT", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"America/New_York", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"US/Eastern", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"EST", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"EDT", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"ET", "EST5EDT,M3.2.0/2,M11.1.0/2"},
};

static void trim_ascii_whitespace(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }

    const char *start = src;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }

    const char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }

    size_t len = (size_t)(end - start);
    if (len >= dst_len) {
        len = dst_len - 1;
    }
    if (len > 0) {
        memcpy(dst, start, len);
    }
    dst[len] = '\0';
}

static bool resolve_timezone_to_posix(
    const char *timezone_input,
    char *timezone_posix_out,
    size_t timezone_posix_out_len,
    char *error_out,
    size_t error_out_len)
{
    char trimmed[TIMEZONE_MAX_LEN];
    trim_ascii_whitespace(timezone_input, trimmed, sizeof(trimmed));

    if (!tools_validate_string_input(trimmed, TIMEZONE_MAX_LEN - 1, error_out, error_out_len)) {
        return false;
    }

    if (trimmed[0] == '\0') {
        snprintf(error_out, error_out_len, "Error: timezone must be non-empty");
        return false;
    }

    for (size_t i = 0; i < sizeof(TZ_ALIASES) / sizeof(TZ_ALIASES[0]); i++) {
        if (strcasecmp(trimmed, TZ_ALIASES[i].alias) == 0) {
            snprintf(timezone_posix_out, timezone_posix_out_len, "%s", TZ_ALIASES[i].posix);
            return true;
        }
    }

    if (strchr(trimmed, '/') != NULL) {
        snprintf(
            error_out,
            error_out_len,
            "Error: timezone name not recognized. Use UTC, America/Los_Angeles, America/Denver, America/Chicago, America/New_York, or a POSIX TZ string."
        );
        return false;
    }

    for (size_t i = 0; trimmed[i] != '\0'; i++) {
        char c = trimmed[i];
        if (c == ' ' || c == '\t') {
            snprintf(error_out, error_out_len, "Error: timezone must not contain spaces");
            return false;
        }
    }

    snprintf(timezone_posix_out, timezone_posix_out_len, "%s", trimmed);
    return true;
}

bool tools_cron_set_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *type_json = cJSON_GetObjectItem(input, "type");
    cJSON *action_json = cJSON_GetObjectItem(input, "action");

    if (!type_json || !cJSON_IsString(type_json)) {
        snprintf(result, result_len, "Error: 'type' required (periodic/daily/once)");
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
    } else if (strcmp(type_str, "once") == 0) {
        type = CRON_TYPE_ONCE;
        cJSON *delay = cJSON_GetObjectItem(input, "delay_minutes");
        if (!delay || !cJSON_IsNumber(delay)) {
            snprintf(result, result_len, "Error: 'delay_minutes' required for once");
            return false;
        }
        if (!cron_validate_periodic_interval(delay->valueint)) {
            snprintf(result, result_len, "Error: delay_minutes must be 1-1440");
            return false;
        }
        interval_or_hour = (uint16_t)delay->valueint;
    } else {
        snprintf(result, result_len, "Error: type must be 'periodic', 'daily', or 'once'");
        return false;
    }

    uint8_t id = cron_set(type, interval_or_hour, minute, action);
    if (id > 0) {
        if (type == CRON_TYPE_PERIODIC) {
            snprintf(result, result_len, "Created schedule #%d: every %d min → %s",
                     id, interval_or_hour, action);
        } else if (type == CRON_TYPE_DAILY) {
            char timezone_abbrev[16];
            cron_get_timezone_abbrev(timezone_abbrev, sizeof(timezone_abbrev));
            snprintf(result, result_len, "Created schedule #%d: daily at %02d:%02d %s → %s",
                     id, interval_or_hour, minute, timezone_abbrev, action);
        } else {
            snprintf(result, result_len, "Created schedule #%d: once in %d min → %s",
                     id, interval_or_hour, action);
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

    esp_err_t err = cron_delete(id_json->valueint);
    if (err == ESP_OK) {
        snprintf(result, result_len, "Deleted schedule #%d", id_json->valueint);
        return true;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        snprintf(result, result_len, "Schedule #%d not found", id_json->valueint);
        return true;
    }

    snprintf(result, result_len, "Error: failed to delete schedule #%d (%s)",
             id_json->valueint, esp_err_to_name(err));
    return false;
}

bool tools_get_time_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input;
    char time_str[32];
    char timezone_posix[TIMEZONE_MAX_LEN];
    char timezone_abbrev[16];

    cron_get_timezone(timezone_posix, sizeof(timezone_posix));
    cron_get_timezone_abbrev(timezone_abbrev, sizeof(timezone_abbrev));

    if (cron_is_time_synced()) {
        cron_get_time_str(time_str, sizeof(time_str));
        snprintf(result, result_len, "%s %s (TZ=%s)", time_str, timezone_abbrev, timezone_posix);
    } else {
        snprintf(result, result_len, "Time not synced (no NTP). Configured TZ=%s (%s)",
                 timezone_posix, timezone_abbrev);
    }
    return true;
}

bool tools_set_timezone_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *tz_json = cJSON_GetObjectItem(input, "timezone");
    if (!tz_json || !cJSON_IsString(tz_json)) {
        snprintf(result, result_len, "Error: 'timezone' required (string)");
        return false;
    }

    char timezone_posix[TIMEZONE_MAX_LEN];
    if (!resolve_timezone_to_posix(
            tz_json->valuestring,
            timezone_posix,
            sizeof(timezone_posix),
            result,
            result_len)) {
        return false;
    }

    esp_err_t err = cron_set_timezone(timezone_posix);
    if (err != ESP_OK) {
        snprintf(result, result_len, "Error: failed to set timezone (%s)", esp_err_to_name(err));
        return false;
    }

    char timezone_abbrev[16];
    char time_str[32];
    cron_get_timezone_abbrev(timezone_abbrev, sizeof(timezone_abbrev));
    if (cron_is_time_synced()) {
        cron_get_time_str(time_str, sizeof(time_str));
        snprintf(result, result_len,
                 "Timezone set to %s (%s). Current local time: %s %s",
                 timezone_posix, timezone_abbrev, time_str, timezone_abbrev);
    } else {
        snprintf(result, result_len,
                 "Timezone set to %s (%s). Time not synced yet (NTP pending).",
                 timezone_posix, timezone_abbrev);
    }
    return true;
}

bool tools_get_timezone_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input;
    char timezone_posix[TIMEZONE_MAX_LEN];
    char timezone_abbrev[16];
    cron_get_timezone(timezone_posix, sizeof(timezone_posix));
    cron_get_timezone_abbrev(timezone_abbrev, sizeof(timezone_abbrev));
    snprintf(result, result_len, "Timezone: %s (%s)", timezone_posix, timezone_abbrev);
    return true;
}
