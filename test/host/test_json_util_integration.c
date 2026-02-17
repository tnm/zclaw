/*
 * Integration tests for production json_util.c request/response handling
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "json_util.h"
#include "tools.h"
#include "mock_llm.h"
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

static bool dummy_tool_execute(const cJSON *input, char *result, size_t result_len)
{
    (void)input;
    snprintf(result, result_len, "ok");
    return true;
}

static const tool_def_t s_test_tools[] = {
    {
        .name = "gpio_write",
        .description = "Toggle a GPIO pin.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"}},\"required\":[\"pin\"]}",
        .execute = dummy_tool_execute
    }
};

TEST(build_anthropic_request)
{
    mock_llm_set_backend(LLM_BACKEND_ANTHROPIC, "claude-test-model");

    char *request = json_build_request("sys prompt", NULL, 0, "hello",
                                       s_test_tools, 1);
    ASSERT(request != NULL);

    cJSON *root = cJSON_Parse(request);
    ASSERT(root != NULL);

    cJSON *model = cJSON_GetObjectItem(root, "model");
    ASSERT(model != NULL && cJSON_IsString(model));
    ASSERT_STR_EQ(model->valuestring, "claude-test-model");

    cJSON *system = cJSON_GetObjectItem(root, "system");
    ASSERT(system != NULL && cJSON_IsString(system));
    ASSERT_STR_EQ(system->valuestring, "sys prompt");

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    ASSERT(messages != NULL && cJSON_IsArray(messages));
    ASSERT(cJSON_GetArraySize(messages) == 1);

    cJSON *tools = cJSON_GetObjectItem(root, "tools");
    ASSERT(tools != NULL && cJSON_IsArray(tools));
    ASSERT(cJSON_GetArraySize(tools) == 1);
    cJSON *tool = cJSON_GetArrayItem(tools, 0);
    ASSERT(tool != NULL);
    cJSON *input_schema = cJSON_GetObjectItem(tool, "input_schema");
    ASSERT(input_schema != NULL && cJSON_IsObject(input_schema));

    cJSON_Delete(root);
    free(request);
    return 0;
}

TEST(build_openai_request)
{
    mock_llm_set_backend(LLM_BACKEND_OPENAI, "gpt-test-model");

    char *request = json_build_request("sys prompt", NULL, 0, "hello",
                                       s_test_tools, 1);
    ASSERT(request != NULL);

    cJSON *root = cJSON_Parse(request);
    ASSERT(root != NULL);

    cJSON *model = cJSON_GetObjectItem(root, "model");
    ASSERT(model != NULL && cJSON_IsString(model));
    ASSERT_STR_EQ(model->valuestring, "gpt-test-model");

    cJSON *system = cJSON_GetObjectItem(root, "system");
    ASSERT(system == NULL);

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    ASSERT(messages != NULL && cJSON_IsArray(messages));
    ASSERT(cJSON_GetArraySize(messages) == 2);

    cJSON *first = cJSON_GetArrayItem(messages, 0);
    ASSERT(first != NULL);
    cJSON *role = cJSON_GetObjectItem(first, "role");
    ASSERT(role != NULL && cJSON_IsString(role));
    ASSERT_STR_EQ(role->valuestring, "system");

    cJSON *tools = cJSON_GetObjectItem(root, "tools");
    ASSERT(tools != NULL && cJSON_IsArray(tools));
    ASSERT(cJSON_GetArraySize(tools) == 1);
    cJSON *tool = cJSON_GetArrayItem(tools, 0);
    ASSERT(tool != NULL);
    cJSON *type = cJSON_GetObjectItem(tool, "type");
    ASSERT(type != NULL && cJSON_IsString(type));
    ASSERT_STR_EQ(type->valuestring, "function");

    cJSON_Delete(root);
    free(request);
    return 0;
}

TEST(parse_anthropic_tool_use)
{
    mock_llm_set_backend(LLM_BACKEND_ANTHROPIC, "claude-test-model");

    const char *response = "{"
        "\"content\":["
        "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"gpio_write\","
        "\"input\":{\"pin\":10,\"state\":1}}"
        "]"
    "}";

    char text[256] = {0};
    char tool_name[32] = {0};
    char tool_id[64] = {0};
    cJSON *tool_input = NULL;

    ASSERT(json_parse_response(response, text, sizeof(text),
                               tool_name, sizeof(tool_name),
                               tool_id, sizeof(tool_id),
                               &tool_input));
    ASSERT_STR_EQ(tool_name, "gpio_write");
    ASSERT_STR_EQ(tool_id, "toolu_1");
    ASSERT(tool_input != NULL);
    ASSERT(cJSON_GetObjectItem(tool_input, "pin")->valueint == 10);
    ASSERT(cJSON_GetObjectItem(tool_input, "state")->valueint == 1);

    json_free_parsed_response();
    return 0;
}

TEST(parse_openai_tool_call)
{
    mock_llm_set_backend(LLM_BACKEND_OPENAI, "gpt-test-model");

    const char *response = "{"
        "\"choices\":[{"
            "\"message\":{"
                "\"role\":\"assistant\","
                "\"content\":null,"
                "\"tool_calls\":[{"
                    "\"id\":\"call_abc\","
                    "\"type\":\"function\","
                    "\"function\":{"
                        "\"name\":\"memory_set\","
                        "\"arguments\":\"{\\\"key\\\":\\\"name\\\",\\\"value\\\":\\\"alice\\\"}\""
                    "}"
                "}]"
            "}"
        "}]"
    "}";

    char text[256] = {0};
    char tool_name[32] = {0};
    char tool_id[64] = {0};
    cJSON *tool_input = NULL;

    ASSERT(json_parse_response(response, text, sizeof(text),
                               tool_name, sizeof(tool_name),
                               tool_id, sizeof(tool_id),
                               &tool_input));
    ASSERT_STR_EQ(tool_name, "memory_set");
    ASSERT_STR_EQ(tool_id, "call_abc");
    ASSERT(tool_input != NULL);
    ASSERT_STR_EQ(cJSON_GetObjectItem(tool_input, "key")->valuestring, "name");
    ASSERT_STR_EQ(cJSON_GetObjectItem(tool_input, "value")->valuestring, "alice");

    json_free_parsed_response();
    return 0;
}

TEST(parse_api_error)
{
    mock_llm_set_backend(LLM_BACKEND_OPENAI, "gpt-test-model");

    const char *response = "{"
        "\"error\":{\"message\":\"Invalid API key\"}"
    "}";

    char text[256] = {0};
    char tool_name[32] = {0};
    char tool_id[64] = {0};
    cJSON *tool_input = NULL;

    ASSERT(json_parse_response(response, text, sizeof(text),
                               tool_name, sizeof(tool_name),
                               tool_id, sizeof(tool_id),
                               &tool_input));
    ASSERT(strstr(text, "Invalid API key") != NULL);
    ASSERT(tool_name[0] == '\0');
    ASSERT(tool_id[0] == '\0');
    ASSERT(tool_input == NULL);

    json_free_parsed_response();
    return 0;
}

int test_json_util_integration_all(void)
{
    int failures = 0;

    printf("\njson_util Integration Tests:\n");

    printf("  build_anthropic_request... ");
    if (test_build_anthropic_request() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  build_openai_request... ");
    if (test_build_openai_request() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  parse_anthropic_tool_use... ");
    if (test_parse_anthropic_tool_use() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  parse_openai_tool_call... ");
    if (test_parse_openai_tool_call() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  parse_api_error... ");
    if (test_parse_api_error() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
