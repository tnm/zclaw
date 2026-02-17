#include "cron.h"
#include "config.h"
#include "cron_utils.h"
#include "memory.h"
#include "messages.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "cron";

static QueueHandle_t s_agent_queue;
static cron_entry_t s_entries[CRON_MAX_ENTRIES];
static bool s_time_synced = false;

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
static void save_entry(int index)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE_CRON, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    char key[16];
    snprintf(key, sizeof(key), "cron_%d", index);

    if (s_entries[index].id == 0) {
        nvs_erase_key(handle, key);
    } else {
        nvs_set_blob(handle, key, &s_entries[index], sizeof(cron_entry_t));
    }

    nvs_commit(handle);
    nvs_close(handle);
}

esp_err_t cron_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    load_entries();
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
    if (!action || action[0] == '\0') {
        ESP_LOGE(TAG, "Cannot create cron entry: empty action");
        return 0;
    }

    if (type == CRON_TYPE_PERIODIC && !cron_validate_periodic_interval((int)interval_or_hour)) {
        ESP_LOGE(TAG, "Invalid periodic interval: %u", interval_or_hour);
        return 0;
    }
    if (type == CRON_TYPE_DAILY && !cron_validate_daily_time((int)interval_or_hour, (int)minute)) {
        ESP_LOGE(TAG, "Invalid daily time: %u:%u", interval_or_hour, minute);
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
        return 0;
    }

    uint8_t next_id = cron_next_entry_id(used_ids, used_count);
    if (next_id == 0) {
        ESP_LOGE(TAG, "No free cron IDs");
        return 0;
    }

    cron_entry_t *entry = &s_entries[slot];
    entry->id = next_id;
    entry->type = type;
    entry->enabled = true;
    entry->last_run = 0;

    if (type == CRON_TYPE_PERIODIC) {
        entry->interval_minutes = interval_or_hour;
        entry->hour = 0;
        entry->minute = 0;
    } else {
        entry->interval_minutes = 0;
        entry->hour = interval_or_hour;
        entry->minute = minute;
    }

    strncpy(entry->action, action, CRON_MAX_ACTION_LEN - 1);
    entry->action[CRON_MAX_ACTION_LEN - 1] = '\0';

    save_entry(slot);

    ESP_LOGI(TAG, "Created cron entry %d: type=%d action=%s", entry->id, type, action);
    return entry->id;
}

void cron_list(char *buf, size_t buf_len)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < CRON_MAX_ENTRIES; i++) {
        if (s_entries[i].id == 0) continue;

        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id", s_entries[i].id);

        const char *type_str = "unknown";
        switch (s_entries[i].type) {
            case CRON_TYPE_PERIODIC: type_str = "periodic"; break;
            case CRON_TYPE_DAILY: type_str = "daily"; break;
            case CRON_TYPE_CONDITION: type_str = "condition"; break;
        }
        cJSON_AddStringToObject(obj, "type", type_str);

        if (s_entries[i].type == CRON_TYPE_PERIODIC) {
            cJSON_AddNumberToObject(obj, "interval_minutes", s_entries[i].interval_minutes);
        } else {
            char time_str[8];
            snprintf(time_str, sizeof(time_str), "%02d:%02d", s_entries[i].hour, s_entries[i].minute);
            cJSON_AddStringToObject(obj, "time", time_str);
        }

        cJSON_AddStringToObject(obj, "action", s_entries[i].action);
        cJSON_AddBoolToObject(obj, "enabled", s_entries[i].enabled);

        cJSON_AddItemToArray(arr, obj);
    }

    char *json = cJSON_PrintUnformatted(arr);
    if (json) {
        strncpy(buf, json, buf_len - 1);
        buf[buf_len - 1] = '\0';
        free(json);
    } else {
        strncpy(buf, "[]", buf_len);
    }

    cJSON_Delete(arr);
}

bool cron_delete(uint8_t id)
{
    for (int i = 0; i < CRON_MAX_ENTRIES; i++) {
        if (s_entries[i].id == id) {
            s_entries[i].id = 0;
            save_entry(i);
            ESP_LOGI(TAG, "Deleted cron entry %d", id);
            return true;
        }
    }
    return false;
}

// Check and fire due entries
static void check_entries(void)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    for (int i = 0; i < CRON_MAX_ENTRIES; i++) {
        cron_entry_t *entry = &s_entries[i];
        if (entry->id == 0 || !entry->enabled) continue;

        bool should_fire = false;

        if (entry->type == CRON_TYPE_PERIODIC) {
            uint32_t interval_seconds = entry->interval_minutes * 60;
            if (now - entry->last_run >= interval_seconds) {
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
            entry->last_run = now;
            save_entry(i);

            ESP_LOGI(TAG, "Firing cron %d: %s", entry->id, entry->action);

            // Push action to agent queue
            channel_msg_t msg;
            snprintf(msg.text, sizeof(msg.text), "[CRON %d] %s", entry->id, entry->action);

            if (xQueueSend(s_agent_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "Agent queue full, cron action dropped");
            }
        }
    }
}

// Cron task
static void cron_task(void *arg)
{
    ESP_LOGI(TAG, "Cron task started");

    while (1) {
        check_entries();
        vTaskDelay(pdMS_TO_TICKS(CRON_CHECK_INTERVAL_MS));
    }
}

void cron_start(QueueHandle_t agent_input_queue)
{
    s_agent_queue = agent_input_queue;

    xTaskCreate(cron_task, "cron", CRON_TASK_STACK_SIZE, NULL,
                CRON_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "Cron task started");
}
