#include "cron.h"
#include "config.h"
#include "cron_utils.h"
#include "memory.h"
#include "messages.h"
#include "nvs_keys.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "cron";

static QueueHandle_t s_agent_queue;
static cron_entry_t s_entries[CRON_MAX_ENTRIES];
static bool s_time_synced = false;
static SemaphoreHandle_t s_entries_mutex = NULL;
static char s_timezone[TIMEZONE_MAX_LEN] = DEFAULT_TIMEZONE_POSIX;
typedef struct {
    uint8_t id;
    char action[CRON_MAX_ACTION_LEN];
} pending_cron_fire_t;
// Keep pending actions in static storage; allocating this on cron task stack
// can exceed CRON_TASK_STACK_SIZE on smaller targets (e.g. ESP32-C6).
static pending_cron_fire_t s_pending_fires[CRON_MAX_ENTRIES];

static bool entries_lock(TickType_t timeout_ticks)
{
    if (!s_entries_mutex) {
        return false;
    }
    return xSemaphoreTake(s_entries_mutex, timeout_ticks) == pdTRUE;
}

static void entries_unlock(void)
{
    if (s_entries_mutex) {
        xSemaphoreGive(s_entries_mutex);
    }
}

static bool timezone_string_is_valid(const char *tz)
{
    if (!tz) {
        return false;
    }

    size_t len = strlen(tz);
    if (len == 0 || len >= TIMEZONE_MAX_LEN) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        char c = tz[i];
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }

    return true;
}

static esp_err_t apply_timezone(const char *timezone_posix, bool persist_to_nvs)
{
    if (!timezone_string_is_valid(timezone_posix)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (setenv("TZ", timezone_posix, 1) != 0) {
        ESP_LOGE(TAG, "Failed to set TZ environment");
        return ESP_FAIL;
    }
    tzset();

    strncpy(s_timezone, timezone_posix, sizeof(s_timezone) - 1);
    s_timezone[sizeof(s_timezone) - 1] = '\0';
    ESP_LOGI(TAG, "Timezone applied: %s", s_timezone);

    if (persist_to_nvs) {
        esp_err_t err = memory_set(NVS_KEY_TIMEZONE, s_timezone);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist timezone: %s", esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;
}

static void load_timezone_from_nvs(void)
{
    char stored_tz[TIMEZONE_MAX_LEN] = {0};
    if (memory_get(NVS_KEY_TIMEZONE, stored_tz, sizeof(stored_tz)) &&
        timezone_string_is_valid(stored_tz)) {
        if (apply_timezone(stored_tz, false) == ESP_OK) {
            return;
        }
    }

    ESP_LOGI(TAG, "Using default timezone: %s", DEFAULT_TIMEZONE_POSIX);
    if (apply_timezone(DEFAULT_TIMEZONE_POSIX, false) != ESP_OK) {
        strncpy(s_timezone, DEFAULT_TIMEZONE_POSIX, sizeof(s_timezone) - 1);
        s_timezone[sizeof(s_timezone) - 1] = '\0';
    }
}

// NTP sync callback
static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "NTP time synchronized");
    s_time_synced = true;
}

// Initialize NTP
static void init_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    config.sync_cb = time_sync_notification_cb;
    esp_netif_sntp_init(&config);
}

// Load entries from NVS
static void load_entries(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE_CRON, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    for (int i = 0; i < CRON_MAX_ENTRIES; i++) {
        char key[16];
        snprintf(key, sizeof(key), "cron_%d", i);

        size_t size = sizeof(cron_entry_t);
        if (nvs_get_blob(handle, key, &s_entries[i], &size) != ESP_OK) {
            s_entries[i].id = 0;  // Mark as empty
        }
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded cron entries from NVS");
}

// Save a single entry to NVS
static esp_err_t save_entry(int index)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CRON, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open cron NVS: %s", esp_err_to_name(err));
        return err;
    }

    char key[16];
    snprintf(key, sizeof(key), "cron_%d", index);

    if (s_entries[index].id == 0) {
        err = nvs_erase_key(handle, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_blob(handle, key, &s_entries[index], sizeof(cron_entry_t));
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist cron entry slot %d: %s", index, esp_err_to_name(err));
    }

    return err;
}

esp_err_t cron_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));

    if (!s_entries_mutex) {
        s_entries_mutex = xSemaphoreCreateMutex();
        if (!s_entries_mutex) {
            ESP_LOGE(TAG, "Failed to create cron mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    load_entries();
    load_timezone_from_nvs();
    init_sntp();

    // Wait for time sync (with timeout)
    int wait_ms = 0;
    while (!s_time_synced && wait_ms < NTP_SYNC_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_ms += 100;
    }

    if (s_time_synced) {
        char time_str[32];
        cron_get_time_str(time_str, sizeof(time_str));
        ESP_LOGI(TAG, "Current time: %s", time_str);
    } else {
        ESP_LOGW(TAG, "NTP sync timed out - clock-based schedules may be delayed");
    }

    return ESP_OK;
}

esp_err_t cron_set_timezone(const char *timezone_posix)
{
    return apply_timezone(timezone_posix, true);
}

void cron_get_timezone(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }
    strncpy(buf, s_timezone, buf_len - 1);
    buf[buf_len - 1] = '\0';
}

void cron_get_timezone_abbrev(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (strftime(buf, buf_len, "%Z", &timeinfo) == 0 || buf[0] == '\0') {
        strncpy(buf, "UTC", buf_len - 1);
        buf[buf_len - 1] = '\0';
    }
}

bool cron_is_time_synced(void)
{
    return s_time_synced;
}

void cron_get_time_str(char *buf, size_t buf_len)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(buf, buf_len, "%Y-%m-%d %H:%M:%S", &timeinfo);
}

uint8_t cron_set(cron_type_t type, uint16_t interval_or_hour, uint8_t minute, const char *action)
{
    uint8_t created_id = 0;

    if (!action || action[0] == '\0') {
        ESP_LOGE(TAG, "Cannot create cron entry: empty action");
        return 0;
    }

    if (type == CRON_TYPE_PERIODIC && !cron_validate_periodic_interval((int)interval_or_hour)) {
        ESP_LOGE(TAG, "Invalid periodic interval: %u", interval_or_hour);
        return 0;
    }
    if (type == CRON_TYPE_ONCE && !cron_validate_periodic_interval((int)interval_or_hour)) {
        ESP_LOGE(TAG, "Invalid once delay: %u", interval_or_hour);
        return 0;
    }
    if (type == CRON_TYPE_DAILY && !cron_validate_daily_time((int)interval_or_hour, (int)minute)) {
        ESP_LOGE(TAG, "Invalid daily time: %u:%u", interval_or_hour, minute);
        return 0;
    }

    if (!entries_lock(pdMS_TO_TICKS(1000))) {
        ESP_LOGE(TAG, "Failed to lock cron entries");
        return 0;
    }

    // Find empty slot and gather used IDs
    int slot = -1;
    uint8_t used_ids[CRON_MAX_ENTRIES] = {0};
    size_t used_count = 0;

    for (int i = 0; i < CRON_MAX_ENTRIES; i++) {
        if (s_entries[i].id == 0 && slot == -1) {
            slot = i;
        }
        if (s_entries[i].id != 0 && used_count < CRON_MAX_ENTRIES) {
            used_ids[used_count++] = s_entries[i].id;
        }
    }

    if (slot == -1) {
        ESP_LOGE(TAG, "No free cron slots");
        goto out;
    }

    uint8_t next_id = cron_next_entry_id(used_ids, used_count);
    if (next_id == 0) {
        ESP_LOGE(TAG, "No free cron IDs");
        goto out;
    }

    cron_entry_t *entry = &s_entries[slot];
    entry->id = next_id;
    entry->type = type;
    entry->enabled = true;
    entry->last_run = 0;

    if (type == CRON_TYPE_PERIODIC || type == CRON_TYPE_ONCE) {
        entry->interval_minutes = interval_or_hour;
        entry->hour = 0;
        entry->minute = 0;
    } else {
        entry->interval_minutes = 0;
        entry->hour = interval_or_hour;
        entry->minute = minute;
    }

    if (type == CRON_TYPE_ONCE) {
        time_t now;
        time(&now);
        if (now >= 0) {
            entry->last_run = (uint32_t)now;
        }
    }

    strncpy(entry->action, action, CRON_MAX_ACTION_LEN - 1);
    entry->action[CRON_MAX_ACTION_LEN - 1] = '\0';

    if (save_entry(slot) != ESP_OK) {
        memset(entry, 0, sizeof(*entry));
        goto out;
    }

    created_id = entry->id;
    ESP_LOGI(TAG, "Created cron entry %d: type=%d action=%s", entry->id, type, action);

out:
    entries_unlock();
    return created_id;
}

void cron_list(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        snprintf(buf, buf_len, "[]");
        return;
    }

    if (!entries_lock(pdMS_TO_TICKS(1000))) {
        cJSON_Delete(arr);
        snprintf(buf, buf_len, "[]");
        return;
    }

    char timezone_posix[TIMEZONE_MAX_LEN];
    char timezone_abbrev[16];
    cron_get_timezone(timezone_posix, sizeof(timezone_posix));
    cron_get_timezone_abbrev(timezone_abbrev, sizeof(timezone_abbrev));

    for (int i = 0; i < CRON_MAX_ENTRIES; i++) {
        if (s_entries[i].id == 0) continue;

        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            ESP_LOGE(TAG, "Failed to allocate cron list entry object");
            entries_unlock();
            cJSON_Delete(arr);
            snprintf(buf, buf_len, "[]");
            return;
        }

        bool ok = true;
        ok &= cJSON_AddNumberToObject(obj, "id", s_entries[i].id) != NULL;

        const char *type_str = "unknown";
        switch (s_entries[i].type) {
            case CRON_TYPE_PERIODIC: type_str = "periodic"; break;
            case CRON_TYPE_DAILY: type_str = "daily"; break;
            case CRON_TYPE_CONDITION: type_str = "condition"; break;
            case CRON_TYPE_ONCE: type_str = "once"; break;
        }
        ok &= cJSON_AddStringToObject(obj, "type", type_str) != NULL;

        if (s_entries[i].type == CRON_TYPE_PERIODIC) {
            ok &= cJSON_AddNumberToObject(obj, "interval_minutes", s_entries[i].interval_minutes) != NULL;
        } else if (s_entries[i].type == CRON_TYPE_ONCE) {
            ok &= cJSON_AddNumberToObject(obj, "delay_minutes", s_entries[i].interval_minutes) != NULL;
        } else {
            char time_str[8];
            snprintf(time_str, sizeof(time_str), "%02d:%02d", s_entries[i].hour, s_entries[i].minute);
            ok &= cJSON_AddStringToObject(obj, "time", time_str) != NULL;
        }

        ok &= cJSON_AddStringToObject(obj, "action", s_entries[i].action) != NULL;
        ok &= cJSON_AddBoolToObject(obj, "enabled", s_entries[i].enabled) != NULL;
        ok &= cJSON_AddStringToObject(obj, "timezone", timezone_posix) != NULL;
        ok &= cJSON_AddStringToObject(obj, "timezone_abbrev", timezone_abbrev) != NULL;
        if (!ok) {
            ESP_LOGE(TAG, "Failed to build cron list entry JSON");
            cJSON_Delete(obj);
            entries_unlock();
            cJSON_Delete(arr);
            snprintf(buf, buf_len, "[]");
            return;
        }

        cJSON_AddItemToArray(arr, obj);
    }

    entries_unlock();

    char *json = cJSON_PrintUnformatted(arr);
    if (json) {
        strncpy(buf, json, buf_len - 1);
        buf[buf_len - 1] = '\0';
        free(json);
    } else {
        snprintf(buf, buf_len, "[]");
    }

    cJSON_Delete(arr);
}

esp_err_t cron_delete(uint8_t id)
{
    if (!entries_lock(pdMS_TO_TICKS(1000))) {
        ESP_LOGE(TAG, "Failed to lock cron entries");
        return ESP_ERR_TIMEOUT;
    }

    for (int i = 0; i < CRON_MAX_ENTRIES; i++) {
        if (s_entries[i].id == id) {
            cron_entry_t previous_entry = s_entries[i];
            s_entries[i].id = 0;
            esp_err_t err = save_entry(i);
            if (err != ESP_OK) {
                s_entries[i] = previous_entry;
                entries_unlock();
                return err;
            }
            ESP_LOGI(TAG, "Deleted cron entry %d", id);
            entries_unlock();
            return ESP_OK;
        }
    }
    entries_unlock();
    return ESP_ERR_NOT_FOUND;
}

// Check and fire due entries
static void check_entries(void)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    int pending_count = 0;

    if (!entries_lock(pdMS_TO_TICKS(1000))) {
        ESP_LOGW(TAG, "Skipping cron check: lock timeout");
        return;
    }

    for (int i = 0; i < CRON_MAX_ENTRIES; i++) {
        cron_entry_t *entry = &s_entries[i];
        if (entry->id == 0 || !entry->enabled) continue;

        bool should_fire = false;

        if (entry->type == CRON_TYPE_PERIODIC) {
            uint32_t interval_seconds = entry->interval_minutes * 60;
            if (now - entry->last_run >= interval_seconds) {
                should_fire = true;
            }
        } else if (entry->type == CRON_TYPE_ONCE) {
            uint32_t delay_seconds = entry->interval_minutes * 60;
            time_t created_at = (time_t)entry->last_run;
            if (now >= created_at && (uint32_t)(now - created_at) >= delay_seconds) {
                should_fire = true;
            }
        } else if (entry->type == CRON_TYPE_DAILY && s_time_synced) {
            // Check if current time matches
            if (timeinfo.tm_hour == entry->hour && timeinfo.tm_min == entry->minute) {
                // Only fire once per minute
                uint32_t minute_start = now - timeinfo.tm_sec;
                if (entry->last_run < minute_start) {
                    should_fire = true;
                }
            }
        }

        if (should_fire) {
            if (pending_count < CRON_MAX_ENTRIES) {
                s_pending_fires[pending_count].id = entry->id;
                strncpy(s_pending_fires[pending_count].action, entry->action, sizeof(s_pending_fires[pending_count].action) - 1);
                s_pending_fires[pending_count].action[sizeof(s_pending_fires[pending_count].action) - 1] = '\0';
                pending_count++;
            }

            if (entry->type == CRON_TYPE_ONCE) {
                uint8_t fired_id = entry->id;
                entry->id = 0;
                if (save_entry(i) != ESP_OK) {
                    entry->id = fired_id;
                    ESP_LOGW(TAG, "Failed to clear one-shot cron %d after firing", fired_id);
                }
            } else {
                entry->last_run = now;
                if (save_entry(i) != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to persist run timestamp for cron %d", entry->id);
                }
            }
        }
    }

    entries_unlock();

    for (int i = 0; i < pending_count; i++) {
        ESP_LOGI(TAG, "Firing cron %d: %s", s_pending_fires[i].id, s_pending_fires[i].action);

        // Push action to agent queue
        channel_msg_t msg;
        snprintf(msg.text, sizeof(msg.text), "[CRON %d] %s", s_pending_fires[i].id, s_pending_fires[i].action);

        if (xQueueSend(s_agent_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Agent queue full, cron action dropped");
        }
    }
}

// Cron task
static void cron_task(void *arg)
{
    (void)arg;

    while (1) {
        check_entries();
        vTaskDelay(pdMS_TO_TICKS(CRON_CHECK_INTERVAL_MS));
    }
}

esp_err_t cron_start(QueueHandle_t agent_input_queue)
{
    if (!agent_input_queue) {
        ESP_LOGE(TAG, "Invalid queue for cron startup");
        return ESP_ERR_INVALID_ARG;
    }

    s_agent_queue = agent_input_queue;

    if (xTaskCreate(cron_task, "cron", CRON_TASK_STACK_SIZE, NULL,
                    CRON_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create cron task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Cron task started");
    return ESP_OK;
}
