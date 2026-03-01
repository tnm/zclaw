#include "tools_handlers.h"
#include "config.h"
#include "memory.h"
#include "nvs_keys.h"
#include "ota.h"
#include "ratelimit.h"
#include "cron.h"
#include "user_tools.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    DIAG_SCOPE_QUICK = 0,
    DIAG_SCOPE_RUNTIME,
    DIAG_SCOPE_MEMORY,
    DIAG_SCOPE_RATES,
    DIAG_SCOPE_TIME,
    DIAG_SCOPE_ALL,
} diag_scope_t;

static void format_uptime(int64_t uptime_us, char *buf, size_t buf_len)
{
    uint64_t total_s;
    uint64_t days;
    uint64_t hours;
    uint64_t minutes;
    uint64_t seconds;

    if (!buf || buf_len == 0) {
        return;
    }

    if (uptime_us <= 0) {
        snprintf(buf, buf_len, "unknown");
        return;
    }

    total_s = (uint64_t)uptime_us / 1000000ULL;
    days = total_s / 86400ULL;
    total_s %= 86400ULL;
    hours = total_s / 3600ULL;
    total_s %= 3600ULL;
    minutes = total_s / 60ULL;
    seconds = total_s % 60ULL;

    if (days > 0) {
        snprintf(buf, buf_len,
                 "%llud %02lluh %02llum %02llus",
                 (unsigned long long)days,
                 (unsigned long long)hours,
                 (unsigned long long)minutes,
                 (unsigned long long)seconds);
        return;
    }
    if (hours > 0) {
        snprintf(buf, buf_len,
                 "%lluh %02llum %02llus",
                 (unsigned long long)hours,
                 (unsigned long long)minutes,
                 (unsigned long long)seconds);
        return;
    }
    if (minutes > 0) {
        snprintf(buf, buf_len,
                 "%llum %02llus",
                 (unsigned long long)minutes,
                 (unsigned long long)seconds);
        return;
    }

    snprintf(buf, buf_len, "%llus", (unsigned long long)seconds);
}

static unsigned diag_fragmentation_percent(uint32_t free_heap, uint32_t largest_block)
{
    if (free_heap == 0 || largest_block >= free_heap) {
        return 0;
    }
    return 100U - (unsigned)((largest_block * 100U) / free_heap);
}

static uint32_t diag_get_free_heap_size(void)
{
#ifdef TEST_BUILD
    return 0;
#else
    return (uint32_t)esp_get_free_heap_size();
#endif
}

static uint32_t diag_get_minimum_free_heap_size(void)
{
#ifdef TEST_BUILD
    return 0;
#else
    return (uint32_t)esp_get_minimum_free_heap_size();
#endif
}

static uint32_t diag_get_largest_heap_block(void)
{
#ifdef TEST_BUILD
    return 0;
#else
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#endif
}

static bool diagnostics_scope_from_text(const char *scope_text, diag_scope_t *scope)
{
    if (!scope_text || !scope) {
        return false;
    }

    if (strcmp(scope_text, "quick") == 0) {
        *scope = DIAG_SCOPE_QUICK;
        return true;
    }
    if (strcmp(scope_text, "runtime") == 0) {
        *scope = DIAG_SCOPE_RUNTIME;
        return true;
    }
    if (strcmp(scope_text, "memory") == 0) {
        *scope = DIAG_SCOPE_MEMORY;
        return true;
    }
    if (strcmp(scope_text, "rates") == 0) {
        *scope = DIAG_SCOPE_RATES;
        return true;
    }
    if (strcmp(scope_text, "time") == 0) {
        *scope = DIAG_SCOPE_TIME;
        return true;
    }
    if (strcmp(scope_text, "all") == 0) {
        *scope = DIAG_SCOPE_ALL;
        return true;
    }

    return false;
}

static bool parse_diagnostics_args(const cJSON *input,
                                   diag_scope_t *scope_out,
                                   bool *verbose_out,
                                   char *result,
                                   size_t result_len)
{
    const cJSON *scope_json;
    const cJSON *verbose_json;

    *scope_out = DIAG_SCOPE_QUICK;
    *verbose_out = false;

    if (!input) {
        return true;
    }

    scope_json = cJSON_GetObjectItemCaseSensitive((cJSON *)input, "scope");
    if (scope_json) {
        if (!cJSON_IsString(scope_json) || !scope_json->valuestring || scope_json->valuestring[0] == '\0') {
            snprintf(result, result_len, "Error: scope must be one of quick|runtime|memory|rates|time|all");
            return false;
        }
        if (!diagnostics_scope_from_text(scope_json->valuestring, scope_out)) {
            snprintf(result, result_len, "Error: unknown scope '%s' (use quick|runtime|memory|rates|time|all)",
                     scope_json->valuestring);
            return false;
        }
    }

    verbose_json = cJSON_GetObjectItemCaseSensitive((cJSON *)input, "verbose");
    if (verbose_json) {
        if (!cJSON_IsBool(verbose_json)) {
            snprintf(result, result_len, "Error: verbose must be boolean");
            return false;
        }
        *verbose_out = cJSON_IsTrue(verbose_json);
    }

    return true;
}

bool tools_get_version_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input;
    snprintf(result, result_len, "zclaw v%s", ota_get_version());
    return true;
}

bool tools_get_health_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input;

    // Get heap info
    uint32_t free_heap = diag_get_free_heap_size();
    uint32_t min_heap = diag_get_minimum_free_heap_size();

    // Get rate limit info
    int requests_hour = ratelimit_get_requests_this_hour();
    int requests_day = ratelimit_get_requests_today();

    // Get time sync status
    bool time_synced = cron_is_time_synced();
    char timezone_posix[TIMEZONE_MAX_LEN];
    char timezone_abbrev[16];
    cron_get_timezone(timezone_posix, sizeof(timezone_posix));
    cron_get_timezone_abbrev(timezone_abbrev, sizeof(timezone_abbrev));

    snprintf(result, result_len,
             "Health: OK | "
             "Heap: %lu free, %lu min | "
             "Requests: %d/hr, %d/day | "
             "Time: %s | "
             "TZ: %s (%s) | "
             "Version: %s",
             (unsigned long)free_heap,
             (unsigned long)min_heap,
             requests_hour,
             requests_day,
             time_synced ? "synced" : "not synced",
             timezone_posix,
             timezone_abbrev,
             ota_get_version());

    return true;
}

bool tools_get_diagnostics_handler(const cJSON *input, char *result, size_t result_len)
{
    uint32_t free_heap = diag_get_free_heap_size();
    uint32_t min_heap = diag_get_minimum_free_heap_size();
    uint32_t largest_heap = diag_get_largest_heap_block();
    unsigned fragmentation_pct = diag_fragmentation_percent(free_heap, largest_heap);
    int requests_hour = ratelimit_get_requests_this_hour();
    int requests_day = ratelimit_get_requests_today();
    bool time_synced = cron_is_time_synced();
    char timezone_posix[TIMEZONE_MAX_LEN];
    char timezone_abbrev[16];
    char boot_count[16] = "unknown";
    char uptime_text[48];
    int64_t uptime_us = esp_timer_get_time();
    diag_scope_t scope = DIAG_SCOPE_QUICK;
    bool verbose = false;

    if (!parse_diagnostics_args(input, &scope, &verbose, result, result_len)) {
        return false;
    }

    cron_get_timezone(timezone_posix, sizeof(timezone_posix));
    cron_get_timezone_abbrev(timezone_abbrev, sizeof(timezone_abbrev));
    (void)memory_get(NVS_KEY_BOOT_COUNT, boot_count, sizeof(boot_count));
    format_uptime(uptime_us, uptime_text, sizeof(uptime_text));

    switch (scope) {
        case DIAG_SCOPE_RUNTIME:
            if (verbose) {
                snprintf(result, result_len,
                         "Runtime diagnostics:\n"
                         "- Uptime: %s (%llu us)\n"
                         "- Boot count: %s\n"
                         "- Version: %s",
                         uptime_text,
                         (unsigned long long)((uptime_us > 0) ? uptime_us : 0),
                         boot_count,
                         ota_get_version());
            } else {
                snprintf(result, result_len,
                         "Runtime: uptime=%s | boot_count=%s | version=%s",
                         uptime_text,
                         boot_count,
                         ota_get_version());
            }
            return true;
        case DIAG_SCOPE_MEMORY:
            if (verbose) {
                snprintf(result, result_len,
                         "Memory diagnostics:\n"
                         "- Heap free: %lu bytes\n"
                         "- Heap min: %lu bytes\n"
                         "- Heap largest block: %lu bytes\n"
                         "- Fragmentation hint: %u%%",
                         (unsigned long)free_heap,
                         (unsigned long)min_heap,
                         (unsigned long)largest_heap,
                         fragmentation_pct);
            } else {
                snprintf(result, result_len,
                         "Memory: free=%lu | min=%lu | largest=%lu | frag~%u%%",
                         (unsigned long)free_heap,
                         (unsigned long)min_heap,
                         (unsigned long)largest_heap,
                         fragmentation_pct);
            }
            return true;
        case DIAG_SCOPE_RATES:
            snprintf(result, result_len,
                     "Rates: requests=%d/hr, %d/day",
                     requests_hour,
                     requests_day);
            return true;
        case DIAG_SCOPE_TIME:
            if (verbose) {
                snprintf(result, result_len,
                         "Time diagnostics:\n"
                         "- Sync: %s\n"
                         "- Timezone (POSIX): %s\n"
                         "- Timezone (abbr): %s",
                         time_synced ? "synced" : "not synced",
                         timezone_posix,
                         timezone_abbrev);
            } else {
                snprintf(result, result_len,
                         "Time: %s | tz=%s (%s)",
                         time_synced ? "synced" : "not synced",
                         timezone_posix,
                         timezone_abbrev);
            }
            return true;
        case DIAG_SCOPE_ALL:
            if (verbose) {
                snprintf(result, result_len,
                         "Diagnostics:\n"
                         "- Uptime: %s (%llu us)\n"
                         "- Heap: free=%lu min=%lu largest=%lu frag~%u%%\n"
                         "- Requests: %d/hr, %d/day\n"
                         "- Time sync: %s\n"
                         "- Timezone: %s (%s)\n"
                         "- Boot count: %s\n"
                         "- Version: %s",
                         uptime_text,
                         (unsigned long long)((uptime_us > 0) ? uptime_us : 0),
                         (unsigned long)free_heap,
                         (unsigned long)min_heap,
                         (unsigned long)largest_heap,
                         fragmentation_pct,
                         requests_hour,
                         requests_day,
                         time_synced ? "synced" : "not synced",
                         timezone_posix,
                         timezone_abbrev,
                         boot_count,
                         ota_get_version());
            } else {
                snprintf(result, result_len,
                         "Diagnostics:\n"
                         "- Uptime: %s\n"
                         "- Heap: free=%lu min=%lu largest=%lu frag~%u%%\n"
                         "- Requests: %d/hr, %d/day\n"
                         "- Time sync: %s\n"
                         "- Timezone: %s (%s)\n"
                         "- Boot count: %s\n"
                         "- Version: %s",
                         uptime_text,
                         (unsigned long)free_heap,
                         (unsigned long)min_heap,
                         (unsigned long)largest_heap,
                         fragmentation_pct,
                         requests_hour,
                         requests_day,
                         time_synced ? "synced" : "not synced",
                         timezone_posix,
                         timezone_abbrev,
                         boot_count,
                         ota_get_version());
            }
            return true;
        case DIAG_SCOPE_QUICK:
        default:
            snprintf(result, result_len,
                     "Diag: uptime=%s | heap=%lu/%lu/%lu | req=%d/hr,%d/day | time=%s | tz=%s (%s) | boot=%s | v=%s",
                     uptime_text,
                     (unsigned long)free_heap,
                     (unsigned long)min_heap,
                     (unsigned long)largest_heap,
                     requests_hour,
                     requests_day,
                     time_synced ? "synced" : "not synced",
                     timezone_posix,
                     timezone_abbrev,
                     boot_count,
                     ota_get_version());
            return true;
    }
}

bool tools_create_tool_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *name_json = cJSON_GetObjectItem(input, "name");
    cJSON *desc_json = cJSON_GetObjectItem(input, "description");
    cJSON *action_json = cJSON_GetObjectItem(input, "action");

    if (!name_json || !cJSON_IsString(name_json)) {
        snprintf(result, result_len, "Error: 'name' required (string, no spaces)");
        return false;
    }
    if (!desc_json || !cJSON_IsString(desc_json)) {
        snprintf(result, result_len, "Error: 'description' required (short description)");
        return false;
    }
    if (!action_json || !cJSON_IsString(action_json)) {
        snprintf(result, result_len, "Error: 'action' required (what to do when called)");
        return false;
    }

    const char *name = name_json->valuestring;
    const char *description = desc_json->valuestring;
    const char *action = action_json->valuestring;

    // Validate name: no spaces, alphanumeric + underscore
    for (size_t i = 0; name[i]; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            snprintf(result, result_len, "Error: name must be alphanumeric/underscore, no spaces");
            return false;
        }
    }

    if (user_tools_create(name, description, action)) {
        snprintf(result, result_len, "Created tool '%s': %s", name, description);
        return true;
    }

    snprintf(result, result_len, "Error: failed to create tool (duplicate or limit reached)");
    return false;
}

bool tools_list_user_tools_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input;
    user_tools_list(result, result_len);
    return true;
}

bool tools_delete_user_tool_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *name_json = cJSON_GetObjectItem(input, "name");

    if (!name_json || !cJSON_IsString(name_json)) {
        snprintf(result, result_len, "Error: 'name' required");
        return false;
    }

    if (user_tools_delete(name_json->valuestring)) {
        snprintf(result, result_len, "Deleted tool '%s'", name_json->valuestring);
        return true;
    }

    snprintf(result, result_len, "Tool '%s' not found", name_json->valuestring);
    return true;
}
