#include "mock_llm.h"
#include "config.h"
#include <string.h>
#include <stdio.h>

#define MOCK_MAX_RESULTS 16
#define MOCK_RESPONSE_MAX_LEN LLM_RESPONSE_BUF_SIZE

typedef struct {
    esp_err_t err;
    char response[MOCK_RESPONSE_MAX_LEN];
    bool has_response;
} llm_result_t;

static llm_backend_t s_backend = LLM_BACKEND_OPENAI;
static char s_model[64] = "mock-model";
static llm_result_t s_results[MOCK_MAX_RESULTS];
static int s_result_count = 0;
static int s_result_index = 0;
static int s_request_count = 0;
static char s_last_request[LLM_REQUEST_BUF_SIZE];

void mock_llm_reset(void)
{
    memset(s_results, 0, sizeof(s_results));
    s_result_count = 0;
    s_result_index = 0;
    s_request_count = 0;
    s_last_request[0] = '\0';
}

void mock_llm_set_backend(llm_backend_t backend, const char *model)
{
    s_backend = backend;
    if (model && model[0] != '\0') {
        strncpy(s_model, model, sizeof(s_model) - 1);
        s_model[sizeof(s_model) - 1] = '\0';
    }
}

bool mock_llm_push_result(esp_err_t err, const char *response_json)
{
    llm_result_t *entry;

    if (s_result_count >= MOCK_MAX_RESULTS) {
        return false;
    }

    entry = &s_results[s_result_count++];
    entry->err = err;
    if (response_json) {
        strncpy(entry->response, response_json, sizeof(entry->response) - 1);
        entry->response[sizeof(entry->response) - 1] = '\0';
        entry->has_response = true;
    }
    return true;
}

int mock_llm_request_count(void)
{
    return s_request_count;
}

const char *mock_llm_last_request_json(void)
{
    return s_last_request;
}

esp_err_t llm_init(void)
{
    return ESP_OK;
}

esp_err_t llm_request(const char *request_json, char *response_buf, size_t response_buf_size)
{
    llm_result_t result = {0};
    const char *default_response =
        "{\"content\":[{\"type\":\"text\",\"text\":\"mock ok\"}],\"stop_reason\":\"end_turn\"}";

    if (request_json) {
        snprintf(s_last_request, sizeof(s_last_request), "%s", request_json);
    } else {
        s_last_request[0] = '\0';
    }
    s_request_count++;

    if (s_result_index < s_result_count) {
        result = s_results[s_result_index++];
    } else {
        result.err = ESP_OK;
        result.has_response = true;
        strncpy(result.response, default_response, sizeof(result.response) - 1);
        result.response[sizeof(result.response) - 1] = '\0';
    }

    if (result.err == ESP_OK && response_buf && response_buf_size > 0) {
        const char *to_copy = result.has_response ? result.response : default_response;
        snprintf(response_buf, response_buf_size, "%s", to_copy);
    }

    return result.err;
}

bool llm_is_stub_mode(void)
{
    return true;
}

llm_backend_t llm_get_backend(void)
{
    return s_backend;
}

const char *llm_get_api_url(void)
{
    return "https://mock.invalid";
}

const char *llm_get_default_model(void)
{
    return "mock-default-model";
}

const char *llm_get_model(void)
{
    return s_model;
}

bool llm_is_openai_format(void)
{
    return s_backend == LLM_BACKEND_OPENAI ||
           s_backend == LLM_BACKEND_OPENROUTER ||
           s_backend == LLM_BACKEND_OLLAMA;
}
