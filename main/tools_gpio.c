#include "tools_handlers.h"
#include "tools_common.h"
#include "config.h"
#include "gpio_policy.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Max delay to prevent accidental lockups (60 seconds)
#define DELAY_MAX_MS 60000

static const char *TAG = "tools";

static bool gpio_append_read_state(char **cursor, size_t *remaining, int pin, bool first_pin)
{
    int level;
    int written;

    // Enable input path without clobbering current output drive/mode.
    if (gpio_input_enable(pin) != 0) {
        return false;
    }
    level = gpio_get_level(pin);

    written = snprintf(*cursor, *remaining, "%s%d=%s",
                       first_pin ? "" : ", ",
                       pin,
                       level ? "HIGH" : "LOW");
    if (written < 0 || (size_t)written >= *remaining) {
        return false;
    }

    *cursor += (size_t)written;
    *remaining -= (size_t)written;
    return true;
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

    if (!tools_validate_allowed_gpio_pin(pin, NULL, result, result_len)) {
        return false;
    }

    if (gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT) != 0 ||
        gpio_set_level(pin, state ? 1 : 0) != 0) {
        snprintf(result, result_len, "Error: failed to configure/write pin %d", pin);
        return false;
    }

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

    if (!tools_validate_allowed_gpio_pin(pin, NULL, result, result_len)) {
        return false;
    }

    if (gpio_input_enable(pin) != 0) {
        snprintf(result, result_len, "Error: failed to configure/read pin %d", pin);
        return false;
    }
    int level = gpio_get_level(pin);

    snprintf(result, result_len, "Pin %d = %s", pin, level ? "HIGH" : "LOW");
    return true;
}

bool tools_gpio_read_all_handler(const cJSON *input, char *result, size_t result_len)
{
    char *cursor = result;
    size_t remaining = result_len;
    int written;
    int count = 0;

    (void)input;

    if (!result || result_len == 0) {
        return false;
    }

    written = snprintf(cursor, remaining, "GPIO states: ");
    if (written < 0 || (size_t)written >= remaining) {
        result[0] = '\0';
        return false;
    }
    cursor += (size_t)written;
    remaining -= (size_t)written;

    if (GPIO_ALLOWED_PINS_CSV[0] != '\0') {
        const char *csv_cursor = GPIO_ALLOWED_PINS_CSV;

        while (*csv_cursor != '\0') {
            char *endptr = NULL;
            long value;

            while (*csv_cursor == ' ' || *csv_cursor == '\t' || *csv_cursor == ',') {
                csv_cursor++;
            }
            if (*csv_cursor == '\0') {
                break;
            }

            value = strtol(csv_cursor, &endptr, 10);
            if (endptr == csv_cursor) {
                while (*csv_cursor != '\0' && *csv_cursor != ',') {
                    csv_cursor++;
                }
                continue;
            }
            if (value < INT_MIN || value > INT_MAX) {
                csv_cursor = endptr;
                continue;
            }
            if (!gpio_policy_pin_is_allowed((int)value)) {
                csv_cursor = endptr;
                continue;
            }

            if (!gpio_append_read_state(&cursor, &remaining, (int)value, count == 0)) {
                snprintf(result, result_len, "Error: failed to read allowed GPIO pin state");
                return false;
            }
            count++;
            csv_cursor = endptr;
        }
    } else {
        int pin;
        for (pin = GPIO_MIN_PIN; pin <= GPIO_MAX_PIN; pin++) {
            if (!gpio_policy_pin_is_allowed(pin)) {
                continue;
            }
            if (!gpio_append_read_state(&cursor, &remaining, pin, count == 0)) {
                snprintf(result, result_len, "Error: failed to read allowed GPIO pin state");
                return false;
            }
            count++;
        }
    }

    if (count == 0) {
        snprintf(result, result_len, "Error: no allowed GPIO pins configured");
        return false;
    }

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
    return gpio_policy_test_pin_is_allowed(pin, csv, min_pin, max_pin, false, true);
}

bool tools_gpio_test_pin_is_allowed_for_esp32_target(int pin, const char *csv, int min_pin, int max_pin)
{
    return gpio_policy_test_pin_is_allowed(pin, csv, min_pin, max_pin, true, true);
}
#endif
