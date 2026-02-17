#include "llm.h"
#include "config.h"
#include "memory.h"
#include "nvs_keys.h"
#include "text_buffer.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "llm";

// Current backend configuration (loaded from NVS)
static llm_backend_t s_backend = LLM_BACKEND_ANTHROPIC;
static char s_api_key[128] = {0};
static char s_model[64] = {0};

#ifndef CONFIG_ZCLAW_STUB_LLM
// Context for HTTP response accumulation (thread-safe via user_data)
typedef struct {
    char *buf;
    size_t len;
    size_t max;
    bool truncated;
} http_response_ctx_t;

// HTTP event handler
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_ctx_t *ctx = (http_response_ctx_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (ctx && ctx->buf) {
                bool ok = text_buffer_append(ctx->buf, &ctx->len, ctx->max,
                                             (const char *)evt->data, evt->data_len);
                if (!ok && !ctx->truncated) {
                    ctx->truncated = true;
                    ESP_LOGW(TAG, "LLM response truncated at %d bytes", (int)(ctx->max - 1));
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}
#endif

esp_err_t llm_init(void)
{
    // Load backend type from NVS
    char backend_str[16] = {0};
    if (memory_get(NVS_KEY_LLM_BACKEND, backend_str, sizeof(backend_str))) {
        if (strcmp(backend_str, "openai") == 0) {
            s_backend = LLM_BACKEND_OPENAI;
        } else if (strcmp(backend_str, "openrouter") == 0) {
            s_backend = LLM_BACKEND_OPENROUTER;
        } else {
            s_backend = LLM_BACKEND_ANTHROPIC;
        }
    }

    // Load API key from NVS
    if (!memory_get(NVS_KEY_API_KEY, s_api_key, sizeof(s_api_key))) {
#if defined(CONFIG_ZCLAW_CLAUDE_API_KEY)
        if (s_backend == LLM_BACKEND_ANTHROPIC && CONFIG_ZCLAW_CLAUDE_API_KEY[0] != '\0') {
            strncpy(s_api_key, CONFIG_ZCLAW_CLAUDE_API_KEY, sizeof(s_api_key) - 1);
            s_api_key[sizeof(s_api_key) - 1] = '\0';
            ESP_LOGI(TAG, "Using compile-time Anthropic API key fallback");
        } else
#endif
        {
            ESP_LOGW(TAG, "No API key configured");
        }
    }

    // Load model (optional override)
    if (!memory_get(NVS_KEY_LLM_MODEL, s_model, sizeof(s_model))) {
        // Use default for backend
        strncpy(s_model, llm_get_default_model(), sizeof(s_model) - 1);
    }

    const char *backend_names[] = {"Anthropic", "OpenAI", "OpenRouter"};
    ESP_LOGI(TAG, "Backend: %s, Model: %s", backend_names[s_backend], s_model);

#ifdef CONFIG_ZCLAW_STUB_LLM
    ESP_LOGW(TAG, "LLM stub mode enabled (QEMU testing)");
#endif

    return ESP_OK;
}

bool llm_is_stub_mode(void)
{
#ifdef CONFIG_ZCLAW_STUB_LLM
    return true;
#else
    return false;
#endif
}

llm_backend_t llm_get_backend(void)
{
    return s_backend;
}

const char *llm_get_api_url(void)
{
    switch (s_backend) {
        case LLM_BACKEND_OPENAI:
            return LLM_API_URL_OPENAI;
        case LLM_BACKEND_OPENROUTER:
            return LLM_API_URL_OPENROUTER;
        default:
            return LLM_API_URL_ANTHROPIC;
    }
}

const char *llm_get_default_model(void)
{
    switch (s_backend) {
        case LLM_BACKEND_OPENAI:
            return LLM_DEFAULT_MODEL_OPENAI;
        case LLM_BACKEND_OPENROUTER:
            return LLM_DEFAULT_MODEL_OPENROUTER;
        default:
            return LLM_DEFAULT_MODEL_ANTHROPIC;
    }
}

const char *llm_get_model(void)
{
    return s_model;
}

bool llm_is_openai_format(void)
{
    return s_backend == LLM_BACKEND_OPENAI || s_backend == LLM_BACKEND_OPENROUTER;
}

#ifdef CONFIG_ZCLAW_STUB_LLM
// Stub response for QEMU testing
static const char *get_stub_response(const char *request_json)
{
    // Check if this is a tool_result (second turn after tool use)
    if (strstr(request_json, "tool_result")) {
        return "{"
            "\"content\": [{\"type\": \"text\", \"text\": \"Done! I executed the tool successfully.\"}],"
            "\"stop_reason\": \"end_turn\""
        "}";
    }

    // Check if the request mentions GPIO
    if (strstr(request_json, "pin") || strstr(request_json, "gpio") || strstr(request_json, "GPIO")) {
        return "{"
            "\"content\": ["
                "{\"type\": \"tool_use\", \"id\": \"toolu_stub_001\", \"name\": \"gpio_write\", "
                "\"input\": {\"pin\": 10, \"state\": 1}}"
            "], \"stop_reason\": \"tool_use\""
        "}";
    }

    // Check if request mentions memory/remember
    if (strstr(request_json, "remember") || strstr(request_json, "memory") || strstr(request_json, "store")) {
        return "{"
            "\"content\": ["
                "{\"type\": \"tool_use\", \"id\": \"toolu_stub_002\", \"name\": \"memory_set\", "
                "\"input\": {\"key\": \"test_key\", \"value\": \"test_value\"}}"
            "], \"stop_reason\": \"tool_use\""
        "}";
    }

    // Default response
    return "{"
        "\"content\": [{\"type\": \"text\", \"text\": \"Hello from zclaw! "
        "I'm running on a tiny ESP32. Try asking me to set a pin high or remember something.\"}],"
        "\"stop_reason\": \"end_turn\""
    "}";
}
#endif

esp_err_t llm_request(const char *request_json, char *response_buf, size_t response_buf_size)
{
#ifdef CONFIG_ZCLAW_STUB_LLM
    const char *stub = get_stub_response(request_json);
    strncpy(response_buf, stub, response_buf_size - 1);
    response_buf[response_buf_size - 1] = '\0';
    ESP_LOGI(TAG, "Stub response: %d bytes", (int)strlen(response_buf));
    return ESP_OK;
#else
    if (s_api_key[0] == '\0') {
        ESP_LOGE(TAG, "No API key configured");
        return ESP_ERR_INVALID_STATE;
    }

    // Thread-safe response context
    http_response_ctx_t ctx = {
        .buf = response_buf,
        .len = 0,
        .max = response_buf_size,
        .truncated = false
    };
    response_buf[0] = '\0';

    esp_http_client_config_t config = {
        .url = llm_get_api_url(),
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    // Set common headers
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    // Set backend-specific headers
    if (s_backend == LLM_BACKEND_ANTHROPIC) {
        esp_http_client_set_header(client, "x-api-key", s_api_key);
        esp_http_client_set_header(client, "anthropic-version", "2023-06-01");
    } else {
        // OpenAI and OpenRouter use Bearer token
        char auth_header[150];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_api_key);
        esp_http_client_set_header(client, "Authorization", auth_header);

        // OpenRouter needs additional headers
        if (s_backend == LLM_BACKEND_OPENROUTER) {
            esp_http_client_set_header(client, "HTTP-Referer", "https://github.com/tnm/zclaw");
            esp_http_client_set_header(client, "X-Title", "zclaw");
        }
    }

    // Set body
    esp_http_client_set_post_field(client, request_json, strlen(request_json));

    const char *backend_names[] = {"Anthropic", "OpenAI", "OpenRouter"};
    ESP_LOGI(TAG, "Sending request to %s...", backend_names[s_backend]);

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Response: %d, %d bytes", status, (int)ctx.len);

        if (status != 200) {
            ESP_LOGE(TAG, "API error: %s", response_buf);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);

    return err;
#endif
}
