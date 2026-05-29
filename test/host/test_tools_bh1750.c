/*
 * Host tests for typed BH1750 ambient light reads.
 */

#include <stdio.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "driver/i2c.h"
#include "mock_freertos.h"
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

TEST(handler_defaults_to_primary_address_and_formats_lux)
{
    cJSON *input = cJSON_Parse("{\"sda_pin\":4,\"scl_pin\":5}");
    char result[128] = {0};
    const uint8_t read_data[2] = {0x01, 0xC2};

    ASSERT(input != NULL);
    i2c_test_reset();
    mock_freertos_reset();
    i2c_test_set_read_data(read_data, sizeof(read_data));
    i2c_test_set_write_to_device_result(ESP_OK);
    i2c_test_set_read_from_device_result(ESP_OK);

    ASSERT(tools_bh1750_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "BH1750 0x23");
    ASSERT_STR_CONTAINS(result, "375.0 lux");
    ASSERT(i2c_test_get_write_to_device_calls() == 2);
    ASSERT(i2c_test_get_write_to_device_address(0) == 0x23);
    ASSERT(i2c_test_get_write_to_device_length(0) == 1);
    ASSERT(i2c_test_get_write_to_device_byte(0, 0) == 0x01);
    ASSERT(i2c_test_get_write_to_device_address(1) == 0x23);
    ASSERT(i2c_test_get_write_to_device_length(1) == 1);
    ASSERT(i2c_test_get_write_to_device_byte(1, 0) == 0x20);
    ASSERT(i2c_test_get_last_read_length() == 2);
    ASSERT(i2c_test_get_driver_delete_calls() == 2);
    ASSERT(mock_freertos_delay_count() == 1);
    ASSERT(mock_freertos_delay_at(0) == pdMS_TO_TICKS(180));

    cJSON_Delete(input);
    return 0;
}

TEST(handler_accepts_alternate_address)
{
    cJSON *input = cJSON_Parse("{\"sda_pin\":4,\"scl_pin\":5,\"address\":92}");
    char result[128] = {0};
    const uint8_t read_data[2] = {0x00, 0x78};

    ASSERT(input != NULL);
    i2c_test_reset();
    mock_freertos_reset();
    i2c_test_set_read_data(read_data, sizeof(read_data));

    ASSERT(tools_bh1750_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "BH1750 0x5C");
    ASSERT(i2c_test_get_write_to_device_address(0) == 0x5C);
    ASSERT(i2c_test_get_write_to_device_address(1) == 0x5C);
    ASSERT(i2c_test_get_driver_delete_calls() == 2);

    cJSON_Delete(input);
    return 0;
}

TEST(handler_rejects_unsupported_address_before_i2c_init)
{
    cJSON *input = cJSON_Parse("{\"sda_pin\":4,\"scl_pin\":5,\"address\":64}");
    char result[128] = {0};

    ASSERT(input != NULL);
    i2c_test_reset();
    mock_freertos_reset();

    ASSERT(!tools_bh1750_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "BH1750 address must be");
    ASSERT(i2c_test_get_param_config_calls() == 0);
    ASSERT(i2c_test_get_driver_install_calls() == 0);
    ASSERT(mock_freertos_delay_count() == 0);

    cJSON_Delete(input);
    return 0;
}

TEST(handler_reports_power_on_failure)
{
    cJSON *input = cJSON_Parse("{\"sda_pin\":4,\"scl_pin\":5}");
    char result[128] = {0};

    ASSERT(input != NULL);
    i2c_test_reset();
    mock_freertos_reset();
    i2c_test_set_write_to_device_result(ESP_FAIL);

    ASSERT(!tools_bh1750_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "power on failed");
    ASSERT(i2c_test_get_write_to_device_calls() == 1);
    ASSERT(i2c_test_get_driver_delete_calls() == 2);
    ASSERT(mock_freertos_delay_count() == 0);

    cJSON_Delete(input);
    return 0;
}

TEST(handler_reports_measurement_start_failure)
{
    cJSON *input = cJSON_Parse("{\"sda_pin\":4,\"scl_pin\":5}");
    char result[128] = {0};

    ASSERT(input != NULL);
    i2c_test_reset();
    mock_freertos_reset();
    ASSERT(i2c_test_push_write_to_device_result(ESP_OK));
    ASSERT(i2c_test_push_write_to_device_result(ESP_FAIL));

    ASSERT(!tools_bh1750_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "start measurement failed");
    ASSERT(i2c_test_get_write_to_device_calls() == 2);
    ASSERT(i2c_test_get_driver_delete_calls() == 2);
    ASSERT(mock_freertos_delay_count() == 0);

    cJSON_Delete(input);
    return 0;
}

TEST(handler_reports_read_failure_after_delay)
{
    cJSON *input = cJSON_Parse("{\"sda_pin\":4,\"scl_pin\":5}");
    char result[128] = {0};

    ASSERT(input != NULL);
    i2c_test_reset();
    mock_freertos_reset();
    i2c_test_set_write_to_device_result(ESP_OK);
    i2c_test_set_read_from_device_result(ESP_FAIL);

    ASSERT(!tools_bh1750_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "read failed");
    ASSERT(i2c_test_get_write_to_device_calls() == 2);
    ASSERT(i2c_test_get_driver_delete_calls() == 2);
    ASSERT(mock_freertos_delay_count() == 1);
    ASSERT(mock_freertos_delay_at(0) == pdMS_TO_TICKS(180));

    cJSON_Delete(input);
    return 0;
}

int test_tools_bh1750_all(void)
{
    int failures = 0;

    printf("\nBH1750 Tool Tests:\n");

    printf("  handler_defaults_to_primary_address_and_formats_lux... ");
    if (test_handler_defaults_to_primary_address_and_formats_lux() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  handler_accepts_alternate_address... ");
    if (test_handler_accepts_alternate_address() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  handler_rejects_unsupported_address_before_i2c_init... ");
    if (test_handler_rejects_unsupported_address_before_i2c_init() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  handler_reports_power_on_failure... ");
    if (test_handler_reports_power_on_failure() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  handler_reports_measurement_start_failure... ");
    if (test_handler_reports_measurement_start_failure() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  handler_reports_read_failure_after_delay... ");
    if (test_handler_reports_read_failure_after_delay() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
