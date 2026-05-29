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
                                       s_test_tools, 1, NULL);
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
    mock_llm_set_backend(LLM_BACKEND_OPENAI, "gpt-5.4");

    char *request = json_build_request("sys prompt", NULL, 0, "hello",
                                       s_test_tools, 1, NULL);
    ASSERT(request != NULL);

    cJSON *root = cJSON_Parse(request);
    ASSERT(root != NULL);

    cJSON *model = cJSON_GetObjectItem(root, "model");
    ASSERT(model != NULL && cJSON_IsString(model));
    ASSERT_STR_EQ(model->valuestring, "gpt-5.4");

    cJSON *max_completion_tokens = cJSON_GetObjectItem(root, "max_completion_tokens");
    ASSERT(max_completion_tokens != NULL && cJSON_IsNumber(max_completion_tokens));
    cJSON *max_tokens = cJSON_GetObjectItem(root, "max_tokens");
    ASSERT(max_tokens == NULL);

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

TEST(build_openrouter_request)
{
    mock_llm_set_backend(LLM_BACKEND_OPENROUTER, "openrouter-test-model");

    char *request = json_build_request("sys prompt", NULL, 0, "hello",
                                       s_test_tools, 1, NULL);
    ASSERT(request != NULL);

    cJSON *root = cJSON_Parse(request);
    ASSERT(root != NULL);

    cJSON *max_tokens = cJSON_GetObjectItem(root, "max_tokens");
    ASSERT(max_tokens != NULL && cJSON_IsNumber(max_tokens));
    cJSON *max_completion_tokens = cJSON_GetObjectItem(root, "max_completion_tokens");
    ASSERT(max_completion_tokens == NULL);

    cJSON_Delete(root);
    free(request);
    return 0;
}

TEST(build_azure_openai_request)
{
    mock_llm_set_backend(LLM_BACKEND_AZURE_OPENAI, "azure/gpt-5-mini");

    char *request = json_build_request("sys prompt", NULL, 0, "hello",
                                       s_test_tools, 1, NULL);
    ASSERT(request != NULL);

    cJSON *root = cJSON_Parse(request);
    ASSERT(root != NULL);

    cJSON *max_output_tokens = cJSON_GetObjectItem(root, "max_output_tokens");
    ASSERT(max_output_tokens != NULL && cJSON_IsNumber(max_output_tokens));

    cJSON *input = cJSON_GetObjectItem(root, "input");
    ASSERT(input != NULL && cJSON_IsArray(input));
    ASSERT(cJSON_GetArraySize(input) == 1);

    cJSON *instructions = cJSON_GetObjectItem(root, "instructions");
    ASSERT(instructions != NULL && cJSON_IsString(instructions));
    ASSERT_STR_EQ(instructions->valuestring, "sys prompt");

    cJSON *parallel_tool_calls = cJSON_GetObjectItem(root, "parallel_tool_calls");
    ASSERT(parallel_tool_calls != NULL && cJSON_IsFalse(parallel_tool_calls));

    cJSON *reasoning = cJSON_GetObjectItem(root, "reasoning");
    ASSERT(reasoning != NULL && cJSON_IsObject(reasoning));
    cJSON *effort = cJSON_GetObjectItem(reasoning, "effort");
    ASSERT(effort != NULL && cJSON_IsString(effort));
    ASSERT_STR_EQ(effort->valuestring, "low");

    cJSON *first = cJSON_GetArrayItem(input, 0);
    ASSERT(first != NULL);
    cJSON *first_role = cJSON_GetObjectItem(first, "role");
    ASSERT(first_role != NULL && cJSON_IsString(first_role));
    ASSERT_STR_EQ(first_role->valuestring, "user");

    cJSON *tools = cJSON_GetObjectItem(root, "tools");
    ASSERT(tools != NULL && cJSON_IsArray(tools));
    ASSERT(cJSON_GetArraySize(tools) == 1);
    cJSON *tool = cJSON_GetArrayItem(tools, 0);
    ASSERT(tool != NULL);
    cJSON *type = cJSON_GetObjectItem(tool, "type");
    ASSERT(type != NULL && cJSON_IsString(type));
    ASSERT_STR_EQ(type->valuestring, "function");
    cJSON *name = cJSON_GetObjectItem(tool, "name");
    ASSERT(name != NULL && cJSON_IsString(name));
    ASSERT_STR_EQ(name->valuestring, "gpio_write");

    cJSON_Delete(root);
    free(request);
    return 0;
}

TEST(build_azure_openai_request_with_previous_response_id)
{
    mock_llm_set_backend(LLM_BACKEND_AZURE_OPENAI, "azure/gpt-5-mini");

    conversation_msg_t history[1] = {0};
    strncpy(history[0].role, "user", sizeof(history[0].role) - 1);
    strncpy(history[0].content, "hello again", sizeof(history[0].content) - 1);

    char *request = json_build_request("sys prompt", history, 1, NULL,
                                       s_test_tools, 1, "resp_prev_123");
    ASSERT(request != NULL);

    cJSON *root = cJSON_Parse(request);
    ASSERT(root != NULL);

    cJSON *previous_response_id = cJSON_GetObjectItem(root, "previous_response_id");
    ASSERT(previous_response_id != NULL && cJSON_IsString(previous_response_id));
    ASSERT_STR_EQ(previous_response_id->valuestring, "resp_prev_123");

    cJSON *input = cJSON_GetObjectItem(root, "input");
    ASSERT(input != NULL && cJSON_IsArray(input));
    ASSERT(cJSON_GetArraySize(input) == 1);

    cJSON *first = cJSON_GetArrayItem(input, 0);
    ASSERT(first != NULL);
    cJSON *role = cJSON_GetObjectItem(first, "role");
    ASSERT(role != NULL && cJSON_IsString(role));
    ASSERT_STR_EQ(role->valuestring, "user");

    cJSON_Delete(root);
    free(request);
    return 0;
}

TEST(build_openai_request_skips_orphan_tool_result)
{
    mock_llm_set_backend(LLM_BACKEND_OPENAI, "gpt-test-model");

    conversation_msg_t history[2] = {0};

    strncpy(history[0].role, "user", sizeof(history[0].role) - 1);
    strncpy(history[0].content, "tool completed", sizeof(history[0].content) - 1);
    history[0].is_tool_result = true;
    strncpy(history[0].tool_id, "call_orphan", sizeof(history[0].tool_id) - 1);

    strncpy(history[1].role, "user", sizeof(history[1].role) - 1);
    strncpy(history[1].content, "remember my name is Ted", sizeof(history[1].content) - 1);

    char *request = json_build_request("sys prompt", history, 2, NULL, s_test_tools, 1, NULL);
    ASSERT(request != NULL);

    cJSON *root = cJSON_Parse(request);
    ASSERT(root != NULL);

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    ASSERT(messages != NULL && cJSON_IsArray(messages));
    ASSERT(cJSON_GetArraySize(messages) == 2);

    cJSON *first = cJSON_GetArrayItem(messages, 0);
    ASSERT(first != NULL);
    cJSON *first_role = cJSON_GetObjectItem(first, "role");
    ASSERT(first_role != NULL && cJSON_IsString(first_role));
    ASSERT_STR_EQ(first_role->valuestring, "system");

    cJSON *second = cJSON_GetArrayItem(messages, 1);
    ASSERT(second != NULL);
    cJSON *second_role = cJSON_GetObjectItem(second, "role");
    ASSERT(second_role != NULL && cJSON_IsString(second_role));
    ASSERT_STR_EQ(second_role->valuestring, "user");

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

TEST(parse_azure_responses_tool_call)
{
    mock_llm_set_backend(LLM_BACKEND_AZURE_OPENAI, "gpt-5.4");

    const char *response = "{"
        "\"output\":[{"
            "\"type\":\"function_call\","
            "\"call_id\":\"call_resp_1\","
            "\"name\":\"memory_set\","
            "\"arguments\":\"{\\\"key\\\":\\\"name\\\",\\\"value\\\":\\\"alice\\\"}\""
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
    ASSERT_STR_EQ(tool_id, "call_resp_1");
    ASSERT(tool_input != NULL);
    ASSERT_STR_EQ(cJSON_GetObjectItem(tool_input, "key")->valuestring, "name");
    ASSERT_STR_EQ(cJSON_GetObjectItem(tool_input, "value")->valuestring, "alice");

    json_free_parsed_response();
    return 0;
}

TEST(parse_azure_responses_text_ignores_null_error_field)
{
    mock_llm_set_backend(LLM_BACKEND_AZURE_OPENAI, "gpt-5.4");

    const char *response = "{"
        "\"error\":null,"
        "\"output\":[{"
            "\"type\":\"message\","
            "\"role\":\"assistant\","
            "\"content\":[{"
                "\"type\":\"output_text\","
                "\"text\":\"hello from azure\""
            "}]"
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
    ASSERT_STR_EQ(text, "hello from azure");
    ASSERT(tool_name[0] == '\0');
    ASSERT(tool_id[0] == '\0');
    ASSERT(tool_input == NULL);

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

    printf("  build_openrouter_request... ");
    if (test_build_openrouter_request() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  build_azure_openai_request... ");
    if (test_build_azure_openai_request() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  build_azure_openai_request_with_previous_response_id... ");
    if (test_build_azure_openai_request_with_previous_response_id() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  build_openai_request_skips_orphan_tool_result... ");
    if (test_build_openai_request_skips_orphan_tool_result() == 0) {
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

    printf("  parse_azure_responses_tool_call... ");
    if (test_parse_azure_responses_tool_call() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  parse_azure_responses_text_ignores_null_error_field... ");
    if (test_parse_azure_responses_text_ignores_null_error_field() == 0) {
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
