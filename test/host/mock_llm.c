#include "mock_llm.h"
#include <string.h>

static llm_backend_t s_backend = LLM_BACKEND_ANTHROPIC;
static char s_model[64] = "mock-model";

void mock_llm_set_backend(llm_backend_t backend, const char *model)
{
    s_backend = backend;
    if (model && model[0] != '\0') {
        strncpy(s_model, model, sizeof(s_model) - 1);
        s_model[sizeof(s_model) - 1] = '\0';
    }
}

esp_err_t llm_init(void)
{
    return ESP_OK;
}

esp_err_t llm_request(const char *request_json, char *response_buf, size_t response_buf_size)
{
    (void)request_json;
    (void)response_buf;
    (void)response_buf_size;
    return ESP_OK;
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
    return s_backend == LLM_BACKEND_OPENAI || s_backend == LLM_BACKEND_OPENROUTER;
}
