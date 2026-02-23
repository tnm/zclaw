/*
 * Host tests for real llm.c runtime behavior in stub mode.
 */

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "llm.h"
#include "mock_memory.h"
#include "nvs_keys.h"

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while(0)

static void configure_mock_store(const char *backend,
                                 const char *model,
                                 const char *api_key,
                                 const char *api_url)
{
    mock_memory_reset();

    if (backend && backend[0] != '\0') {
        mock_memory_set_kv(NVS_KEY_LLM_BACKEND, backend);
    }
    if (model && model[0] != '\0') {
        mock_memory_set_kv(NVS_KEY_LLM_MODEL, model);
    }
    if (api_key && api_key[0] != '\0') {
        mock_memory_set_kv(NVS_KEY_API_KEY, api_key);
    }
    if (api_url && api_url[0] != '\0') {
        mock_memory_set_kv(NVS_KEY_LLM_API_URL, api_url);
    }
}

TEST(defaults_to_openai_on_first_init)
{
    configure_mock_store(NULL, NULL, "test-key", NULL);
    ASSERT(llm_init() == ESP_OK);
    ASSERT(llm_get_backend() == LLM_BACKEND_OPENAI);
    ASSERT(strcmp(llm_get_api_url(), LLM_API_URL_OPENAI) == 0);
    ASSERT(strcmp(llm_get_model(), LLM_DEFAULT_MODEL_OPENAI) == 0);
    ASSERT(llm_is_openai_format());
    return 0;
}

TEST(loads_anthropic_backend_and_default_model)
{
    configure_mock_store("anthropic", NULL, "test-key", NULL);
    ASSERT(llm_init() == ESP_OK);
    ASSERT(llm_get_backend() == LLM_BACKEND_ANTHROPIC);
    ASSERT(strcmp(llm_get_api_url(), LLM_API_URL_ANTHROPIC) == 0);
    ASSERT(strcmp(llm_get_model(), LLM_DEFAULT_MODEL_ANTHROPIC) == 0);
    ASSERT(!llm_is_openai_format());
    return 0;
}

TEST(loads_openrouter_backend_and_custom_model)
{
    configure_mock_store("openrouter", "custom/router-model", "test-key", NULL);
    ASSERT(llm_init() == ESP_OK);
    ASSERT(llm_get_backend() == LLM_BACKEND_OPENROUTER);
    ASSERT(strcmp(llm_get_api_url(), LLM_API_URL_OPENROUTER) == 0);
    ASSERT(strcmp(llm_get_model(), "custom/router-model") == 0);
    ASSERT(llm_is_openai_format());
    return 0;
}

TEST(unknown_backend_falls_back_to_openai)
{
    configure_mock_store("mystery_backend", NULL, "test-key", NULL);
    ASSERT(llm_init() == ESP_OK);
    ASSERT(llm_get_backend() == LLM_BACKEND_OPENAI);
    ASSERT(strcmp(llm_get_api_url(), LLM_API_URL_OPENAI) == 0);
    ASSERT(strcmp(llm_get_model(), LLM_DEFAULT_MODEL_OPENAI) == 0);
    ASSERT(llm_is_openai_format());
    return 0;
}

TEST(stub_request_returns_response)
{
    char response[LLM_RESPONSE_BUF_SIZE];
    configure_mock_store("openai", "gpt-5.2", "test-key", NULL);
    ASSERT(llm_init() == ESP_OK);
    ASSERT(llm_request("{\"message\":\"toggle gpio\"}", response, sizeof(response)) == ESP_OK);
    ASSERT(response[0] != '\0');
    ASSERT(strstr(response, "tool_use") != NULL);
    return 0;
}

TEST(loads_ollama_backend_with_default_model)
{
    configure_mock_store("ollama", NULL, NULL, NULL);
    ASSERT(llm_init() == ESP_OK);
    ASSERT(llm_get_backend() == LLM_BACKEND_OLLAMA);
    ASSERT(strcmp(llm_get_api_url(), LLM_API_URL_OLLAMA) == 0);
    ASSERT(strcmp(llm_get_model(), LLM_DEFAULT_MODEL_OLLAMA) == 0);
    ASSERT(llm_is_openai_format());
    return 0;
}

TEST(custom_api_url_override_applies_to_any_backend)
{
    configure_mock_store("openai", NULL, "test-key", "http://192.168.1.50:11434/v1/chat/completions");
    ASSERT(llm_init() == ESP_OK);
    ASSERT(strcmp(llm_get_api_url(), "http://192.168.1.50:11434/v1/chat/completions") == 0);
    return 0;
}

TEST(reinit_without_key_clears_previous_api_key_state)
{
    configure_mock_store("openai", NULL, "test-key", NULL);
    ASSERT(llm_init() == ESP_OK);
    ASSERT(llm_stub_has_api_key_for_test());

    configure_mock_store("ollama", NULL, NULL, NULL);
    ASSERT(llm_init() == ESP_OK);
    ASSERT(!llm_stub_has_api_key_for_test());
    return 0;
}

int test_llm_runtime_all(void)
{
    int failures = 0;

    printf("\nLLM Runtime Tests:\n");

    printf("  defaults_to_openai_on_first_init... ");
    if (test_defaults_to_openai_on_first_init() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  loads_anthropic_backend_and_default_model... ");
    if (test_loads_anthropic_backend_and_default_model() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  loads_openrouter_backend_and_custom_model... ");
    if (test_loads_openrouter_backend_and_custom_model() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  unknown_backend_falls_back_to_openai... ");
    if (test_unknown_backend_falls_back_to_openai() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  stub_request_returns_response... ");
    if (test_stub_request_returns_response() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  loads_ollama_backend_with_default_model... ");
    if (test_loads_ollama_backend_with_default_model() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  custom_api_url_override_applies_to_any_backend... ");
    if (test_custom_api_url_override_applies_to_any_backend() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  reinit_without_key_clears_previous_api_key_state... ");
    if (test_reinit_without_key_clears_previous_api_key_state() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
