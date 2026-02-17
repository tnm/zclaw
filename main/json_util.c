#include "json_util.h"
#include "config.h"
#include "tools.h"
#include "user_tools.h"
#include "llm.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "json";

// Keep parsed response tree alive for tool_input access
static cJSON *s_parsed_response = NULL;

// -----------------------------------------------------------------------------
// Anthropic Format (Claude API)
// -----------------------------------------------------------------------------

static char *build_anthropic_request(
    const char *system_prompt,
    const conversation_msg_t *history,
    int history_len,
    const char *user_message,
    const tool_def_t *tools,
    int tool_count)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "model", llm_get_model());
    cJSON_AddNumberToObject(root, "max_tokens", LLM_MAX_TOKENS);
    cJSON_AddStringToObject(root, "system", system_prompt);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");

    // Add history
    for (int i = 0; i < history_len; i++) {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", history[i].role);

        if (history[i].is_tool_use) {
            cJSON *content = cJSON_AddArrayToObject(msg, "content");
            cJSON *tool_use = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_use, "type", "tool_use");
            cJSON_AddStringToObject(tool_use, "id", history[i].tool_id);
            cJSON_AddStringToObject(tool_use, "name", history[i].tool_name);
            cJSON *input = cJSON_Parse(history[i].content);
            cJSON_AddItemToObject(tool_use, "input", input ? input : cJSON_CreateObject());
            cJSON_AddItemToArray(content, tool_use);
        } else if (history[i].is_tool_result) {
            cJSON *content = cJSON_AddArrayToObject(msg, "content");
            cJSON *tool_result = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_result, "type", "tool_result");
            cJSON_AddStringToObject(tool_result, "tool_use_id", history[i].tool_id);
            cJSON_AddStringToObject(tool_result, "content", history[i].content);
            cJSON_AddItemToArray(content, tool_result);
        } else {
            cJSON_AddStringToObject(msg, "content", history[i].content);
        }

        cJSON_AddItemToArray(messages, msg);
    }

    // Add new user message
    if (user_message && user_message[0] != '\0') {
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", user_message);
        cJSON_AddItemToArray(messages, user_msg);
    }

    // Tools array (built-in + user-defined)
    int user_tool_count = user_tools_count();
    if (tool_count > 0 || user_tool_count > 0) {
        cJSON *tools_arr = cJSON_AddArrayToObject(root, "tools");

        // Built-in tools
        for (int i = 0; i < tool_count; i++) {
            cJSON *tool = cJSON_CreateObject();
            cJSON_AddStringToObject(tool, "name", tools[i].name);
            cJSON_AddStringToObject(tool, "description", tools[i].description);
            cJSON *schema = cJSON_Parse(tools[i].input_schema_json);
            if (schema) cJSON_AddItemToObject(tool, "input_schema", schema);
            cJSON_AddItemToArray(tools_arr, tool);
        }

        // User-defined tools
        user_tool_t user_tools_arr[MAX_DYNAMIC_TOOLS];
        int loaded = user_tools_get_all(user_tools_arr, MAX_DYNAMIC_TOOLS);
        for (int i = 0; i < loaded; i++) {
            cJSON *tool = cJSON_CreateObject();
            cJSON_AddStringToObject(tool, "name", user_tools_arr[i].name);
            cJSON_AddStringToObject(tool, "description", user_tools_arr[i].description);
            // User tools take no input - just call them
            cJSON *schema = cJSON_CreateObject();
            cJSON_AddStringToObject(schema, "type", "object");
            cJSON_AddItemToObject(schema, "properties", cJSON_CreateObject());
            cJSON_AddItemToObject(tool, "input_schema", schema);
            cJSON_AddItemToArray(tools_arr, tool);
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

static bool parse_anthropic_response(
    cJSON *root,
    char *text_out,
    size_t text_out_len,
    char *tool_name_out,
    size_t tool_name_len,
    char *tool_id_out,
    size_t tool_id_len,
    cJSON **tool_input_out)
{
    cJSON *content = cJSON_GetObjectItem(root, "content");
    if (!content || !cJSON_IsArray(content)) {
        ESP_LOGE(TAG, "No content array in response");
        return false;
    }

    cJSON *block;
    cJSON_ArrayForEach(block, content) {
        cJSON *type = cJSON_GetObjectItem(block, "type");
        if (!type || !cJSON_IsString(type)) continue;

        if (strcmp(type->valuestring, "text") == 0) {
            cJSON *text = cJSON_GetObjectItem(block, "text");
            if (text && cJSON_IsString(text)) {
                strncpy(text_out, text->valuestring, text_out_len - 1);
                text_out[text_out_len - 1] = '\0';
            }
        } else if (strcmp(type->valuestring, "tool_use") == 0) {
            cJSON *name = cJSON_GetObjectItem(block, "name");
            cJSON *id = cJSON_GetObjectItem(block, "id");
            cJSON *input = cJSON_GetObjectItem(block, "input");

            if (name && cJSON_IsString(name)) {
                strncpy(tool_name_out, name->valuestring, tool_name_len - 1);
                tool_name_out[tool_name_len - 1] = '\0';
            }
            if (id && cJSON_IsString(id)) {
                strncpy(tool_id_out, id->valuestring, tool_id_len - 1);
                tool_id_out[tool_id_len - 1] = '\0';
            }
            if (input) {
                *tool_input_out = input;
            }
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// OpenAI Format (OpenAI, OpenRouter)
// -----------------------------------------------------------------------------

static char *build_openai_request(
    const char *system_prompt,
    const conversation_msg_t *history,
    int history_len,
    const char *user_message,
    const tool_def_t *tools,
    int tool_count)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "model", llm_get_model());
    cJSON_AddNumberToObject(root, "max_tokens", LLM_MAX_TOKENS);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");

    // System message first
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", system_prompt);
    cJSON_AddItemToArray(messages, sys_msg);

    // Add history
    for (int i = 0; i < history_len; i++) {
        cJSON *msg = cJSON_CreateObject();

        if (history[i].is_tool_use) {
            // Assistant message with tool_calls
            cJSON_AddStringToObject(msg, "role", "assistant");
            cJSON_AddNullToObject(msg, "content");

            cJSON *tool_calls = cJSON_AddArrayToObject(msg, "tool_calls");
            cJSON *tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "id", history[i].tool_id);
            cJSON_AddStringToObject(tc, "type", "function");

            cJSON *func = cJSON_CreateObject();
            cJSON_AddStringToObject(func, "name", history[i].tool_name);
            cJSON_AddStringToObject(func, "arguments", history[i].content);
            cJSON_AddItemToObject(tc, "function", func);

            cJSON_AddItemToArray(tool_calls, tc);
        } else if (history[i].is_tool_result) {
            // Tool response message
            cJSON_AddStringToObject(msg, "role", "tool");
            cJSON_AddStringToObject(msg, "tool_call_id", history[i].tool_id);
            cJSON_AddStringToObject(msg, "content", history[i].content);
        } else {
            // Regular message
            cJSON_AddStringToObject(msg, "role", history[i].role);
            cJSON_AddStringToObject(msg, "content", history[i].content);
        }

        cJSON_AddItemToArray(messages, msg);
    }

    // Add new user message
    if (user_message && user_message[0] != '\0') {
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", user_message);
        cJSON_AddItemToArray(messages, user_msg);
    }

    // Tools array (OpenAI format: built-in + user-defined)
    int user_tool_count = user_tools_count();
    if (tool_count > 0 || user_tool_count > 0) {
        cJSON *tools_arr = cJSON_AddArrayToObject(root, "tools");

        // Built-in tools
        for (int i = 0; i < tool_count; i++) {
            cJSON *tool = cJSON_CreateObject();
            cJSON_AddStringToObject(tool, "type", "function");

            cJSON *func = cJSON_CreateObject();
            cJSON_AddStringToObject(func, "name", tools[i].name);
            cJSON_AddStringToObject(func, "description", tools[i].description);
            cJSON *params = cJSON_Parse(tools[i].input_schema_json);
            if (params) cJSON_AddItemToObject(func, "parameters", params);

            cJSON_AddItemToObject(tool, "function", func);
            cJSON_AddItemToArray(tools_arr, tool);
        }

        // User-defined tools
        user_tool_t user_tools_arr[MAX_DYNAMIC_TOOLS];
        int loaded = user_tools_get_all(user_tools_arr, MAX_DYNAMIC_TOOLS);
        for (int i = 0; i < loaded; i++) {
            cJSON *tool = cJSON_CreateObject();
            cJSON_AddStringToObject(tool, "type", "function");

            cJSON *func = cJSON_CreateObject();
            cJSON_AddStringToObject(func, "name", user_tools_arr[i].name);
            cJSON_AddStringToObject(func, "description", user_tools_arr[i].description);
            // User tools take no input
            cJSON *params = cJSON_CreateObject();
            cJSON_AddStringToObject(params, "type", "object");
            cJSON_AddItemToObject(params, "properties", cJSON_CreateObject());
            cJSON_AddItemToObject(func, "parameters", params);

            cJSON_AddItemToObject(tool, "function", func);
            cJSON_AddItemToArray(tools_arr, tool);
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

static bool parse_openai_response(
    cJSON *root,
    char *text_out,
    size_t text_out_len,
    char *tool_name_out,
    size_t tool_name_len,
    char *tool_id_out,
    size_t tool_id_len,
    cJSON **tool_input_out)
{
    // OpenAI: choices[0].message
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        ESP_LOGE(TAG, "No choices in response");
        return false;
    }

    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    if (!message) {
        ESP_LOGE(TAG, "No message in choice");
        return false;
    }

    // Check for text content
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content && cJSON_IsString(content)) {
        strncpy(text_out, content->valuestring, text_out_len - 1);
        text_out[text_out_len - 1] = '\0';
    }

    // Check for tool_calls
    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (tool_calls && cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
        cJSON *tc = cJSON_GetArrayItem(tool_calls, 0);

        cJSON *id = cJSON_GetObjectItem(tc, "id");
        if (id && cJSON_IsString(id)) {
            strncpy(tool_id_out, id->valuestring, tool_id_len - 1);
            tool_id_out[tool_id_len - 1] = '\0';
        }

        cJSON *func = cJSON_GetObjectItem(tc, "function");
        if (func) {
            cJSON *name = cJSON_GetObjectItem(func, "name");
            if (name && cJSON_IsString(name)) {
                strncpy(tool_name_out, name->valuestring, tool_name_len - 1);
                tool_name_out[tool_name_len - 1] = '\0';
            }

            // Parse arguments string into JSON
            cJSON *args = cJSON_GetObjectItem(func, "arguments");
            if (args && cJSON_IsString(args)) {
                cJSON *parsed_args = cJSON_Parse(args->valuestring);
                if (!parsed_args) {
                    parsed_args = cJSON_CreateObject();
                }
                if (parsed_args) {
                    cJSON_AddItemToObject(tc, "_parsed_arguments", parsed_args);
                    *tool_input_out = parsed_args;
                }
            }
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

char *json_build_request(
    const char *system_prompt,
    const conversation_msg_t *history,
    int history_len,
    const char *user_message,
    const tool_def_t *tools,
    int tool_count)
{
    char *json_str;

    if (llm_is_openai_format()) {
        json_str = build_openai_request(system_prompt, history, history_len,
                                         user_message, tools, tool_count);
    } else {
        json_str = build_anthropic_request(system_prompt, history, history_len,
                                            user_message, tools, tool_count);
    }

    if (json_str) {
        ESP_LOGD(TAG, "Built request: %d bytes", (int)strlen(json_str));
    }

    return json_str;
}

bool json_parse_response(
    const char *response_json,
    char *text_out,
    size_t text_out_len,
    char *tool_name_out,
    size_t tool_name_len,
    char *tool_id_out,
    size_t tool_id_len,
    cJSON **tool_input_out)
{
    // Free any previous parsed response
    json_free_parsed_response();

    text_out[0] = '\0';
    tool_name_out[0] = '\0';
    tool_id_out[0] = '\0';
    *tool_input_out = NULL;

    s_parsed_response = cJSON_Parse(response_json);
    if (!s_parsed_response) {
        ESP_LOGE(TAG, "Failed to parse response JSON");
        return false;
    }

    // Check for error (both APIs use similar format)
    cJSON *error = cJSON_GetObjectItem(s_parsed_response, "error");
    if (error) {
        cJSON *msg = cJSON_GetObjectItem(error, "message");
        if (msg && cJSON_IsString(msg)) {
            snprintf(text_out, text_out_len, "API Error: %s", msg->valuestring);
        } else {
            snprintf(text_out, text_out_len, "API Error (unknown)");
        }
        return true;
    }

    // Parse based on format
    if (llm_is_openai_format()) {
        return parse_openai_response(s_parsed_response, text_out, text_out_len,
                                      tool_name_out, tool_name_len,
                                      tool_id_out, tool_id_len, tool_input_out);
    } else {
        return parse_anthropic_response(s_parsed_response, text_out, text_out_len,
                                         tool_name_out, tool_name_len,
                                         tool_id_out, tool_id_len, tool_input_out);
    }
}

void json_free_parsed_response(void)
{
    if (s_parsed_response) {
        cJSON_Delete(s_parsed_response);
        s_parsed_response = NULL;
    }
}
