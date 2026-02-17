#include "user_tools.h"
#include "tools.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "user_tools";

// In-memory cache of user tools
static user_tool_t s_tools[MAX_DYNAMIC_TOOLS];
static int s_tool_count = 0;

// NVS key format: "ut_<index>" for tool data
// "ut_count" for total count

static bool name_conflicts_with_builtin_tool(const char *name)
{
    if (!name) {
        return false;
    }

    int builtin_count = 0;
    const tool_def_t *builtin_tools = tools_get_all(&builtin_count);
    if (!builtin_tools || builtin_count <= 0) {
        return false;
    }

    for (int i = 0; i < builtin_count; i++) {
        if (strcmp(builtin_tools[i].name, name) == 0) {
            return true;
        }
    }

    return false;
}

static esp_err_t save_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_TOOLS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing");
        return err;
    }

    // Save count
    err = nvs_set_u8(handle, "ut_count", (uint8_t)s_tool_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist tool count: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    // Save each tool as a blob
    for (int i = 0; i < s_tool_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "ut_%d", i);
        err = nvs_set_blob(handle, key, &s_tools[i], sizeof(user_tool_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist tool slot %d: %s", i, esp_err_to_name(err));
            nvs_close(handle);
            return err;
        }
    }

    // Clear any remaining old slots
    for (int i = s_tool_count; i < MAX_DYNAMIC_TOOLS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "ut_%d", i);
        err = nvs_erase_key(handle, key);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed clearing stale tool slot %d: %s", i, esp_err_to_name(err));
            nvs_close(handle);
            return err;
        }
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit user tools: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);
    return ESP_OK;
}

static void load_from_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE_TOOLS, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "No saved user tools");
        return;
    }

    uint8_t count = 0;
    if (nvs_get_u8(handle, "ut_count", &count) != ESP_OK) {
        nvs_close(handle);
        return;
    }

    if (count > MAX_DYNAMIC_TOOLS) {
        count = MAX_DYNAMIC_TOOLS;
    }

    for (int i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "ut_%d", i);
        size_t len = sizeof(user_tool_t);
        if (nvs_get_blob(handle, key, &s_tools[i], &len) == ESP_OK) {
            s_tool_count++;
            ESP_LOGI(TAG, "Loaded user tool: %s", s_tools[i].name);
        }
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded %d user tools", s_tool_count);
}

void user_tools_init(void)
{
    s_tool_count = 0;
    memset(s_tools, 0, sizeof(s_tools));
    load_from_nvs();
}

bool user_tools_create(const char *name, const char *description, const char *action)
{
    if (!name || !description || !action) {
        return false;
    }

    if (strlen(name) == 0 || strlen(name) >= TOOL_NAME_MAX_LEN) {
        ESP_LOGW(TAG, "Invalid tool name length");
        return false;
    }

    if (name_conflicts_with_builtin_tool(name)) {
        ESP_LOGW(TAG, "Tool '%s' conflicts with built-in tool name", name);
        return false;
    }

    // Check for duplicate
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            ESP_LOGW(TAG, "Tool '%s' already exists", name);
            return false;
        }
    }

    if (s_tool_count >= MAX_DYNAMIC_TOOLS) {
        ESP_LOGW(TAG, "Max user tools reached (%d)", MAX_DYNAMIC_TOOLS);
        return false;
    }

    user_tool_t *tool = &s_tools[s_tool_count];
    strncpy(tool->name, name, TOOL_NAME_MAX_LEN - 1);
    tool->name[TOOL_NAME_MAX_LEN - 1] = '\0';
    strncpy(tool->description, description, TOOL_DESC_MAX_LEN - 1);
    tool->description[TOOL_DESC_MAX_LEN - 1] = '\0';
    strncpy(tool->action, action, CRON_MAX_ACTION_LEN - 1);
    tool->action[CRON_MAX_ACTION_LEN - 1] = '\0';

    s_tool_count++;
    esp_err_t save_err = save_to_nvs();
    if (save_err != ESP_OK) {
        s_tool_count--;
        memset(&s_tools[s_tool_count], 0, sizeof(user_tool_t));
        ESP_LOGE(TAG, "Failed to persist user tool '%s': %s", name, esp_err_to_name(save_err));
        return false;
    }

    ESP_LOGI(TAG, "Created user tool: %s", name);
    return true;
}

bool user_tools_delete(const char *name)
{
    if (!name) {
        return false;
    }

    user_tool_t previous_tools[MAX_DYNAMIC_TOOLS];
    memcpy(previous_tools, s_tools, sizeof(previous_tools));
    int previous_count = s_tool_count;

    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            // Shift remaining tools down
            for (int j = i; j < s_tool_count - 1; j++) {
                s_tools[j] = s_tools[j + 1];
            }
            s_tool_count--;
            memset(&s_tools[s_tool_count], 0, sizeof(user_tool_t));
            esp_err_t save_err = save_to_nvs();
            if (save_err != ESP_OK) {
                memcpy(s_tools, previous_tools, sizeof(s_tools));
                s_tool_count = previous_count;
                ESP_LOGE(TAG, "Failed to persist deletion of '%s': %s",
                         name, esp_err_to_name(save_err));
                return false;
            }
            ESP_LOGI(TAG, "Deleted user tool: %s", name);
            return true;
        }
    }

    return false;
}

int user_tools_get_all(user_tool_t *tools, int max_count)
{
    int count = s_tool_count < max_count ? s_tool_count : max_count;
    if (tools && count > 0) {
        memcpy(tools, s_tools, count * sizeof(user_tool_t));
    }
    return count;
}

const user_tool_t *user_tools_find(const char *name)
{
    if (!name) {
        return NULL;
    }

    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            return &s_tools[i];
        }
    }

    return NULL;
}

int user_tools_count(void)
{
    return s_tool_count;
}

void user_tools_list(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }

    if (s_tool_count == 0) {
        snprintf(buf, buf_len, "No user tools defined");
        return;
    }

    char *ptr = buf;
    size_t remaining = buf_len;
    int written = snprintf(ptr, remaining, "User tools (%d):", s_tool_count);
    if (written > 0 && (size_t)written < remaining) {
        ptr += written;
        remaining -= written;
    }

    for (int i = 0; i < s_tool_count && remaining > 20; i++) {
        written = snprintf(ptr, remaining, "\n  %s - %s",
                          s_tools[i].name, s_tools[i].description);
        if (written > 0 && (size_t)written < remaining) {
            ptr += written;
            remaining -= written;
        } else {
            break;
        }
    }
}
