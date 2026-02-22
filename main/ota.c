#include "ota.h"
#include "config.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "ota";
static bool s_pending_verify = false;

// Current firmware version (set at compile time)
#ifndef ZCLAW_VERSION
#define ZCLAW_VERSION "2.4.1"
#endif

esp_err_t ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGE(TAG, "Failed to get running partition");
        return ESP_FAIL;
    }

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            s_pending_verify = true;
            ESP_LOGW(TAG, "Image pending verification; will mark valid after stable boot window");
        }
    }

    ESP_LOGI(TAG, "Running from: %s (v%s)", running->label, ZCLAW_VERSION);
    return ESP_OK;
}

const char *ota_get_version(void)
{
    return ZCLAW_VERSION;
}

esp_err_t ota_mark_valid(void)
{
    return esp_ota_mark_app_valid_cancel_rollback();
}

esp_err_t ota_mark_valid_if_pending(void)
{
    if (!s_pending_verify) {
        return ESP_OK;
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        s_pending_verify = false;
    }
    return err;
}

bool ota_is_pending_verify(void)
{
    return s_pending_verify;
}

esp_err_t ota_rollback(void)
{
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    // This should not return if successful
    return err;
}
