/*
 * Host tests for i2c_scan pin policy and initialization behavior.
 */

#include <stdio.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "driver/i2c.h"
#include "tools_handlers.h"

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while (0)
#define ASSERT_STR_CONTAINS(haystack, needle) do { \
    if (strstr((haystack), (needle)) == NULL) { \
        printf("  FAIL: expected substring '%s' in '%s' (line %d)\n", (needle), (haystack), __LINE__); \
        return 1; \
    } \
} while (0)

bool tools_i2c_test_pin_is_allowed_for_esp32_target(int pin, const char *csv, int min_pin, int max_pin);

TEST(esp32_target_blocks_flash_pins_for_i2c_policy)
{
    ASSERT(tools_i2c_test_pin_is_allowed_for_esp32_target(5, "", 2, 12));
    ASSERT(!tools_i2c_test_pin_is_allowed_for_esp32_target(6, "", 2, 12));
    ASSERT(!tools_i2c_test_pin_is_allowed_for_esp32_target(11, "", 2, 12));
    ASSERT(tools_i2c_test_pin_is_allowed_for_esp32_target(12, "", 2, 12));
    return 0;
}

TEST(rejects_disallowed_pin_before_i2c_init)
{
    cJSON *input = cJSON_Parse("{\"sda_pin\":1,\"scl_pin\":2}");
    char result[256] = {0};

    ASSERT(input != NULL);
    i2c_test_reset();

    ASSERT(!tools_i2c_scan_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "SDA pin");
    ASSERT(i2c_test_get_param_config_calls() == 0);
    ASSERT(i2c_test_get_driver_install_calls() == 0);

    cJSON_Delete(input);
    return 0;
}

TEST(valid_scan_path_initializes_i2c_and_returns_no_devices)
{
    cJSON *input = cJSON_Parse("{\"sda_pin\":4,\"scl_pin\":5,\"frequency_hz\":100000}");
    char result[256] = {0};

    ASSERT(input != NULL);
    i2c_test_reset();
    i2c_test_set_cmd_begin_result(ESP_FAIL);

    ASSERT(tools_i2c_scan_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "No I2C devices on");
    ASSERT(i2c_test_get_param_config_calls() == 1);
    ASSERT(i2c_test_get_driver_install_calls() == 1);
    ASSERT(i2c_test_get_driver_delete_calls() >= 2);

    cJSON_Delete(input);
    return 0;
}

TEST(write_parses_hex_payload_and_records_device_write)
{
    cJSON *input = cJSON_Parse("{\"sda_pin\":4,\"scl_pin\":5,\"address\":118,\"data_hex\":\"0xF4 0x2E\"}");
    char result[256] = {0};

    ASSERT(input != NULL);
    i2c_test_reset();
    i2c_test_set_write_to_device_result(ESP_OK);

    ASSERT(tools_i2c_write_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "wrote 2 byte(s)");
    ASSERT(i2c_test_get_last_address() == 118);
    ASSERT(i2c_test_get_last_write_length() == 2);
    ASSERT(i2c_test_get_last_write_byte(0) == 0xF4);
    ASSERT(i2c_test_get_last_write_byte(1) == 0x2E);

    cJSON_Delete(input);
    return 0;
}

TEST(read_returns_hex_payload_from_mock)
{
    cJSON *input = cJSON_Parse("{\"sda_pin\":4,\"scl_pin\":5,\"address\":64,\"read_length\":3}");
    char result[256] = {0};
    const uint8_t read_data[3] = {0xDE, 0xAD, 0xBE};

    ASSERT(input != NULL);
    i2c_test_reset();
    i2c_test_set_read_data(read_data, sizeof(read_data));
    i2c_test_set_read_from_device_result(ESP_OK);

    ASSERT(tools_i2c_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "read 3 byte(s)");
    ASSERT_STR_CONTAINS(result, "0xDE 0xAD 0xBE");
    ASSERT(i2c_test_get_last_address() == 64);
    ASSERT(i2c_test_get_last_read_length() == 3);

    cJSON_Delete(input);
    return 0;
}

TEST(write_read_combines_register_write_and_readback)
{
    cJSON *input = cJSON_Parse("{\"sda_pin\":4,\"scl_pin\":5,\"address\":118,\"write_hex\":\"0xD0\",\"read_length\":1}");
    char result[256] = {0};
    const uint8_t read_data[1] = {0x60};

    ASSERT(input != NULL);
    i2c_test_reset();
    i2c_test_set_read_data(read_data, sizeof(read_data));
    i2c_test_set_write_read_device_result(ESP_OK);

    ASSERT(tools_i2c_write_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "after writing 1 byte(s)");
    ASSERT_STR_CONTAINS(result, "0x60");
    ASSERT(i2c_test_get_last_address() == 118);
    ASSERT(i2c_test_get_last_write_length() == 1);
    ASSERT(i2c_test_get_last_write_byte(0) == 0xD0);
    ASSERT(i2c_test_get_last_read_length() == 1);

    cJSON_Delete(input);
    return 0;
}

TEST(rejects_malformed_hex_payload_before_i2c_init)
{
    cJSON *input = cJSON_Parse("{\"sda_pin\":4,\"scl_pin\":5,\"address\":118,\"data_hex\":\"not_hex\"}");
    char result[256] = {0};

    ASSERT(input != NULL);
    i2c_test_reset();

    ASSERT(!tools_i2c_write_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "hex bytes");
    ASSERT(i2c_test_get_param_config_calls() == 0);
    ASSERT(i2c_test_get_driver_install_calls() == 0);

    cJSON_Delete(input);
    return 0;
}

TEST(rejects_out_of_range_address_before_i2c_init)
{
    cJSON *input = cJSON_Parse("{\"sda_pin\":4,\"scl_pin\":5,\"address\":120,\"read_length\":1}");
    char result[256] = {0};

    ASSERT(input != NULL);
    i2c_test_reset();

    ASSERT(!tools_i2c_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "address must be");
    ASSERT(i2c_test_get_param_config_calls() == 0);
    ASSERT(i2c_test_get_driver_install_calls() == 0);

    cJSON_Delete(input);
    return 0;
}

TEST(rejects_out_of_range_read_length_before_i2c_init)
{
    cJSON *input = cJSON_Parse("{\"sda_pin\":4,\"scl_pin\":5,\"address\":64,\"read_length\":65}");
    char result[256] = {0};

    ASSERT(input != NULL);
    i2c_test_reset();

    ASSERT(!tools_i2c_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "read_length must be 1-64");
    ASSERT(i2c_test_get_param_config_calls() == 0);
    ASSERT(i2c_test_get_driver_install_calls() == 0);

    cJSON_Delete(input);
    return 0;
}

int test_tools_i2c_policy_all(void)
{
    int failures = 0;

    printf("\nI2C Policy Tests:\n");

    printf("  esp32_target_blocks_flash_pins_for_i2c_policy... ");
    if (test_esp32_target_blocks_flash_pins_for_i2c_policy() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  rejects_disallowed_pin_before_i2c_init... ");
    if (test_rejects_disallowed_pin_before_i2c_init() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  valid_scan_path_initializes_i2c_and_returns_no_devices... ");
    if (test_valid_scan_path_initializes_i2c_and_returns_no_devices() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  write_parses_hex_payload_and_records_device_write... ");
    if (test_write_parses_hex_payload_and_records_device_write() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  read_returns_hex_payload_from_mock... ");
    if (test_read_returns_hex_payload_from_mock() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  write_read_combines_register_write_and_readback... ");
    if (test_write_read_combines_register_write_and_readback() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  rejects_malformed_hex_payload_before_i2c_init... ");
    if (test_rejects_malformed_hex_payload_before_i2c_init() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  rejects_out_of_range_address_before_i2c_init... ");
    if (test_rejects_out_of_range_address_before_i2c_init() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  rejects_out_of_range_read_length_before_i2c_init... ");
    if (test_rejects_out_of_range_read_length_before_i2c_init() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
