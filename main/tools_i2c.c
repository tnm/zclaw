#include "tools_handlers.h"
#include "tools_common.h"
#include "gpio_policy.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define I2C_TOOL_PORT                I2C_NUM_0
#define I2C_ADDR_FIRST               0x03
#define I2C_ADDR_LAST                0x77
#define I2C_DEFAULT_FREQ_HZ          100000
#define I2C_MIN_FREQ_HZ              10000
#define I2C_MAX_FREQ_HZ              1000000
#define I2C_ADDR_TIMEOUT_MS          25
#define I2C_IO_TIMEOUT_MS            1000
#define I2C_MAX_DATA_BYTES           64

typedef enum {
    I2C_TOOL_WRITE = 0,
    I2C_TOOL_READ,
    I2C_TOOL_WRITE_READ,
} i2c_tool_mode_t;

static bool parse_frequency(const cJSON *input, int *frequency_hz, char *result, size_t result_len)
{
    cJSON *freq_json = cJSON_GetObjectItem(input, "frequency_hz");

    *frequency_hz = I2C_DEFAULT_FREQ_HZ;
    if (!freq_json) {
        return true;
    }
    if (!cJSON_IsNumber(freq_json)) {
        snprintf(result, result_len, "Error: frequency_hz must be a number");
        return false;
    }
    *frequency_hz = freq_json->valueint;
    if (*frequency_hz < I2C_MIN_FREQ_HZ || *frequency_hz > I2C_MAX_FREQ_HZ) {
        snprintf(result, result_len, "Error: frequency_hz must be %d-%d", I2C_MIN_FREQ_HZ, I2C_MAX_FREQ_HZ);
        return false;
    }
    return true;
}

static bool parse_address(const cJSON *input, uint8_t *address, char *result, size_t result_len)
{
    cJSON *address_json = cJSON_GetObjectItem(input, "address");

    if (!address_json || !cJSON_IsNumber(address_json)) {
        snprintf(result, result_len, "Error: address required");
        return false;
    }
    if (address_json->valueint < I2C_ADDR_FIRST || address_json->valueint > I2C_ADDR_LAST) {
        snprintf(result, result_len, "Error: address must be %d-%d (7-bit I2C)", I2C_ADDR_FIRST, I2C_ADDR_LAST);
        return false;
    }
    *address = (uint8_t)address_json->valueint;
    return true;
}

static bool parse_read_length(const cJSON *input, size_t *read_length, char *result, size_t result_len)
{
    cJSON *read_length_json = cJSON_GetObjectItem(input, "read_length");

    if (!read_length_json || !cJSON_IsNumber(read_length_json)) {
        snprintf(result, result_len, "Error: read_length required");
        return false;
    }
    if (read_length_json->valueint <= 0 || read_length_json->valueint > I2C_MAX_DATA_BYTES) {
        snprintf(result, result_len, "Error: read_length must be 1-%d", I2C_MAX_DATA_BYTES);
        return false;
    }
    *read_length = (size_t)read_length_json->valueint;
    return true;
}

static bool parse_hex_payload_field(const cJSON *input,
                                    const char *field_name,
                                    uint8_t *buffer,
                                    size_t max_len,
                                    size_t *out_len,
                                    char *result,
                                    size_t result_len)
{
    cJSON *field_json = cJSON_GetObjectItem(input, field_name);
    const char *cursor;
    size_t count = 0;

    if (!field_json || !cJSON_IsString(field_json) || !field_json->valuestring) {
        snprintf(result, result_len, "Error: %s required", field_name);
        return false;
    }

    cursor = field_json->valuestring;
    while (*cursor != '\0') {
        char *endptr = NULL;
        long value;

        while (*cursor != '\0' &&
               (isspace((unsigned char)*cursor) || *cursor == ',' || *cursor == ';' || *cursor == ':')) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        value = strtol(cursor, &endptr, 16);
        if (endptr == cursor ||
            (*endptr != '\0' &&
             !isspace((unsigned char)*endptr) &&
             *endptr != ',' &&
             *endptr != ';' &&
             *endptr != ':') ||
            value < 0 ||
            value > 255) {
            snprintf(result, result_len, "Error: %s must be hex bytes", field_name);
            return false;
        }
        if (count >= max_len) {
            snprintf(result, result_len, "Error: %s exceeds %d bytes", field_name, I2C_MAX_DATA_BYTES);
            return false;
        }

        buffer[count++] = (uint8_t)value;
        cursor = endptr;
    }

    if (count == 0) {
        snprintf(result, result_len, "Error: %s must contain at least one byte", field_name);
        return false;
    }

    *out_len = count;
    return true;
}

static bool format_hex_bytes(const uint8_t *bytes, size_t byte_count, char *result, size_t result_len)
{
    size_t offset = 0;

    for (size_t i = 0; i < byte_count; i++) {
        int written;

        if (offset >= result_len) {
            return false;
        }
        written = snprintf(result + offset,
                           result_len - offset,
                           i == 0 ? "0x%02X" : " 0x%02X",
                           bytes[i]);
        if (written < 0 || (size_t)written >= result_len - offset) {
            return false;
        }
        offset += (size_t)written;
    }
    return true;
}

static bool parse_bus_pins(const cJSON *input, int *sda_pin, int *scl_pin, char *result, size_t result_len)
{
    cJSON *sda_pin_json = cJSON_GetObjectItem(input, "sda_pin");
    cJSON *scl_pin_json = cJSON_GetObjectItem(input, "scl_pin");

    if (!sda_pin_json || !cJSON_IsNumber(sda_pin_json)) {
        snprintf(result, result_len, "Error: sda_pin required");
        return false;
    }
    if (!scl_pin_json || !cJSON_IsNumber(scl_pin_json)) {
        snprintf(result, result_len, "Error: scl_pin required");
        return false;
    }

    *sda_pin = sda_pin_json->valueint;
    *scl_pin = scl_pin_json->valueint;
    if (*sda_pin == *scl_pin) {
        snprintf(result, result_len, "Error: SDA and SCL must differ");
        return false;
    }
    if (!tools_validate_allowed_gpio_pin(*sda_pin, "SDA", result, result_len) ||
        !tools_validate_allowed_gpio_pin(*scl_pin, "SCL", result, result_len)) {
        return false;
    }

    return true;
}

static bool init_i2c_master(int sda_pin, int scl_pin, int frequency_hz, char *result, size_t result_len)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = frequency_hz,
    };
    esp_err_t err;

    // Clear any previous configuration on this port so calls are repeatable.
    i2c_driver_delete(I2C_TOOL_PORT);

    err = i2c_param_config(I2C_TOOL_PORT, &conf);
    if (err != ESP_OK) {
        snprintf(result, result_len, "Error: i2c_param_config failed (%s)", esp_err_to_name(err));
        return false;
    }

    err = i2c_driver_install(I2C_TOOL_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        snprintf(result, result_len, "Error: i2c_driver_install failed (%s)", esp_err_to_name(err));
        return false;
    }

    return true;
}

static const char *i2c_mode_name(i2c_tool_mode_t mode)
{
    switch (mode) {
        case I2C_TOOL_WRITE:
            return "i2c_write";
        case I2C_TOOL_READ:
            return "i2c_read";
        default:
            return "i2c_write_read";
    }
}

static bool execute_i2c_transfer(const cJSON *input,
                                 i2c_tool_mode_t mode,
                                 char *result,
                                 size_t result_len)
{
    uint8_t address = 0;
    uint8_t write_buffer[I2C_MAX_DATA_BYTES];
    uint8_t read_buffer[I2C_MAX_DATA_BYTES];
    char hex_buffer[(5 * I2C_MAX_DATA_BYTES) + 1];
    size_t write_len = 0;
    size_t read_len = 0;
    int sda_pin;
    int scl_pin;
    int frequency_hz = I2C_DEFAULT_FREQ_HZ;
    esp_err_t err;

    if (!parse_bus_pins(input, &sda_pin, &scl_pin, result, result_len) ||
        !parse_address(input, &address, result, result_len) ||
        !parse_frequency(input, &frequency_hz, result, result_len)) {
        return false;
    }
    if (mode != I2C_TOOL_READ &&
        !parse_hex_payload_field(input,
                                 mode == I2C_TOOL_WRITE ? "data_hex" : "write_hex",
                                 write_buffer,
                                 sizeof(write_buffer),
                                 &write_len,
                                 result,
                                 result_len)) {
        return false;
    }
    if (mode != I2C_TOOL_WRITE &&
        !parse_read_length(input, &read_len, result, result_len)) {
        return false;
    }
    if (!init_i2c_master(sda_pin, scl_pin, frequency_hz, result, result_len)) {
        return false;
    }

    memset(read_buffer, 0, sizeof(read_buffer));
    switch (mode) {
        case I2C_TOOL_WRITE:
            err = i2c_master_write_to_device(
                I2C_TOOL_PORT,
                address,
                write_buffer,
                write_len,
                pdMS_TO_TICKS(I2C_IO_TIMEOUT_MS)
            );
            break;
        case I2C_TOOL_READ:
            err = i2c_master_read_from_device(
                I2C_TOOL_PORT,
                address,
                read_buffer,
                read_len,
                pdMS_TO_TICKS(I2C_IO_TIMEOUT_MS)
            );
            break;
        default:
            err = i2c_master_write_read_device(
                I2C_TOOL_PORT,
                address,
                write_buffer,
                write_len,
                read_buffer,
                read_len,
                pdMS_TO_TICKS(I2C_IO_TIMEOUT_MS)
            );
            break;
    }

    i2c_driver_delete(I2C_TOOL_PORT);
    if (err != ESP_OK) {
        snprintf(result, result_len, "Error: %s failed (%s)", i2c_mode_name(mode), esp_err_to_name(err));
        return false;
    }

    if (mode == I2C_TOOL_WRITE) {
        snprintf(result, result_len, "I2C 0x%02X wrote %d byte(s)", address, (int)write_len);
        return true;
    }
    if (!format_hex_bytes(read_buffer, read_len, hex_buffer, sizeof(hex_buffer))) {
        snprintf(result, result_len, "Error: failed to format I2C read");
        return false;
    }
    if (mode == I2C_TOOL_READ) {
        snprintf(result, result_len, "I2C 0x%02X read %d byte(s): %s", address, (int)read_len, hex_buffer);
        return true;
    }

    snprintf(result,
             result_len,
             "I2C 0x%02X read %d byte(s) after writing %d byte(s): %s",
             address,
             (int)read_len,
             (int)write_len,
             hex_buffer);
    return true;
}

bool tools_i2c_scan_handler(const cJSON *input, char *result, size_t result_len)
{
    int sda_pin;
    int scl_pin;
    int frequency_hz = I2C_DEFAULT_FREQ_HZ;

    if (!parse_bus_pins(input, &sda_pin, &scl_pin, result, result_len) ||
        !parse_frequency(input, &frequency_hz, result, result_len)) {
        return false;
    }

    if (!init_i2c_master(sda_pin, scl_pin, frequency_hz, result, result_len)) {
        return false;
    }

    uint8_t found_addresses[I2C_ADDR_LAST - I2C_ADDR_FIRST + 1];
    int found_count = 0;
    esp_err_t err;

    for (int addr = I2C_ADDR_FIRST; addr <= I2C_ADDR_LAST; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        if (!cmd) {
            i2c_driver_delete(I2C_TOOL_PORT);
            snprintf(result, result_len, "Error: out of memory during I2C scan");
            return false;
        }

        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);

        err = i2c_master_cmd_begin(
            I2C_TOOL_PORT,
            cmd,
            pdMS_TO_TICKS(I2C_ADDR_TIMEOUT_MS)
        );
        i2c_cmd_link_delete(cmd);

        if (err == ESP_OK && found_count < (int)(sizeof(found_addresses))) {
            found_addresses[found_count++] = (uint8_t)addr;
        }
    }

    i2c_driver_delete(I2C_TOOL_PORT);

    if (found_count == 0) {
        snprintf(
            result,
            result_len,
            "No I2C devices on SDA=%d SCL=%d @ %d Hz",
            sda_pin,
            scl_pin,
            frequency_hz
        );
        return true;
    }

    size_t offset = 0;
    int written = snprintf(
        result,
        result_len,
        "I2C SDA=%d SCL=%d @ %d Hz: ",
        sda_pin,
        scl_pin,
        frequency_hz
    );
    if (written < 0) {
        snprintf(result, result_len, "I2C scan found %d device(s)", found_count);
        return true;
    }
    offset = (size_t)written < result_len ? (size_t)written : result_len - 1;

    int listed = 0;
    for (int i = 0; i < found_count; i++) {
        if (offset >= result_len) {
            break;
        }

        written = snprintf(
            result + offset,
            result_len - offset,
            listed == 0 ? "0x%02X" : ", 0x%02X",
            found_addresses[i]
        );
        if (written < 0 || (size_t)written >= result_len - offset) {
            break;
        }
        offset += (size_t)written;
        listed++;
    }

    if (listed < found_count && offset < result_len) {
        snprintf(result + offset, result_len - offset, " ... (+%d more)", found_count - listed);
    }

    return true;
}

bool tools_i2c_write_handler(const cJSON *input, char *result, size_t result_len)
{
    return execute_i2c_transfer(input, I2C_TOOL_WRITE, result, result_len);
}

bool tools_i2c_read_handler(const cJSON *input, char *result, size_t result_len)
{
    return execute_i2c_transfer(input, I2C_TOOL_READ, result, result_len);
}

bool tools_i2c_write_read_handler(const cJSON *input, char *result, size_t result_len)
{
    return execute_i2c_transfer(input, I2C_TOOL_WRITE_READ, result, result_len);
}

#ifdef TEST_BUILD
bool tools_i2c_test_pin_is_allowed_for_esp32_target(int pin, const char *csv, int min_pin, int max_pin)
{
    return gpio_policy_test_pin_is_allowed(pin, csv, min_pin, max_pin, true, true);
}
#endif
