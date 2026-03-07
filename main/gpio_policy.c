#include "gpio_policy.h"

#include "config.h"
#include "driver/gpio.h"

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

#ifndef GPIO_IS_VALID_GPIO
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0)
#endif

static bool pin_in_allowlist(int pin, const char *csv)
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

static bool pin_is_allowed_impl(int pin,
                                const char *allowlist_csv,
                                int min_pin,
                                int max_pin,
                                bool block_esp32_flash_pins,
                                bool require_valid_gpio)
{
    bool in_policy;

    if (pin < 0) {
        return false;
    }

    if (block_esp32_flash_pins && pin >= 6 && pin <= 11) {
        return false;
    }

    if (allowlist_csv && allowlist_csv[0] != '\0') {
        in_policy = pin_in_allowlist(pin, allowlist_csv);
    } else {
        in_policy = pin >= min_pin && pin <= max_pin;
    }

    if (!in_policy) {
        return false;
    }

    if (require_valid_gpio) {
        return GPIO_IS_VALID_GPIO((gpio_num_t)pin);
    }

    return true;
}

bool gpio_policy_pin_is_allowed(int pin)
{
#if defined(CONFIG_IDF_TARGET_ESP32)
    return pin_is_allowed_impl(pin, GPIO_ALLOWED_PINS_CSV, GPIO_MIN_PIN, GPIO_MAX_PIN, true, true);
#else
    return pin_is_allowed_impl(pin, GPIO_ALLOWED_PINS_CSV, GPIO_MIN_PIN, GPIO_MAX_PIN, false, true);
#endif
}

bool gpio_policy_runtime_input_pin_is_safe(int pin)
{
#if defined(CONFIG_IDF_TARGET_ESP32)
    return pin_is_allowed_impl(pin, NULL, 0, INT_MAX, true, true);
#else
    return pin_is_allowed_impl(pin, NULL, 0, INT_MAX, false, true);
#endif
}

bool gpio_policy_pin_forbidden_hint(int pin, char *result, size_t result_len)
{
#if defined(CONFIG_IDF_TARGET_ESP32)
    if (pin >= 6 && pin <= 11) {
        snprintf(result, result_len,
                 "Error: pin %d is reserved for ESP32 flash/PSRAM (GPIO6-11); choose a different pin",
                 pin);
        return true;
    }
#else
    (void)pin;
    (void)result;
    (void)result_len;
#endif

    return false;
}

#ifdef TEST_BUILD
bool gpio_policy_test_pin_is_allowed(int pin,
                                     const char *csv,
                                     int min_pin,
                                     int max_pin,
                                     bool block_esp32_flash_pins,
                                     bool require_valid_gpio)
{
    return pin_is_allowed_impl(pin, csv, min_pin, max_pin, block_esp32_flash_pins, require_valid_gpio);
}

bool gpio_policy_test_runtime_input_pin_is_safe(int pin,
                                                bool block_esp32_flash_pins,
                                                bool require_valid_gpio)
{
    return pin_is_allowed_impl(pin, NULL, 0, INT_MAX, block_esp32_flash_pins, require_valid_gpio);
}
#endif
