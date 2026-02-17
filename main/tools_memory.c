#include "tools_handlers.h"
#include "config.h"
#include "memory.h"
#include "memory_keys.h"
#include "tools_common.h"
#include "esp_err.h"
#include "nvs.h"
#include <stdio.h>

bool tools_memory_set_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *key_json = cJSON_GetObjectItem(input, "key");
    cJSON *value_json = cJSON_GetObjectItem(input, "value");

    if (!key_json || !cJSON_IsString(key_json)) {
        snprintf(result, result_len, "Error: 'key' required (string)");
        return false;
    }
    if (!value_json || !cJSON_IsString(value_json)) {
        snprintf(result, result_len, "Error: 'value' required (string)");
        return false;
    }

    const char *key = key_json->valuestring;
    const char *value = value_json->valuestring;

    // Validate key format
    if (!tools_validate_nvs_key(key, result, result_len)) {
        return false;
    }

    if (!tools_validate_user_memory_key(key, result, result_len)) {
        return false;
    }

    if (memory_keys_is_sensitive(key)) {
        snprintf(result, result_len, "Error: cannot modify system key '%s'", key);
        return false;
    }

    // Validate value
    if (!tools_validate_string_input(value, NVS_MAX_VALUE_LEN, result, result_len)) {
        return false;
    }

    esp_err_t err = memory_set(key, value);
    if (err == ESP_OK) {
        snprintf(result, result_len, "Saved: %s = %s", key, value);
        return true;
    }
    snprintf(result, result_len, "Error: %s", esp_err_to_name(err));
    return false;
}

bool tools_memory_get_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *key_json = cJSON_GetObjectItem(input, "key");

    if (!key_json || !cJSON_IsString(key_json)) {
        snprintf(result, result_len, "Error: 'key' required (string)");
        return false;
    }

    const char *key = key_json->valuestring;

    // Validate key format
    if (!tools_validate_nvs_key(key, result, result_len)) {
        return false;
    }

    if (!tools_validate_user_memory_key(key, result, result_len)) {
        return false;
    }

    // Block access to sensitive keys
    if (memory_keys_is_sensitive(key)) {
        snprintf(result, result_len, "Error: cannot access system key '%s'", key);
        return false;
    }

    char value[NVS_MAX_VALUE_LEN + 1];

    if (memory_get(key, value, sizeof(value))) {
        snprintf(result, result_len, "%s = %s", key, value);
    } else {
        snprintf(result, result_len, "Key '%s' not found", key);
    }
    return true;
}

bool tools_memory_list_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input;

    if (!result || result_len == 0) {
        return false;
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        snprintf(result, result_len, "No stored keys");
        return true;
    }

    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find_in_handle(handle, NVS_TYPE_STR, &it);

    char *ptr = result;
    size_t remaining = result_len;
    int count = 0;

    if (!tools_append_fmt(&ptr, &remaining, "Stored keys: ")) {
        if (it) {
            nvs_release_iterator(it);
        }
        nvs_close(handle);
        result[result_len - 1] = '\0';
        return true;
    }

    while (err == ESP_OK && it != NULL && remaining > 20) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        if (!memory_keys_is_user_key(info.key)) {
            err = nvs_entry_next(&it);
            continue;
        }

        // Skip sensitive system keys
        if (memory_keys_is_sensitive(info.key)) {
            err = nvs_entry_next(&it);
            continue;
        }

        if (count > 0) {
            if (!tools_append_fmt(&ptr, &remaining, ", ")) {
                break;
            }
        }

        if (!tools_append_fmt(&ptr, &remaining, "%s", info.key)) {
            break;
        }
        count++;

        err = nvs_entry_next(&it);
    }

    if (it) {
        nvs_release_iterator(it);
    }
    nvs_close(handle);

    if (count == 0) {
        snprintf(result, result_len, "No stored keys");
    }
    return true;
}

bool tools_memory_delete_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *key_json = cJSON_GetObjectItem(input, "key");

    if (!key_json || !cJSON_IsString(key_json)) {
        snprintf(result, result_len, "Error: 'key' required (string)");
        return false;
    }

    const char *key = key_json->valuestring;

    // Validate key format
    if (!tools_validate_nvs_key(key, result, result_len)) {
        return false;
    }

    if (!tools_validate_user_memory_key(key, result, result_len)) {
        return false;
    }

    // Block deletion of sensitive keys
    if (memory_keys_is_sensitive(key)) {
        snprintf(result, result_len, "Error: cannot delete system key '%s'", key);
        return false;
    }

    esp_err_t err = memory_delete(key);
    if (err == ESP_OK) {
        snprintf(result, result_len, "Deleted: %s", key);
        return true;
    }
    snprintf(result, result_len, "Key not found: %s", key);
    return true;
}
