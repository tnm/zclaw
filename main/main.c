#include "config.h"
#include "memory.h"
#include "channel.h"
#include "agent.h"
#include "llm.h"
#include "tools.h"
#include "telegram.h"
#include "cron.h"
#include "ratelimit.h"
#include "ota.h"
#include "boot_guard.h"
#include "nvs_keys.h"
#include "messages.h"
#include "wifi_credentials.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#if CONFIG_ZCLAW_BLE_PROVISIONING
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#endif
#include <string.h>

static const char *TAG = "main";

// WiFi event group
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static bool s_safe_mode = false;
static uint8_t s_last_disconnect_reason = 0;
static bool s_in_ble_provisioning = false;
static bool s_wifi_handlers_registered = false;
static bool s_wifi_inited = false;
static bool s_sta_netif_created = false;
static esp_event_handler_instance_t s_instance_any_id;
static esp_event_handler_instance_t s_instance_got_ip;

#ifndef WIFI_REASON_BEACON_TIMEOUT
#define WIFI_REASON_BEACON_TIMEOUT 200
#endif
#ifndef WIFI_REASON_NO_AP_FOUND
#define WIFI_REASON_NO_AP_FOUND 201
#endif
#ifndef WIFI_REASON_AUTH_FAIL
#define WIFI_REASON_AUTH_FAIL 202
#endif
#ifndef WIFI_REASON_ASSOC_FAIL
#define WIFI_REASON_ASSOC_FAIL 203
#endif
#ifndef WIFI_REASON_HANDSHAKE_TIMEOUT
#define WIFI_REASON_HANDSHAKE_TIMEOUT 204
#endif

static const char *wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_LEAVE: return "AUTH_LEAVE";
        case WIFI_REASON_ASSOC_EXPIRE: return "ASSOC_EXPIRE";
        case WIFI_REASON_ASSOC_TOOMANY: return "ASSOC_TOOMANY";
        case WIFI_REASON_NOT_AUTHED: return "NOT_AUTHED";
        case WIFI_REASON_NOT_ASSOCED: return "NOT_ASSOCED";
        case WIFI_REASON_ASSOC_LEAVE: return "ASSOC_LEAVE";
        case WIFI_REASON_ASSOC_NOT_AUTHED: return "ASSOC_NOT_AUTHED";
        case WIFI_REASON_MIC_FAILURE: return "MIC_FAILURE";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "GROUP_KEY_UPDATE_TIMEOUT";
        case WIFI_REASON_802_1X_AUTH_FAILED: return "802_1X_AUTH_FAILED";
        case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
        default: return "UNKNOWN";
    }
}

static const char *wifi_disconnect_reason_hint(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
            return "SSID not found. Confirm exact SSID and ensure 2.4GHz is enabled.";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "Authentication failed. Re-run provisioning and double-check WiFi password.";
        case WIFI_REASON_802_1X_AUTH_FAILED:
            return "AP may require WPA2-Enterprise (802.1X), which is not supported by PSK provisioning.";
        case WIFI_REASON_ASSOC_FAIL:
            return "Association failed. Check router compatibility (WPA2/WPA3 mixed mode recommended).";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "AP not stable/reachable. Move device closer or reduce interference.";
        default:
            return NULL;
    }
}

static int wifi_channel_to_mhz(uint8_t channel)
{
    if (channel == 14) {
        return 2484;
    }
    if (channel >= 1 && channel <= 13) {
        return 2407 + (5 * channel);
    }
    if (channel >= 32 && channel <= 196) {
        return 5000 + (5 * channel);
    }
    return 0;
}

static const char *wifi_authmode_name(wifi_auth_mode_t mode)
{
    switch (mode) {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA_PSK";
        case WIFI_AUTH_WPA2_PSK: return "WPA2_PSK";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA_WPA2_PSK";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
        case WIFI_AUTH_WPA3_PSK: return "WPA3_PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
        case WIFI_AUTH_WAPI_PSK: return "WAPI_PSK";
        default: return "UNKNOWN";
    }
}

static void log_target_ap_scan(const char *ssid)
{
    wifi_scan_config_t scan_cfg = {0};
    uint16_t ap_num = 0;
    uint16_t record_count = 1;
    wifi_ap_record_t ap = {0};

    if (ssid == NULL || ssid[0] == '\0') {
        return;
    }

    scan_cfg.ssid = (uint8_t *)ssid;
    scan_cfg.show_hidden = true;

    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed before connect");
        return;
    }

    if (esp_wifi_scan_get_ap_num(&ap_num) != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan result count unavailable");
        return;
    }

    if (ap_num == 0) {
        ESP_LOGW(TAG, "WiFi scan: '%s' not visible on 2.4GHz", ssid);
        return;
    }

    if (esp_wifi_scan_get_ap_records(&record_count, &ap) != ESP_OK || record_count == 0) {
        ESP_LOGW(TAG, "WiFi scan: no AP record available for '%s'", ssid);
        return;
    }

    int mhz = wifi_channel_to_mhz(ap.primary);
    if (mhz > 0) {
        ESP_LOGI(TAG, "WiFi scan: ssid='%s' channel=%u freq=%dMHz auth=%s rssi=%d",
                 ssid, ap.primary, mhz, wifi_authmode_name(ap.authmode), ap.rssi);
    } else {
        ESP_LOGI(TAG, "WiFi scan: ssid='%s' channel=%u auth=%s rssi=%d",
                 ssid, ap.primary, wifi_authmode_name(ap.authmode), ap.rssi);
    }
}

static void fail_fast_startup(const char *component, esp_err_t err)
{
    ESP_LOGE(TAG, "Startup failure in %s: %s", component, esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void clear_boot_count(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(BOOT_SUCCESS_DELAY_MS));
    UBaseType_t start_hwm_words = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "boot_ok stack high-water mark at start: %u words",
             (unsigned)start_hwm_words);

    bool pending_before = ota_is_pending_verify();
    if (pending_before) {
        esp_err_t ota_err = ota_mark_valid_if_pending();
        if (ota_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to confirm OTA image: %s", esp_err_to_name(ota_err));
        } else {
            ESP_LOGI(TAG, "OTA image confirmed after stable boot window");
        }
    }

    esp_err_t boot_count_err = boot_guard_set_persisted_count(0);
    if (boot_count_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear boot counter: %s", esp_err_to_name(boot_count_err));
    } else {
        ESP_LOGI(TAG, "Boot counter cleared - system stable");
    }
    UBaseType_t end_hwm_words = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "boot_ok stack high-water mark before exit: %u words",
             (unsigned)end_hwm_words);
    vTaskDelete(NULL);
}

// WiFi event handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = event ? event->reason : 0;
        const char *reason_name = wifi_disconnect_reason_name(reason);
        const char *hint = wifi_disconnect_reason_hint(reason);

        s_last_disconnect_reason = reason;
        ESP_LOGW(TAG, "WiFi disconnected: reason=%u (%s)", reason, reason_name);
        if (hint) {
            ESP_LOGW(TAG, "WiFi hint: %s", hint);
        }

        if (s_in_ble_provisioning) {
            ESP_LOGW(TAG, "Waiting for new credentials via BLE provisioning");
            return;
        }

        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "WiFi retry %d/%d", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "WiFi connection failed after %d retries (last reason=%u: %s)",
                     WIFI_MAX_RETRY, s_last_disconnect_reason,
                     wifi_disconnect_reason_name(s_last_disconnect_reason));
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_stack_init(void)
{
    esp_err_t err;

    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
        if (!s_wifi_event_group) {
            return ESP_ERR_NO_MEM;
        }
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (!s_sta_netif_created) {
        if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
            if (esp_netif_create_default_wifi_sta() == NULL) {
                return ESP_FAIL;
            }
        }
        s_sta_netif_created = true;
    }

    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        s_wifi_inited = true;
    }

    if (!s_wifi_handlers_registered) {
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                  &wifi_event_handler, NULL, &s_instance_any_id);
        if (err != ESP_OK) {
            return err;
        }

        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                  &wifi_event_handler, NULL, &s_instance_got_ip);
        if (err != ESP_OK) {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_instance_any_id);
            return err;
        }
        s_wifi_handlers_registered = true;
    }

    return ESP_OK;
}

// Check factory reset button
static bool check_factory_reset(void)
{
    gpio_reset_pin(FACTORY_RESET_PIN);
    gpio_set_direction(FACTORY_RESET_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(FACTORY_RESET_PIN, GPIO_PULLUP_ONLY);

    // Check if button is held low
    if (gpio_get_level(FACTORY_RESET_PIN) == 0) {
        ESP_LOGW(TAG, "Factory reset button detected, hold for 5 seconds...");

        int held_ms = 0;
        while (gpio_get_level(FACTORY_RESET_PIN) == 0 && held_ms < FACTORY_RESET_HOLD_MS) {
            vTaskDelay(pdMS_TO_TICKS(100));
            held_ms += 100;
        }

        if (held_ms >= FACTORY_RESET_HOLD_MS) {
            ESP_LOGW(TAG, "Factory reset triggered!");
            nvs_flash_erase();
            ESP_LOGI(TAG, "NVS erased, restarting...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
            return true;
        }
    }
    return false;
}

static bool device_is_configured(void)
{
    char ssid[64] = {0};
    if (memory_get(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid)) && ssid[0] != '\0') {
        return true;
    }

#if defined(CONFIG_ZCLAW_WIFI_SSID)
    return CONFIG_ZCLAW_WIFI_SSID[0] != '\0';
#else
    return false;
#endif
}

#if !CONFIG_ZCLAW_BLE_PROVISIONING
static void print_provisioning_help(void)
{
    ESP_LOGE(TAG, "");
    ESP_LOGE(TAG, "========================================");
    ESP_LOGE(TAG, "  Device is not provisioned");
    ESP_LOGE(TAG, "========================================");
    ESP_LOGE(TAG, "Run on host:");
    ESP_LOGE(TAG, "  ./scripts/provision.sh --port <serial-port>");
    ESP_LOGE(TAG, "Then restart the board.");
    ESP_LOGE(TAG, "");
}
#endif

// Connect to WiFi using stored credentials
static bool wifi_connect_sta(void)
{
    char ssid[64] = {0};
    char pass[64] = {0};
    char wifi_error[96] = {0};

    if (!memory_get(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid)) || ssid[0] == '\0') {
#if defined(CONFIG_ZCLAW_WIFI_SSID)
        if (CONFIG_ZCLAW_WIFI_SSID[0] == '\0') {
            return false;
        }
        strncpy(ssid, CONFIG_ZCLAW_WIFI_SSID, sizeof(ssid) - 1);
        ssid[sizeof(ssid) - 1] = '\0';
#else
        return false;
#endif
    }

    if (!memory_get(NVS_KEY_WIFI_PASS, pass, sizeof(pass))) {
#if defined(CONFIG_ZCLAW_WIFI_PASSWORD)
        strncpy(pass, CONFIG_ZCLAW_WIFI_PASSWORD, sizeof(pass) - 1);
        pass[sizeof(pass) - 1] = '\0';
#else
        pass[0] = '\0';
#endif
    }

    if (!wifi_credentials_validate(ssid, pass, wifi_error, sizeof(wifi_error))) {
        ESP_LOGE(TAG, "Invalid WiFi credentials: %s", wifi_error);
        return false;
    }

    ESP_LOGI(TAG, "Loaded WiFi credentials: ssid='%s', password_len=%u",
             ssid, (unsigned)strlen(pass));

    esp_err_t init_err = wifi_stack_init();
    if (init_err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi stack init failed: %s", esp_err_to_name(init_err));
        return false;
    }
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;

    wifi_config_t wifi_config = {0};
    wifi_credentials_copy_to_sta_config(wifi_config.sta.ssid, wifi_config.sta.password, ssid, pass);
    // Allow WPA/WPA2/WPA3 PSK networks; open networks stay open-only.
    wifi_config.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Favor link stability for HTTPS-heavy workloads over power savings.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "WiFi power save disabled");

    log_target_ap_scan(ssid);
    ESP_LOGI(TAG, "Connecting to %s...", ssid);
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, portMAX_DELAY);

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

#if CONFIG_ZCLAW_BLE_PROVISIONING
static const char *ble_prov_fail_reason_name(wifi_prov_sta_fail_reason_t reason)
{
    switch (reason) {
        case WIFI_PROV_STA_AUTH_ERROR: return "AUTH_ERROR";
        case WIFI_PROV_STA_AP_NOT_FOUND: return "AP_NOT_FOUND";
        default: return "UNKNOWN";
    }
}

static void ble_prov_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != WIFI_PROV_EVENT) {
        return;
    }

    switch (event_id) {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "BLE provisioning started");
            break;
        case WIFI_PROV_CRED_RECV: {
            const wifi_sta_config_t *sta_cfg = (const wifi_sta_config_t *)event_data;
            char ssid[33] = {0};
            if (sta_cfg) {
                memcpy(ssid, sta_cfg->ssid, sizeof(sta_cfg->ssid));
            }
            ESP_LOGI(TAG, "BLE provisioning received credentials for ssid='%s'", ssid);
            break;
        }
        case WIFI_PROV_CRED_FAIL: {
            const wifi_prov_sta_fail_reason_t *reason = (const wifi_prov_sta_fail_reason_t *)event_data;
            wifi_prov_sta_fail_reason_t fail = reason ? *reason : WIFI_PROV_STA_AP_NOT_FOUND;
            ESP_LOGW(TAG, "BLE provisioning connect failed: %s", ble_prov_fail_reason_name(fail));
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "BLE provisioning WiFi credentials accepted");
            break;
        case WIFI_PROV_END:
            ESP_LOGI(TAG, "BLE provisioning service stopped");
            break;
        default:
            break;
    }
}

static void build_ble_service_name(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(out, out_len, "zclaw-%02X%02X%02X", mac[3], mac[4], mac[5]);
        return;
    }
    snprintf(out, out_len, "zclaw");
}

static bool wifi_provision_over_ble(void)
{
    esp_err_t err = wifi_stack_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi stack init failed for BLE provisioning: %s", esp_err_to_name(err));
        return false;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;

    wifi_prov_mgr_config_t prov_cfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BLE,
        .app_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };

    err = wifi_prov_mgr_init(prov_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_init failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_event_handler_instance_t prov_event_instance;
    err = esp_event_handler_instance_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                              &ble_prov_event_handler, NULL, &prov_event_instance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register BLE provisioning event handler: %s", esp_err_to_name(err));
        wifi_prov_mgr_deinit();
        return false;
    }

    bool provisioned = false;
    err = wifi_prov_mgr_is_provisioned(&provisioned);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_is_provisioned failed: %s", esp_err_to_name(err));
        esp_event_handler_instance_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, prov_event_instance);
        wifi_prov_mgr_deinit();
        return false;
    }

    if (provisioned) {
        ESP_LOGI(TAG, "NVS already has WiFi credentials; skipping BLE provisioning");
        esp_event_handler_instance_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, prov_event_instance);
        wifi_prov_mgr_deinit();
        return wifi_connect_sta();
    }

    char service_name[20] = {0};
    build_ble_service_name(service_name, sizeof(service_name));

    wifi_prov_security_t security = WIFI_PROV_SECURITY_0;
    const void *security_params = NULL;
    const char *pop_value = "";

#if defined(CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1) && defined(CONFIG_ZCLAW_BLE_PROV_POP)
    if (CONFIG_ZCLAW_BLE_PROV_POP[0] != '\0') {
        security = WIFI_PROV_SECURITY_1;
        security_params = CONFIG_ZCLAW_BLE_PROV_POP;
        pop_value = CONFIG_ZCLAW_BLE_PROV_POP;
    }
#endif

    ESP_LOGW(TAG, "No WiFi credentials found in NVS. Starting BLE provisioning...");
    ESP_LOGI(TAG, "BLE service name: %s", service_name);
    ESP_LOGI(TAG, "Use the Espressif 'ESP BLE Provisioning' app");
    ESP_LOGI(TAG, "QR payload: {\"ver\":\"v1\",\"name\":\"%s\",\"transport\":\"ble\",\"pop\":\"%s\"}",
             service_name, pop_value);

    s_in_ble_provisioning = true;
    err = wifi_prov_mgr_start_provisioning(security, security_params, service_name, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_start_provisioning failed: %s", esp_err_to_name(err));
        s_in_ble_provisioning = false;
        esp_event_handler_instance_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, prov_event_instance);
        wifi_prov_mgr_deinit();
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdTRUE, pdFALSE, portMAX_DELAY);
    bool connected = (bits & WIFI_CONNECTED_BIT) != 0;
    s_in_ble_provisioning = false;

    wifi_prov_mgr_stop_provisioning();
    wifi_prov_mgr_wait();
    esp_event_handler_instance_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, prov_event_instance);
    wifi_prov_mgr_deinit();

    if (!connected) {
        ESP_LOGE(TAG, "BLE provisioning ended without WiFi connection");
        return false;
    }

    ESP_LOGI(TAG, "BLE provisioning complete, WiFi connected");
    return true;
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  zclaw v%s", ota_get_version());
    ESP_LOGI(TAG, "  AI Agent on ESP32");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");

    // 1. Initialize NVS
    ESP_ERROR_CHECK(memory_init());

    // 2. Initialize OTA (check for pending rollback)
    ota_init();

    // 3. Check factory reset button
#if !CONFIG_ZCLAW_EMULATOR_MODE
    check_factory_reset();
#endif

    // 4. Boot loop protection
#if !CONFIG_ZCLAW_EMULATOR_MODE
    int boot_count = boot_guard_get_persisted_count();
    int next_boot_count = boot_guard_next_count(boot_count);
    esp_err_t boot_count_err = boot_guard_set_persisted_count(next_boot_count);
    if (boot_count_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist boot counter: %s", esp_err_to_name(boot_count_err));
    }

    if (boot_guard_should_enter_safe_mode(boot_count, MAX_BOOT_FAILURES)) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "========================================");
        ESP_LOGE(TAG, "  SAFE MODE - Too many boot failures");
        ESP_LOGE(TAG, "  Hold BOOT button for factory reset");
        ESP_LOGE(TAG, "========================================");
        ESP_LOGE(TAG, "");
        s_safe_mode = true;
    }
#endif

#if CONFIG_ZCLAW_EMULATOR_MODE
    ESP_LOGW(TAG, "Emulator mode enabled: skipping WiFi/NTP/Telegram startup");
#ifndef CONFIG_ZCLAW_STUB_LLM
    ESP_LOGW(TAG, "Stub LLM is disabled; without network, LLM requests may fail");
#endif

    ESP_ERROR_CHECK(llm_init());
    ratelimit_init();
    tools_init();
    channel_init();

    QueueHandle_t input_queue = xQueueCreate(INPUT_QUEUE_LENGTH, sizeof(channel_msg_t));
    QueueHandle_t channel_output_queue = xQueueCreate(OUTPUT_QUEUE_LENGTH, sizeof(channel_output_msg_t));
    if (!input_queue || !channel_output_queue) {
        ESP_LOGE(TAG, "Failed to create emulator queues");
        esp_restart();
    }

    esp_err_t startup_err = channel_start(input_queue, channel_output_queue);
    if (startup_err != ESP_OK) {
        fail_fast_startup("channel_start", startup_err);
    }

    startup_err = agent_start(input_queue, channel_output_queue, NULL);
    if (startup_err != ESP_OK) {
        fail_fast_startup("agent_start", startup_err);
    }

    channel_write("\r\nzclaw emulator ready. Type a message and press Enter.\r\n\r\n");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else

    // 4. Safe mode blocks normal startup
    if (s_safe_mode) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "========================================");
        ESP_LOGE(TAG, "  SAFE MODE - Too many boot failures");
        ESP_LOGE(TAG, "  Hold BOOT button for factory reset");
        ESP_LOGE(TAG, "========================================");
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "Recovery options:");
        ESP_LOGE(TAG, "  1) Hold BOOT for factory reset");
        ESP_LOGE(TAG, "  2) Reflash firmware and reprovision");
        ESP_LOGE(TAG, "");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    // 5. Connect to WiFi (or provision first if needed)
    bool wifi_ready = false;
    if (device_is_configured()) {
        wifi_ready = wifi_connect_sta();
    } else {
#if CONFIG_ZCLAW_BLE_PROVISIONING
        wifi_ready = wifi_provision_over_ble();
#else
        print_provisioning_help();
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
#endif
    }

    if (!wifi_ready) {
        ESP_LOGE(TAG, "WiFi failed, restarting...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    // 6. Start task to clear boot counter after stable period
    if (xTaskCreate(clear_boot_count, "boot_ok", BOOT_OK_TASK_STACK_SIZE, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create boot confirmation task");
    }

    // 7. Initialize cron (includes NTP sync)
    ESP_ERROR_CHECK(cron_init());

    // 8. Initialize LLM client
    ESP_ERROR_CHECK(llm_init());

    // 9. Initialize rate limiter
    ratelimit_init();

    // 10. Initialize Telegram
#if CONFIG_ZCLAW_STUB_TELEGRAM
    ESP_LOGW(TAG, "Telegram stub mode enabled; skipping Telegram startup");
#else
    esp_err_t telegram_init_err = telegram_init();  // Missing token is non-fatal
    if (telegram_init_err != ESP_OK && telegram_init_err != ESP_ERR_NOT_FOUND) {
        fail_fast_startup("telegram_init", telegram_init_err);
    }
#endif

    // 11. Register tools
    tools_init();

    // 12. Initialize USB serial channel
    channel_init();

    // 13. Create queues
    QueueHandle_t input_queue = xQueueCreate(INPUT_QUEUE_LENGTH, sizeof(channel_msg_t));
    QueueHandle_t channel_output_queue = xQueueCreate(OUTPUT_QUEUE_LENGTH, sizeof(channel_output_msg_t));
    QueueHandle_t telegram_output_queue = NULL;
#if CONFIG_ZCLAW_STUB_TELEGRAM
    bool telegram_enabled = false;
#else
    bool telegram_enabled = telegram_is_configured();
#endif
    if (telegram_enabled) {
        telegram_output_queue = xQueueCreate(TELEGRAM_OUTPUT_QUEUE_LENGTH, sizeof(telegram_msg_t));
    }

    if (!input_queue || !channel_output_queue || (telegram_enabled && !telegram_output_queue)) {
        ESP_LOGE(TAG, "Failed to create queues");
        esp_restart();
    }

    // 14. Start channel task (USB serial)
    esp_err_t startup_err = channel_start(input_queue, channel_output_queue);
    if (startup_err != ESP_OK) {
        fail_fast_startup("channel_start", startup_err);
    }

    // 15. Start Telegram channel
    if (telegram_enabled) {
        startup_err = telegram_start(input_queue, telegram_output_queue);
        if (startup_err != ESP_OK) {
            fail_fast_startup("telegram_start", startup_err);
        }
    }

    // 16. Start agent task
    startup_err = agent_start(input_queue, channel_output_queue, telegram_output_queue);
    if (startup_err != ESP_OK) {
        fail_fast_startup("agent_start", startup_err);
    }

    // 17. Start cron task
    startup_err = cron_start(input_queue);
    if (startup_err != ESP_OK) {
        fail_fast_startup("cron_start", startup_err);
    }

    // 18. Print ready message
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Ready! Free heap: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");

    // 19. Send startup notification on Telegram
    if (telegram_enabled && telegram_is_configured()) {
        telegram_send_startup();
    }

    // app_main returns - FreeRTOS scheduler continues running tasks
#endif
}
