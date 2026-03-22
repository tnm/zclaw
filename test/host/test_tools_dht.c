/*
 * Host tests for DHT tool decode and handler behavior.
 */

#include <stdio.h>
#include <string.h>

#include <cjson/cJSON.h>

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

TEST(decode_dht11_payload_formats_temperature_and_humidity)
{
    const uint8_t data[5] = {55, 0, 24, 0, 79};
    char result[128] = {0};

    ASSERT(tools_dht_test_decode_bytes("dht11", 5, data, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "DHT11 GPIO 5");
    ASSERT_STR_CONTAINS(result, "55.0% RH");
    ASSERT_STR_CONTAINS(result, "24.0 C");
    return 0;
}

TEST(decode_dht22_payload_formats_fractional_values)
{
    const uint8_t data[5] = {0x02, 0x2B, 0x00, 0xEB, 0x18};
    char result[128] = {0};

    ASSERT(tools_dht_test_decode_bytes("dht22", 4, data, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "DHT22 GPIO 4");
    ASSERT_STR_CONTAINS(result, "55.5% RH");
    ASSERT_STR_CONTAINS(result, "23.5 C");
    return 0;
}

TEST(decode_rejects_bad_checksum)
{
    const uint8_t data[5] = {55, 0, 24, 0, 0};
    char result[128] = {0};

    ASSERT(!tools_dht_test_decode_bytes("dht11", 5, data, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "checksum mismatch");
    return 0;
}

TEST(handler_uses_mocked_read_and_enforces_retry_validation)
{
    cJSON *input = cJSON_Parse("{\"pin\":5,\"model\":\"dht11\"}");
    char result[128] = {0};
    const uint8_t data[5] = {55, 0, 24, 0, 79};

    ASSERT(input != NULL);
    tools_dht_test_reset();
    tools_dht_test_set_mock_success(data);

    ASSERT(tools_dht_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "DHT11 GPIO 5");

    cJSON_Delete(input);
    return 0;
}

TEST(handler_reports_mock_failure)
{
    cJSON *input = cJSON_Parse("{\"pin\":5,\"model\":\"dht22\",\"retries\":1}");
    char result[128] = {0};

    ASSERT(input != NULL);
    tools_dht_test_reset();
    tools_dht_test_set_mock_failure("Error: no DHT response");

    ASSERT(!tools_dht_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "no DHT response");

    cJSON_Delete(input);
    return 0;
}

TEST(handler_rejects_invalid_retry_count)
{
    cJSON *input = cJSON_Parse("{\"pin\":5,\"model\":\"dht11\",\"retries\":4}");
    char result[128] = {0};

    ASSERT(input != NULL);
    tools_dht_test_reset();
    mock_freertos_reset();

    ASSERT(!tools_dht_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "retries must be 1-3");
    ASSERT(mock_freertos_delay_count() == 0);

    cJSON_Delete(input);
    return 0;
}

TEST(handler_rejects_invalid_model)
{
    cJSON *input = cJSON_Parse("{\"pin\":5,\"model\":\"dht99\"}");
    char result[128] = {0};

    ASSERT(input != NULL);
    tools_dht_test_reset();

    ASSERT(!tools_dht_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "model must be");

    cJSON_Delete(input);
    return 0;
}

TEST(handler_rejects_disallowed_pin)
{
    cJSON *input = cJSON_Parse("{\"pin\":1,\"model\":\"dht11\"}");
    char result[128] = {0};

    ASSERT(input != NULL);
    tools_dht_test_reset();

    ASSERT(!tools_dht_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "pin");

    cJSON_Delete(input);
    return 0;
}

TEST(handler_enforces_min_interval_between_successive_reads)
{
    cJSON *input = cJSON_Parse("{\"pin\":5,\"model\":\"dht11\"}");
    char result[128] = {0};
    const uint8_t data[5] = {55, 0, 24, 0, 79};

    ASSERT(input != NULL);
    tools_dht_test_reset();
    mock_freertos_reset();
    tools_dht_test_set_mock_success(data);

    ASSERT(tools_dht_read_handler(input, result, sizeof(result)));
    ASSERT(tools_dht_read_handler(input, result, sizeof(result)));
    ASSERT(mock_freertos_delay_count() >= 1);
    ASSERT(mock_freertos_delay_at(0) >= 900);

    cJSON_Delete(input);
    return 0;
}

int test_tools_dht_all(void)
{
    int failures = 0;

    printf("\nDHT Tool Tests:\n");

    printf("  decode_dht11_payload_formats_temperature_and_humidity... ");
    if (test_decode_dht11_payload_formats_temperature_and_humidity() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  decode_dht22_payload_formats_fractional_values... ");
    if (test_decode_dht22_payload_formats_fractional_values() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  decode_rejects_bad_checksum... ");
    if (test_decode_rejects_bad_checksum() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  handler_uses_mocked_read_and_enforces_retry_validation... ");
    if (test_handler_uses_mocked_read_and_enforces_retry_validation() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  handler_reports_mock_failure... ");
    if (test_handler_reports_mock_failure() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  handler_rejects_invalid_retry_count... ");
    if (test_handler_rejects_invalid_retry_count() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  handler_rejects_invalid_model... ");
    if (test_handler_rejects_invalid_model() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  handler_rejects_disallowed_pin... ");
    if (test_handler_rejects_disallowed_pin() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  handler_enforces_min_interval_between_successive_reads... ");
    if (test_handler_enforces_min_interval_between_successive_reads() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
