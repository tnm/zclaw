/*
 * JSON parsing tests
 */

#include <stdio.h>
#include <stdlib.h>
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
 * Test parsing Claude API response
 */
TEST(parse_text_response)
{
    const char *json = "{"
        "\"content\": ["
        "  {\"type\": \"text\", \"text\": \"Hello, world!\"}"
        "],"
        "\"stop_reason\": \"end_turn\""
        "}";

    cJSON *root = cJSON_Parse(json);
    ASSERT(root != NULL);

    cJSON *content = cJSON_GetObjectItem(root, "content");
    ASSERT(content != NULL);
    ASSERT(cJSON_IsArray(content));

    cJSON *block = cJSON_GetArrayItem(content, 0);
    ASSERT(block != NULL);

    cJSON *type = cJSON_GetObjectItem(block, "type");
    ASSERT(type != NULL);
    ASSERT_STR_EQ(type->valuestring, "text");

    cJSON *text = cJSON_GetObjectItem(block, "text");
    ASSERT(text != NULL);
    ASSERT_STR_EQ(text->valuestring, "Hello, world!");

    cJSON_Delete(root);
    return 0;
}

/*
 * Test parsing tool_use response
 */
TEST(parse_tool_use_response)
{
    const char *json = "{"
        "\"content\": ["
        "  {\"type\": \"text\", \"text\": \"I'll turn on the LED.\"},"
        "  {\"type\": \"tool_use\", \"id\": \"toolu_123\", \"name\": \"gpio_write\","
        "   \"input\": {\"pin\": 2, \"value\": 1}}"
        "],"
        "\"stop_reason\": \"tool_use\""
        "}";

    cJSON *root = cJSON_Parse(json);
    ASSERT(root != NULL);

    cJSON *content = cJSON_GetObjectItem(root, "content");
    ASSERT(cJSON_GetArraySize(content) == 2);

    // Check tool_use block
    cJSON *tool_block = cJSON_GetArrayItem(content, 1);
    cJSON *type = cJSON_GetObjectItem(tool_block, "type");
    ASSERT_STR_EQ(type->valuestring, "tool_use");

    cJSON *name = cJSON_GetObjectItem(tool_block, "name");
    ASSERT_STR_EQ(name->valuestring, "gpio_write");

    cJSON *id = cJSON_GetObjectItem(tool_block, "id");
    ASSERT_STR_EQ(id->valuestring, "toolu_123");

    cJSON *input = cJSON_GetObjectItem(tool_block, "input");
    ASSERT(input != NULL);

    cJSON *pin = cJSON_GetObjectItem(input, "pin");
    ASSERT(pin != NULL);
    ASSERT(pin->valueint == 2);

    cJSON *value = cJSON_GetObjectItem(input, "value");
    ASSERT(value != NULL);
    ASSERT(value->valueint == 1);

    cJSON_Delete(root);
    return 0;
}

/*
 * Test parsing error response
 */
TEST(parse_error_response)
{
    const char *json = "{"
        "\"error\": {"
        "  \"type\": \"invalid_request_error\","
        "  \"message\": \"Invalid API key\""
        "}"
        "}";

    cJSON *root = cJSON_Parse(json);
    ASSERT(root != NULL);

    cJSON *error = cJSON_GetObjectItem(root, "error");
    ASSERT(error != NULL);

    cJSON *msg = cJSON_GetObjectItem(error, "message");
    ASSERT(msg != NULL);
    ASSERT_STR_EQ(msg->valuestring, "Invalid API key");

    cJSON_Delete(root);
    return 0;
}

/*
 * Test building request JSON
 */
TEST(build_request)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "claude-sonnet-4-20250514");
    cJSON_AddNumberToObject(root, "max_tokens", 1024);
    cJSON_AddStringToObject(root, "system", "You are a helpful assistant.");

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", "Hello");
    cJSON_AddItemToArray(messages, msg);

    char *json_str = cJSON_PrintUnformatted(root);
    ASSERT(json_str != NULL);
    ASSERT(strstr(json_str, "claude-sonnet-4-20250514") != NULL);
    ASSERT(strstr(json_str, "Hello") != NULL);

    free(json_str);
    cJSON_Delete(root);
    return 0;
}

/*
 * Run all JSON tests
 */
int test_json_all(void)
{
    int failures = 0;

    printf("JSON Tests:\n");

    printf("  parse_text_response... ");
    if (test_parse_text_response() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  parse_tool_use_response... ");
    if (test_parse_tool_use_response() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  parse_error_response... ");
    if (test_parse_error_response() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  build_request... ");
    if (test_build_request() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
