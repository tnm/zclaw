/*
 * Tool input parsing tests
 */

#include <stdio.h>
#include <string.h>
#include <cjson/cJSON.h>
#include "mock_esp.h"

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while(0)
#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("  FAIL: '%s' != '%s' (line %d)\n", (a), (b), __LINE__); \
        return 1; \
    } \
} while(0)

/*
 * Test parsing gpio_write input
 */
TEST(gpio_write_input)
{
    const char *json = "{\"pin\": 2, \"value\": 1}";

    cJSON *input = cJSON_Parse(json);
    ASSERT(input != NULL);

    cJSON *pin = cJSON_GetObjectItem(input, "pin");
    ASSERT(pin != NULL);
    ASSERT(cJSON_IsNumber(pin));
    ASSERT(pin->valueint == 2);

    cJSON *value = cJSON_GetObjectItem(input, "value");
    ASSERT(value != NULL);
    ASSERT(cJSON_IsNumber(value));
    ASSERT(value->valueint == 1);

    cJSON_Delete(input);
    return 0;
}

/*
 * Test parsing memory_set input
 */
TEST(memory_set_input)
{
    const char *json = "{\"key\": \"my_setting\", \"value\": \"hello world\"}";

    cJSON *input = cJSON_Parse(json);
    ASSERT(input != NULL);

    cJSON *key = cJSON_GetObjectItem(input, "key");
    ASSERT(key != NULL);
    ASSERT(cJSON_IsString(key));
    ASSERT_STR_EQ(key->valuestring, "my_setting");

    cJSON *value = cJSON_GetObjectItem(input, "value");
    ASSERT(value != NULL);
    ASSERT(cJSON_IsString(value));
    ASSERT_STR_EQ(value->valuestring, "hello world");

    cJSON_Delete(input);
    return 0;
}

/*
 * Test parsing cron_set input (periodic)
 */
TEST(cron_set_periodic)
{
    const char *json = "{\"type\": \"periodic\", \"interval_minutes\": 30, \"action\": \"check temperature\"}";

    cJSON *input = cJSON_Parse(json);
    ASSERT(input != NULL);

    cJSON *type = cJSON_GetObjectItem(input, "type");
    ASSERT_STR_EQ(type->valuestring, "periodic");

    cJSON *interval = cJSON_GetObjectItem(input, "interval_minutes");
    ASSERT(interval->valueint == 30);

    cJSON *action = cJSON_GetObjectItem(input, "action");
    ASSERT_STR_EQ(action->valuestring, "check temperature");

    cJSON_Delete(input);
    return 0;
}

/*
 * Test parsing cron_set input (daily)
 */
TEST(cron_set_daily)
{
    const char *json = "{\"type\": \"daily\", \"hour\": 8, \"minute\": 30, \"action\": \"good morning\"}";

    cJSON *input = cJSON_Parse(json);
    ASSERT(input != NULL);

    cJSON *type = cJSON_GetObjectItem(input, "type");
    ASSERT_STR_EQ(type->valuestring, "daily");

    cJSON *hour = cJSON_GetObjectItem(input, "hour");
    ASSERT(hour->valueint == 8);

    cJSON *minute = cJSON_GetObjectItem(input, "minute");
    ASSERT(minute->valueint == 30);

    cJSON_Delete(input);
    return 0;
}

/*
 * Test missing required field
 */
TEST(missing_field)
{
    const char *json = "{\"pin\": 2}";  // missing "value"

    cJSON *input = cJSON_Parse(json);
    ASSERT(input != NULL);

    cJSON *value = cJSON_GetObjectItem(input, "value");
    ASSERT(value == NULL);  // Should be NULL

    cJSON_Delete(input);
    return 0;
}

/*
 * Test invalid JSON
 */
TEST(invalid_json)
{
    const char *json = "{invalid json}";

    cJSON *input = cJSON_Parse(json);
    ASSERT(input == NULL);  // Should fail to parse

    return 0;
}

/*
 * Run all tool parsing tests
 */
int test_tools_parse_all(void)
{
    int failures = 0;

    printf("\nTool Parsing Tests:\n");

    printf("  gpio_write_input... ");
    if (test_gpio_write_input() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  memory_set_input... ");
    if (test_memory_set_input() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  cron_set_periodic... ");
    if (test_cron_set_periodic() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  cron_set_daily... ");
    if (test_cron_set_daily() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  missing_field... ");
    if (test_missing_field() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  invalid_json... ");
    if (test_invalid_json() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
