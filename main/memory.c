#include "memory.h"
#include "config.h"
#include "security.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_flash_encrypt.h"
#include "esp_partition.h"

static const char *TAG = "memory";

static const char *log_value_for_key(const char *key, const char *value)
{
    if (security_key_is_sensitive(key)) {
        return "<redacted>";
    }
    return value ? value : "";
}

static esp_err_t init_encrypted_nvs(void)
{
    const esp_partition_t *nvs_key_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, NULL);

    if (!nvs_key_part) {
        ESP_LOGW(TAG, "NVS keys partition not found, using unencrypted NVS");
        return ESP_ERR_NOT_FOUND;
    }

    nvs_sec_cfg_t nvs_sec_cfg;
    esp_err_t err = nvs_flash_read_security_cfg(nvs_key_part, &nvs_sec_cfg);

    if (err == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
        ESP_LOGI(TAG, "Generating NVS encryption keys");
        err = nvs_flash_generate_keys(nvs_key_part, &nvs_sec_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to generate NVS keys: %s", esp_err_to_name(err));
            return err;
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read NVS security cfg: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_flash_secure_init(&nvs_sec_cfg);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_secure_init(&nvs_sec_cfg);
    }

    return err;
}

esp_err_t memory_init(void)
{
    esp_err_t err;

    // Check if flash encryption is enabled
    if (esp_flash_encryption_enabled()) {
        ESP_LOGI(TAG, "Flash encryption enabled, using encrypted NVS");
        err = init_encrypted_nvs();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Encrypted NVS initialized");
            return ESP_OK;
        }
        // Fall through to unencrypted if encrypted init fails
        ESP_LOGW(TAG, "Encrypted NVS init failed, falling back to unencrypted");
    }

    // Standard unencrypted NVS init
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized");
    }
    return err;
}

esp_err_t memory_set(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set '%s': %s", key, esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Stored: %s = %s", key, log_value_for_key(key, value));
    }

    nvs_close(handle);
    return err;
}

bool memory_get(const char *key, char *value, size_t max_len)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t required_size = max_len;
    err = nvs_get_str(handle, key, value, &required_size);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Retrieved: %s = %s", key, log_value_for_key(key, value));
        return true;
    }
    return false;
}

esp_err_t memory_delete(const char *key)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(handle, key);
    if (err == ESP_OK) {
        esp_err_t commit_err = nvs_commit(handle);
        if (commit_err == ESP_OK) {
            ESP_LOGI(TAG, "Deleted: %s", key);
        } else {
            ESP_LOGE(TAG, "Failed to commit delete '%s': %s", key, esp_err_to_name(commit_err));
            err = commit_err;
        }
    }

    nvs_close(handle);
    return err;
}
