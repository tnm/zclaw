#include "tools_handlers.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

// Max delay to prevent accidental lockups (60 seconds)
#define DELAY_MAX_MS 60000

static const char *TAG = "tools";

bool tools_gpio_write_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *pin_json = cJSON_GetObjectItem(input, "pin");
    cJSON *state_json = cJSON_GetObjectItem(input, "state");

    if (!pin_json || !cJSON_IsNumber(pin_json)) {
        snprintf(result, result_len, "Error: 'pin' required (number)");
        return false;
    }
    if (!state_json || !cJSON_IsNumber(state_json)) {
        snprintf(result, result_len, "Error: 'state' required (0 or 1)");
        return false;
    }

    int pin = pin_json->valueint;
    int state = state_json->valueint;

    if (pin < GPIO_MIN_PIN || pin > GPIO_MAX_PIN) {
        snprintf(result, result_len, "Error: pin must be %d-%d", GPIO_MIN_PIN, GPIO_MAX_PIN);
        return false;
    }

    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, state ? 1 : 0);

    snprintf(result, result_len, "Pin %d → %s", pin, state ? "HIGH" : "LOW");
    return true;
}

bool tools_gpio_read_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *pin_json = cJSON_GetObjectItem(input, "pin");

    if (!pin_json || !cJSON_IsNumber(pin_json)) {
        snprintf(result, result_len, "Error: 'pin' required (number)");
        return false;
    }

    int pin = pin_json->valueint;

    if (pin < GPIO_MIN_PIN || pin > GPIO_MAX_PIN) {
        snprintf(result, result_len, "Error: pin must be %d-%d", GPIO_MIN_PIN, GPIO_MAX_PIN);
        return false;
    }

    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    int level = gpio_get_level(pin);

    snprintf(result, result_len, "Pin %d = %s", pin, level ? "HIGH" : "LOW");
    return true;
}

bool tools_delay_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *ms_json = cJSON_GetObjectItem(input, "milliseconds");

    if (!ms_json || !cJSON_IsNumber(ms_json)) {
        snprintf(result, result_len, "Error: 'milliseconds' required (number)");
        return false;
    }

    int ms = ms_json->valueint;

    if (ms <= 0) {
        snprintf(result, result_len, "Error: milliseconds must be positive");
        return false;
    }

    if (ms > DELAY_MAX_MS) {
        snprintf(result, result_len, "Error: max delay is %d ms (got %d)", DELAY_MAX_MS, ms);
        return false;
    }

    ESP_LOGI(TAG, "Delaying %d ms...", ms);
    vTaskDelay(pdMS_TO_TICKS(ms));

    snprintf(result, result_len, "Waited %d ms", ms);
    return true;
}
