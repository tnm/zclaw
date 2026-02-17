#include "config.h"
#include "memory.h"
#include "channel.h"
#include "agent.h"
#include "llm.h"
#include "tools.h"
#include "telegram.h"
#include "cron.h"
#include "websetup.h"
#include "ratelimit.h"
#include "ota.h"
#include "boot_guard.h"
#include "nvs_keys.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "main";

// WiFi event group
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static bool s_safe_mode = false;

// Boot loop protection
static int get_boot_count(void)
{
    char buf[16];
    if (memory_get(NVS_KEY_BOOT_COUNT, buf, sizeof(buf))) {
        return atoi(buf);
    }
    return 0;
}

static void set_boot_count(int count)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", count);
    memory_set(NVS_KEY_BOOT_COUNT, buf);
}

static void clear_boot_count(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(BOOT_SUCCESS_DELAY_MS));

    bool pending_before = ota_is_pending_verify();
    if (pending_before) {
        esp_err_t ota_err = ota_mark_valid_if_pending();
        if (ota_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to confirm OTA image: %s", esp_err_to_name(ota_err));
        } else {
            ESP_LOGI(TAG, "OTA image confirmed after stable boot window");
        }
    }

    set_boot_count(0);
    ESP_LOGI(TAG, "Boot counter cleared - system stable");
    vTaskDelete(NULL);
}

// WiFi event handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "WiFi retry %d/%d", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "WiFi connection failed");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
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

// Connect to WiFi using stored credentials
static bool wifi_connect_sta(void)
{
    char ssid[64] = {0};
    char pass[64] = {0};

    if (!websetup_get_wifi_ssid(ssid, sizeof(ssid))) {
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

    if (!websetup_get_wifi_pass(pass, sizeof(pass))) {
#if defined(CONFIG_ZCLAW_WIFI_PASSWORD)
        strncpy(pass, CONFIG_ZCLAW_WIFI_PASSWORD, sizeof(pass) - 1);
        pass[sizeof(pass) - 1] = '\0';
#endif
    }

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to %s...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, portMAX_DELAY);

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  zclaw v2.0");
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
    int boot_count = get_boot_count();
    int next_boot_count = boot_guard_next_count(boot_count);
    set_boot_count(next_boot_count);

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

    QueueHandle_t input_queue = xQueueCreate(INPUT_QUEUE_LENGTH, CHANNEL_RX_BUF_SIZE);
    QueueHandle_t channel_output_queue = xQueueCreate(OUTPUT_QUEUE_LENGTH, CHANNEL_RX_BUF_SIZE);
    if (!input_queue || !channel_output_queue) {
        ESP_LOGE(TAG, "Failed to create emulator queues");
        esp_restart();
    }

    channel_start(input_queue, channel_output_queue);
    agent_start(input_queue, channel_output_queue, NULL);

    channel_write("\r\nzclaw emulator ready. Type a message and press Enter.\r\n\r\n");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else

    // 4. Check if configured or in safe mode
    if (!websetup_is_configured() || s_safe_mode) {
        if (s_safe_mode) {
            ESP_LOGW(TAG, "Safe mode - starting setup AP for recovery");
        } else {
            ESP_LOGW(TAG, "Not configured - starting setup mode");
        }

        // Initialize WiFi for AP mode
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        websetup_start_ap_mode();

        // Stay in setup mode forever (will restart after config saved)
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // 5. Connect to WiFi
    if (!wifi_connect_sta()) {
        ESP_LOGE(TAG, "WiFi failed, restarting...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    // 6. Start task to clear boot counter after stable period
    if (xTaskCreate(clear_boot_count, "boot_ok", 2048, NULL, 1, NULL) != pdPASS) {
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
    telegram_init();  // May fail if not configured, that's OK
#endif

    // 11. Register tools
    tools_init();

    // 12. Initialize USB serial channel
    channel_init();

    // 13. Create queues
    QueueHandle_t input_queue = xQueueCreate(INPUT_QUEUE_LENGTH, CHANNEL_RX_BUF_SIZE);
    QueueHandle_t channel_output_queue = xQueueCreate(OUTPUT_QUEUE_LENGTH, CHANNEL_RX_BUF_SIZE);
    QueueHandle_t telegram_output_queue = NULL;
#if CONFIG_ZCLAW_STUB_TELEGRAM
    bool telegram_enabled = false;
#else
    bool telegram_enabled = telegram_is_configured();
#endif
    if (telegram_enabled) {
        telegram_output_queue = xQueueCreate(TELEGRAM_OUTPUT_QUEUE_LENGTH, TELEGRAM_MAX_MSG_LEN);
    }

    if (!input_queue || !channel_output_queue || (telegram_enabled && !telegram_output_queue)) {
        ESP_LOGE(TAG, "Failed to create queues");
        esp_restart();
    }

    // 14. Start channel task (USB serial)
    channel_start(input_queue, channel_output_queue);

    // 15. Start Telegram channel
    if (telegram_enabled) {
        telegram_start(input_queue, telegram_output_queue);
    }

    // 16. Start agent task
    agent_start(input_queue, channel_output_queue, telegram_output_queue);

    // 17. Start cron task
    cron_start(input_queue);

    // 18. Start web config server (for reconfiguration)
#if WEBSETUP_ENABLE_STA_SERVER
    websetup_start_sta_mode();
#else
    ESP_LOGI(TAG, "Web setup server disabled in STA mode");
#endif

    // Print ready message
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
