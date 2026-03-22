#include "tools_handlers.h"
#include "tools_common.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DHT_DATA_BYTES           5
#define DHT_MAX_RETRIES          3
#define DHT_TRACKED_PINS         50
#define DHT_RESPONSE_TIMEOUT_US  120
#define DHT_BIT_TIMEOUT_US       120
#define DHT_SAMPLE_DELAY_US      40

typedef enum {
    DHT_MODEL_DHT11,
    DHT_MODEL_DHT22,
} dht_model_t;

typedef struct {
    const char *name;
    int start_low_ms;
    int min_interval_ms;
} dht_profile_t;

static const dht_profile_t DHT11_PROFILE = {
    .name = "DHT11",
    .start_low_ms = 20,
    .min_interval_ms = 1000,
};

static const dht_profile_t DHT22_PROFILE = {
    .name = "DHT22",
    .start_low_ms = 2,
    .min_interval_ms = 2000,
};

static int64_t s_last_read_us[DHT_TRACKED_PINS];

#ifdef TEST_BUILD
static bool s_mock_active = false;
static bool s_mock_success = false;
static uint8_t s_mock_bytes[DHT_DATA_BYTES];
static char s_mock_error[128];
#endif

static const dht_profile_t *dht_profile_from_name(const char *name)
{
    if (!name) {
        return NULL;
    }
    if (strcmp(name, "dht11") == 0) {
        return &DHT11_PROFILE;
    }
    if (strcmp(name, "dht22") == 0) {
        return &DHT22_PROFILE;
    }
    return NULL;
}

static void dht_delay_us(int delay_us)
{
    int64_t started_us;

    if (delay_us <= 0) {
        return;
    }

    started_us = esp_timer_get_time();
    while ((esp_timer_get_time() - started_us) < delay_us) {
    }
}

static bool dht_wait_for_level(int pin, int level, int timeout_us)
{
    int64_t started_us = esp_timer_get_time();

    while ((esp_timer_get_time() - started_us) < timeout_us) {
        if (gpio_get_level(pin) == level) {
            return true;
        }
    }

    return false;
}

static void dht_enforce_min_interval(int pin, const dht_profile_t *profile)
{
    int64_t last_read_us;
    int64_t elapsed_us;
    int64_t remaining_us;
    int remaining_ms;

    if (pin < 0 || pin >= DHT_TRACKED_PINS || !profile) {
        return;
    }

    last_read_us = s_last_read_us[pin];
    if (last_read_us <= 0) {
        return;
    }

    elapsed_us = esp_timer_get_time() - last_read_us;
    remaining_us = ((int64_t)profile->min_interval_ms * 1000LL) - elapsed_us;
    if (remaining_us <= 0) {
        return;
    }

    remaining_ms = (int)((remaining_us + 999LL) / 1000LL);
    if (remaining_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(remaining_ms));
    }
}

static void dht_record_success(int pin)
{
    if (pin >= 0 && pin < DHT_TRACKED_PINS) {
        s_last_read_us[pin] = esp_timer_get_time();
    }
}

static bool dht_capture_bytes_from_sensor(int pin,
                                          const dht_profile_t *profile,
                                          uint8_t data[DHT_DATA_BYTES],
                                          char *result,
                                          size_t result_len)
{
    memset(data, 0, DHT_DATA_BYTES);

    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    vTaskDelay(pdMS_TO_TICKS(profile->start_low_ms));
    gpio_set_level(pin, 1);
    dht_delay_us(40);
    gpio_set_direction(pin, GPIO_MODE_INPUT);

    if (!dht_wait_for_level(pin, 0, DHT_RESPONSE_TIMEOUT_US) ||
        !dht_wait_for_level(pin, 1, DHT_RESPONSE_TIMEOUT_US) ||
        !dht_wait_for_level(pin, 0, DHT_RESPONSE_TIMEOUT_US)) {
        snprintf(result, result_len, "Error: no DHT response on pin %d", pin);
        return false;
    }

    for (int bit = 0; bit < 40; bit++) {
        if (!dht_wait_for_level(pin, 1, DHT_BIT_TIMEOUT_US)) {
            snprintf(result, result_len, "Error: DHT timeout on pin %d", pin);
            return false;
        }

        data[bit / 8] <<= 1;
        dht_delay_us(DHT_SAMPLE_DELAY_US);
        if (gpio_get_level(pin)) {
            data[bit / 8] |= 1;
        }

        if (bit < 39 && !dht_wait_for_level(pin, 0, DHT_BIT_TIMEOUT_US)) {
            snprintf(result, result_len, "Error: DHT timeout on pin %d", pin);
            return false;
        }
    }

    return true;
}

static bool dht_read_raw_bytes(int pin,
                               const dht_profile_t *profile,
                               uint8_t data[DHT_DATA_BYTES],
                               char *result,
                               size_t result_len)
{
#ifdef TEST_BUILD
    if (s_mock_active) {
        if (!s_mock_success) {
            snprintf(result,
                     result_len,
                     "%s",
                     s_mock_error[0] != '\0' ? s_mock_error : "Error: mocked DHT read failure");
            return false;
        }
        memcpy(data, s_mock_bytes, DHT_DATA_BYTES);
        return true;
    }
#endif

    return dht_capture_bytes_from_sensor(pin, profile, data, result, result_len);
}

static bool dht_validate_and_format_result(const dht_profile_t *profile,
                                           int pin,
                                           const uint8_t data[DHT_DATA_BYTES],
                                           char *result,
                                           size_t result_len)
{
    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    int humidity_tenths;
    int temperature_tenths;
    int humidity_abs;
    int temperature_abs;

    if (!profile) {
        snprintf(result, result_len, "Error: unsupported DHT model");
        return false;
    }
    if (checksum != data[4]) {
        snprintf(result,
                 result_len,
                 "Error: DHT checksum mismatch (expected 0x%02X got 0x%02X)",
                 checksum,
                 data[4]);
        return false;
    }

    if (profile == &DHT11_PROFILE) {
        humidity_tenths = ((int)data[0] * 10) + (int)data[1];
        temperature_tenths = ((int)data[2] * 10) + (int)data[3];
    } else {
        humidity_tenths = ((int)data[0] << 8) | (int)data[1];
        temperature_tenths = (((int)data[2] & 0x7F) << 8) | (int)data[3];
        if ((data[2] & 0x80) != 0) {
            temperature_tenths = -temperature_tenths;
        }
    }

    humidity_abs = humidity_tenths < 0 ? -humidity_tenths : humidity_tenths;
    temperature_abs = temperature_tenths < 0 ? -temperature_tenths : temperature_tenths;

    snprintf(result,
             result_len,
             "%s GPIO %d: %d.%d%% RH, %s%d.%d C",
             profile->name,
             pin,
             humidity_abs / 10,
             humidity_abs % 10,
             temperature_tenths < 0 ? "-" : "",
             temperature_abs / 10,
             temperature_abs % 10);
    return true;
}

bool tools_dht_read_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *pin_json = cJSON_GetObjectItem(input, "pin");
    cJSON *model_json = cJSON_GetObjectItem(input, "model");
    cJSON *retries_json = cJSON_GetObjectItem(input, "retries");
    const dht_profile_t *profile;
    uint8_t data[DHT_DATA_BYTES];
    int pin;
    int retries = 1;

    if (!pin_json || !cJSON_IsNumber(pin_json)) {
        snprintf(result, result_len, "Error: pin required");
        return false;
    }
    if (!model_json || !cJSON_IsString(model_json) || !model_json->valuestring) {
        snprintf(result, result_len, "Error: model required");
        return false;
    }

    profile = dht_profile_from_name(model_json->valuestring);
    if (!profile) {
        snprintf(result, result_len, "Error: model must be 'dht11' or 'dht22'");
        return false;
    }
    if (retries_json) {
        if (!cJSON_IsNumber(retries_json)) {
            snprintf(result, result_len, "Error: retries must be a number");
            return false;
        }
        retries = retries_json->valueint;
    }
    if (retries <= 0 || retries > DHT_MAX_RETRIES) {
        snprintf(result, result_len, "Error: retries must be 1-%d", DHT_MAX_RETRIES);
        return false;
    }

    pin = pin_json->valueint;
    if (!tools_validate_allowed_gpio_pin(pin, NULL, result, result_len)) {
        return false;
    }

    for (int attempt = 0; attempt < retries; attempt++) {
        dht_enforce_min_interval(pin, profile);

        if (dht_read_raw_bytes(pin, profile, data, result, result_len) &&
            dht_validate_and_format_result(profile, pin, data, result, result_len)) {
            dht_record_success(pin);
            return true;
        }

        if (attempt + 1 < retries) {
            vTaskDelay(pdMS_TO_TICKS(profile->min_interval_ms));
        }
    }

    return false;
}

#ifdef TEST_BUILD
bool tools_dht_test_decode_bytes(const char *model_name,
                                 int pin,
                                 const uint8_t data[5],
                                 char *result,
                                 size_t result_len)
{
    return dht_validate_and_format_result(dht_profile_from_name(model_name), pin, data, result, result_len);
}

void tools_dht_test_reset(void)
{
    memset(s_last_read_us, 0, sizeof(s_last_read_us));
    s_mock_active = false;
    s_mock_success = false;
    memset(s_mock_bytes, 0, sizeof(s_mock_bytes));
    memset(s_mock_error, 0, sizeof(s_mock_error));
}

void tools_dht_test_set_mock_success(const uint8_t data[5])
{
    s_mock_active = true;
    s_mock_success = true;
    memcpy(s_mock_bytes, data, DHT_DATA_BYTES);
    memset(s_mock_error, 0, sizeof(s_mock_error));
}

void tools_dht_test_set_mock_failure(const char *error_message)
{
    s_mock_active = true;
    s_mock_success = false;
    memset(s_mock_bytes, 0, sizeof(s_mock_bytes));
    snprintf(s_mock_error,
             sizeof(s_mock_error),
             "%s",
             error_message && error_message[0] != '\0' ? error_message : "Error: mocked DHT read failure");
}
#endif
