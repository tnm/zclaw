#include "tools_handlers.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Max delay to prevent accidental lockups (60 seconds)
#define DELAY_MAX_MS 60000

static const char *TAG = "tools";

static bool gpio_pin_in_allowlist(int pin, const char *csv)
{
    const char *cursor;

    if (!csv || csv[0] == '\0') {
        return false;
    }

    cursor = csv;
    while (*cursor != '\0') {
        char *endptr = NULL;
        long value;

        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        value = strtol(cursor, &endptr, 10);
        if (endptr == cursor) {
            while (*cursor != '\0' && *cursor != ',') {
                cursor++;
            }
            continue;
        }

        if ((int)value == pin) {
            return true;
        }
        cursor = endptr;
    }

    return false;
}

static bool gpio_pin_is_allowed(int pin)
{
    if (GPIO_ALLOWED_PINS_CSV[0] != '\0') {
        return gpio_pin_in_allowlist(pin, GPIO_ALLOWED_PINS_CSV);
    }
    return pin >= GPIO_MIN_PIN && pin <= GPIO_MAX_PIN;
}

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

    if (!gpio_pin_is_allowed(pin)) {
        if (GPIO_ALLOWED_PINS_CSV[0] != '\0') {
            snprintf(result, result_len, "Error: pin %d is not in allowed list", pin);
        } else {
            snprintf(result, result_len, "Error: pin must be %d-%d", GPIO_MIN_PIN, GPIO_MAX_PIN);
        }
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

    if (!gpio_pin_is_allowed(pin)) {
        if (GPIO_ALLOWED_PINS_CSV[0] != '\0') {
            snprintf(result, result_len, "Error: pin %d is not in allowed list", pin);
        } else {
            snprintf(result, result_len, "Error: pin must be %d-%d", GPIO_MIN_PIN, GPIO_MAX_PIN);
        }
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

#ifdef TEST_BUILD
bool tools_gpio_test_pin_is_allowed(int pin, const char *csv, int min_pin, int max_pin)
{
    if (csv && csv[0] != '\0') {
        return gpio_pin_in_allowlist(pin, csv);
    }
    return pin >= min_pin && pin <= max_pin;
}
#endif
