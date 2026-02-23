#include "llm.h"
#include "llm_auth.h"
#include "channel.h"
#include "config.h"
#include "memory.h"
#include "nvs_keys.h"
#include "text_buffer.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

static const char *TAG = "llm";

#if CONFIG_ZCLAW_EMULATOR_LIVE_LLM && CONFIG_ZCLAW_STUB_LLM
#error "ZCLAW_EMULATOR_LIVE_LLM and ZCLAW_STUB_LLM cannot both be enabled"
#endif

// Current backend configuration (loaded from NVS)
static llm_backend_t s_backend = LLM_BACKEND_OPENAI;
static char s_api_key[LLM_API_KEY_BUF_SIZE] = {0};
static char s_model[64] = {0};
static char s_api_url_override[192] = {0};

static bool llm_backend_requires_api_key(llm_backend_t backend)
{
    return backend != LLM_BACKEND_OLLAMA;
}

static const char *llm_backend_name(llm_backend_t backend)
{
    switch (backend) {
        case LLM_BACKEND_ANTHROPIC:
            return "Anthropic";
        case LLM_BACKEND_OPENAI:
            return "OpenAI";
        case LLM_BACKEND_OPENROUTER:
            return "OpenRouter";
        case LLM_BACKEND_OLLAMA:
            return "Ollama";
        default:
            return "Unknown";
    }
}

#if !CONFIG_ZCLAW_STUB_LLM && !CONFIG_ZCLAW_EMULATOR_LIVE_LLM
// Context for HTTP response accumulation (thread-safe via user_data)
typedef struct {
    char *buf;
    size_t len;
    size_t max;
    bool truncated;
    bool saw_connected;
    bool saw_headers_sent;
    bool saw_header;
    bool saw_data;
    bool saw_finish;
    bool saw_disconnected;
    bool saw_error_event;
    int event_errno;
    esp_err_t tls_last_esp_err;
    int tls_stack_err;
    int tls_cert_flags;
    bool tls_error_present;
    uint32_t header_count;
    uint32_t data_event_count;
    uint32_t data_bytes;
    int64_t connected_us;
    int64_t headers_sent_us;
    int64_t first_header_us;
    int64_t first_data_us;
    int64_t finish_us;
    int64_t disconnected_us;
} http_response_ctx_t;

typedef struct {
    uint32_t free_heap;
    uint32_t min_heap;
    uint32_t largest_block;
    int rssi;
    bool rssi_valid;
} net_diag_snapshot_t;

static const char *http_transport_name(esp_http_client_transport_t transport)
{
    switch (transport) {
        case HTTP_TRANSPORT_OVER_TCP:
            return "tcp";
        case HTTP_TRANSPORT_OVER_SSL:
            return "ssl";
        default:
            return "unknown";
    }
}

static uint32_t elapsed_ms_since(int64_t started_us)
{
    int64_t now_us = esp_timer_get_time();
    if (now_us <= started_us) {
        return 0;
    }
    int64_t elapsed_us = now_us - started_us;
    return (uint32_t)(elapsed_us / 1000);
}

static int32_t elapsed_ms_at(int64_t started_us, int64_t event_us)
{
    if (event_us <= 0 || event_us <= started_us) {
        return -1;
    }

    int64_t elapsed_us = event_us - started_us;
    if (elapsed_us > (INT32_MAX * 1000LL)) {
        return INT32_MAX;
    }
    return (int32_t)(elapsed_us / 1000);
}

static void capture_net_diag_snapshot(net_diag_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }

    snapshot->free_heap = (uint32_t)esp_get_free_heap_size();
    snapshot->min_heap = (uint32_t)esp_get_minimum_free_heap_size();
    snapshot->largest_block = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    snapshot->rssi_valid = false;
    snapshot->rssi = 0;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        snapshot->rssi = ap_info.rssi;
        snapshot->rssi_valid = true;
    }
}

static void log_http_diag(const char *operation,
                          esp_http_client_handle_t client,
                          esp_err_t err,
                          int status,
                          size_t response_len,
                          bool truncated,
                          int64_t started_us,
                          const http_response_ctx_t *ctx,
                          const net_diag_snapshot_t *before,
                          const net_diag_snapshot_t *after)
{
    int sock_errno = 0;
    esp_http_client_transport_t transport = HTTP_TRANSPORT_UNKNOWN;
    int heap_delta = 0;
    uint32_t heap_free = 0;
    uint32_t heap_min = 0;
    uint32_t heap_largest = 0;
    int rssi = 0;
    int rssi_ok = 0;
    bool ok = false;
    int event_errno = 0;
    int conn_ms = -1;
    int hdr_sent_ms = -1;
    int hdr_ms = -1;
    int data_ms = -1;
    int finish_ms = -1;
    int disc_ms = -1;
    int tls_last_err = 0;
    int tls_stack_err = 0;
    int tls_cert_flags = 0;
    int tls_diag = 0;
    int ev_connected = 0;
    int ev_hdr_sent = 0;
    int ev_header = 0;
    int ev_data = 0;
    int ev_finish = 0;
    int ev_disc = 0;
    int ev_error = 0;
    uint32_t header_count = 0;
    uint32_t data_event_count = 0;
    uint32_t data_bytes = 0;

    if (ctx) {
        ev_connected = ctx->saw_connected ? 1 : 0;
        ev_hdr_sent = ctx->saw_headers_sent ? 1 : 0;
        ev_header = ctx->saw_header ? 1 : 0;
        ev_data = ctx->saw_data ? 1 : 0;
        ev_finish = ctx->saw_finish ? 1 : 0;
        ev_disc = ctx->saw_disconnected ? 1 : 0;
        ev_error = ctx->saw_error_event ? 1 : 0;
        event_errno = ctx->event_errno;
        conn_ms = elapsed_ms_at(started_us, ctx->connected_us);
        hdr_sent_ms = elapsed_ms_at(started_us, ctx->headers_sent_us);
        hdr_ms = elapsed_ms_at(started_us, ctx->first_header_us);
        data_ms = elapsed_ms_at(started_us, ctx->first_data_us);
        finish_ms = elapsed_ms_at(started_us, ctx->finish_us);
        disc_ms = elapsed_ms_at(started_us, ctx->disconnected_us);
        header_count = ctx->header_count;
        data_event_count = ctx->data_event_count;
        data_bytes = ctx->data_bytes;
        if (ctx->tls_error_present) {
            tls_diag = 1;
            tls_last_err = ctx->tls_last_esp_err;
            tls_stack_err = ctx->tls_stack_err;
            tls_cert_flags = ctx->tls_cert_flags;
        }
    }

    if (client) {
        if (status < 0) {
            status = esp_http_client_get_status_code(client);
        }
        sock_errno = esp_http_client_get_errno(client);
        transport = esp_http_client_get_transport_type(client);
    }

    if (after) {
        heap_free = after->free_heap;
        heap_min = after->min_heap;
        heap_largest = after->largest_block;
        if (after->rssi_valid) {
            rssi_ok = 1;
            rssi = after->rssi;
        }
    }
    if (before && after) {
        heap_delta = (int)after->free_heap - (int)before->free_heap;
    }

    ok = (err == ESP_OK && status == 200 && !truncated);
    if (ok) {
        ESP_LOGI(TAG,
                 "NETDIAG op=%s ok=1 status=%d err=%s(%d) errno=%d(%s) transport=%s "
                 "dur_ms=%lu resp_bytes=%u truncated=%d ev_conn=%d ev_hsent=%d ev_hdr=%d "
                 "ev_data=%d ev_fin=%d ev_disc=%d ev_err=%d ev_errno=%d(%s) "
                 "hdr_count=%u data_ev=%u data_bytes=%u t_conn_ms=%d t_hsent_ms=%d "
                 "t_hdr_ms=%d t_data_ms=%d t_fin_ms=%d t_disc_ms=%d tls_diag=%d "
                 "tls_last_err=%d tls_stack_err=%d tls_cert_flags=%d "
                 "heap_free=%lu heap_delta=%d heap_min=%lu heap_largest=%lu rssi=%d rssi_ok=%d",
                 operation ? operation : "llm_request",
                 status,
                 esp_err_to_name(err), err,
                 sock_errno,
                 sock_errno ? strerror(sock_errno) : "n/a",
                 http_transport_name(transport),
                 (unsigned long)elapsed_ms_since(started_us),
                 (unsigned)response_len,
                 truncated ? 1 : 0,
                 ev_connected,
                 ev_hdr_sent,
                 ev_header,
                 ev_data,
                 ev_finish,
                 ev_disc,
                 ev_error,
                 event_errno,
                 event_errno ? strerror(event_errno) : "n/a",
                 (unsigned)header_count,
                 (unsigned)data_event_count,
                 (unsigned)data_bytes,
                 conn_ms,
                 hdr_sent_ms,
                 hdr_ms,
                 data_ms,
                 finish_ms,
                 disc_ms,
                 tls_diag,
                 tls_last_err,
                 tls_stack_err,
                 tls_cert_flags,
                 (unsigned long)heap_free,
                 heap_delta,
                 (unsigned long)heap_min,
                 (unsigned long)heap_largest,
                 rssi,
                 rssi_ok);
    } else {
        ESP_LOGW(TAG,
                 "NETDIAG op=%s ok=0 status=%d err=%s(%d) errno=%d(%s) transport=%s "
                 "dur_ms=%lu resp_bytes=%u truncated=%d ev_conn=%d ev_hsent=%d ev_hdr=%d "
                 "ev_data=%d ev_fin=%d ev_disc=%d ev_err=%d ev_errno=%d(%s) "
                 "hdr_count=%u data_ev=%u data_bytes=%u t_conn_ms=%d t_hsent_ms=%d "
                 "t_hdr_ms=%d t_data_ms=%d t_fin_ms=%d t_disc_ms=%d tls_diag=%d "
                 "tls_last_err=%d tls_stack_err=%d tls_cert_flags=%d "
                 "heap_free=%lu heap_delta=%d heap_min=%lu heap_largest=%lu rssi=%d rssi_ok=%d",
                 operation ? operation : "llm_request",
                 status,
                 esp_err_to_name(err), err,
                 sock_errno,
                 sock_errno ? strerror(sock_errno) : "n/a",
                 http_transport_name(transport),
                 (unsigned long)elapsed_ms_since(started_us),
                 (unsigned)response_len,
                 truncated ? 1 : 0,
                 ev_connected,
                 ev_hdr_sent,
                 ev_header,
                 ev_data,
                 ev_finish,
                 ev_disc,
                 ev_error,
                 event_errno,
                 event_errno ? strerror(event_errno) : "n/a",
                 (unsigned)header_count,
                 (unsigned)data_event_count,
                 (unsigned)data_bytes,
                 conn_ms,
                 hdr_sent_ms,
                 hdr_ms,
                 data_ms,
                 finish_ms,
                 disc_ms,
                 tls_diag,
                 tls_last_err,
                 tls_stack_err,
                 tls_cert_flags,
                 (unsigned long)heap_free,
                 heap_delta,
                 (unsigned long)heap_min,
                 (unsigned long)heap_largest,
                 rssi,
                 rssi_ok);
    }
}

static void log_http_failure(const char *operation, esp_http_client_handle_t client, esp_err_t err)
{
    int status = -1;
    int sock_errno = 0;
    esp_http_client_transport_t transport = HTTP_TRANSPORT_UNKNOWN;

    if (client) {
        status = esp_http_client_get_status_code(client);
        sock_errno = esp_http_client_get_errno(client);
        transport = esp_http_client_get_transport_type(client);
    }

    ESP_LOGE(TAG,
             "%s failed: err=%s(%d) status=%d errno=%d(%s) transport=%s",
             operation ? operation : "HTTP request",
             esp_err_to_name(err), err,
             status,
             sock_errno,
             sock_errno ? strerror(sock_errno) : "n/a",
             http_transport_name(transport));
}

// HTTP event handler
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_ctx_t *ctx = (http_response_ctx_t *)evt->user_data;
    int64_t now_us = esp_timer_get_time();

    switch (evt->event_id) {
        case HTTP_EVENT_ON_CONNECTED:
            if (ctx) {
                ctx->saw_connected = true;
                ctx->connected_us = now_us;
            }
            break;
        case HTTP_EVENT_HEADERS_SENT:
            if (ctx) {
                ctx->saw_headers_sent = true;
                ctx->headers_sent_us = now_us;
            }
            break;
        case HTTP_EVENT_ON_HEADER:
            if (ctx) {
                ctx->saw_header = true;
                ctx->header_count++;
                if (ctx->first_header_us == 0) {
                    ctx->first_header_us = now_us;
                }
            }
            break;
        case HTTP_EVENT_ON_DATA:
            if (ctx && ctx->buf) {
                ctx->saw_data = true;
                ctx->data_event_count++;
                if (evt->data_len > 0) {
                    ctx->data_bytes += (uint32_t)evt->data_len;
                }
                if (ctx->first_data_us == 0) {
                    ctx->first_data_us = now_us;
                }
                bool ok = text_buffer_append(ctx->buf, &ctx->len, ctx->max,
                                             (const char *)evt->data, evt->data_len);
                if (!ok && !ctx->truncated) {
                    ctx->truncated = true;
                    ESP_LOGW(TAG, "LLM response truncated at %d bytes", (int)(ctx->max - 1));
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            if (ctx) {
                ctx->saw_finish = true;
                ctx->finish_us = now_us;
            }
            break;
        case HTTP_EVENT_ERROR:
            if (ctx) {
                ctx->saw_error_event = true;
            }
            break;
        case HTTP_EVENT_DISCONNECTED:
            if (ctx) {
                ctx->saw_disconnected = true;
                ctx->disconnected_us = now_us;
            }
            if (evt && evt->client) {
                int sock_errno = esp_http_client_get_errno(evt->client);
                if (ctx) {
                    ctx->event_errno = sock_errno;
                }
                if (sock_errno != 0) {
                    ESP_LOGD(TAG, "HTTP event=%d errno=%d(%s)",
                             evt->event_id, sock_errno, strerror(sock_errno));
                }
            }
            if (ctx && evt && evt->data) {
                int tls_stack_err = 0;
                int tls_cert_flags = 0;
                esp_err_t tls_err = esp_tls_get_and_clear_last_error(
                    (esp_tls_error_handle_t)evt->data, &tls_stack_err, &tls_cert_flags);
                if (tls_err != ESP_OK || tls_stack_err != 0 || tls_cert_flags != 0) {
                    ctx->tls_error_present = true;
                    ctx->tls_last_esp_err = tls_err;
                    ctx->tls_stack_err = tls_stack_err;
                    ctx->tls_cert_flags = tls_cert_flags;
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
        if (strcmp(backend_str, "anthropic") == 0) {
            s_backend = LLM_BACKEND_ANTHROPIC;
        } else if (strcmp(backend_str, "openai") == 0) {
            s_backend = LLM_BACKEND_OPENAI;
        } else if (strcmp(backend_str, "openrouter") == 0) {
            s_backend = LLM_BACKEND_OPENROUTER;
        } else if (strcmp(backend_str, "ollama") == 0) {
            s_backend = LLM_BACKEND_OLLAMA;
        } else {
            ESP_LOGW(TAG, "Unknown llm_backend '%s', defaulting to OpenAI", backend_str);
            s_backend = LLM_BACKEND_OPENAI;
        }
    }

    // Reset key state first so re-init never keeps stale credentials in RAM.
    memset(s_api_key, 0, sizeof(s_api_key));

    // Load API key from NVS
    if (!memory_get(NVS_KEY_API_KEY, s_api_key, sizeof(s_api_key))) {
#if defined(CONFIG_ZCLAW_CLAUDE_API_KEY)
        if (s_backend == LLM_BACKEND_ANTHROPIC && CONFIG_ZCLAW_CLAUDE_API_KEY[0] != '\0') {
            if (llm_copy_api_key(s_api_key, sizeof(s_api_key), CONFIG_ZCLAW_CLAUDE_API_KEY)) {
                ESP_LOGI(TAG, "Using compile-time Anthropic API key fallback");
            } else {
                ESP_LOGE(TAG, "Compile-time API key exceeds maximum supported length (%d)",
                         LLM_API_KEY_MAX_LEN);
            }
        } else
#endif
        {
            if (llm_backend_requires_api_key(s_backend)) {
                ESP_LOGW(TAG, "No API key configured (or key exceeds %d bytes)", LLM_API_KEY_MAX_LEN);
            }
        }
    }

    // Load model (optional override)
    if (!memory_get(NVS_KEY_LLM_MODEL, s_model, sizeof(s_model))) {
        // Use default for backend
        strncpy(s_model, llm_get_default_model(), sizeof(s_model) - 1);
        s_model[sizeof(s_model) - 1] = '\0';
    }

    s_api_url_override[0] = '\0';
    memory_get(NVS_KEY_LLM_API_URL, s_api_url_override, sizeof(s_api_url_override));

    ESP_LOGI(TAG, "Backend: %s, Model: %s", llm_backend_name(s_backend), s_model);
    if (s_api_url_override[0] != '\0') {
        ESP_LOGI(TAG, "Using custom LLM API endpoint override");
    } else if (s_backend == LLM_BACKEND_OLLAMA) {
        ESP_LOGW(TAG, "Ollama backend using default loopback URL; set llm_api_url for network access");
    }

#ifdef CONFIG_ZCLAW_STUB_LLM
    ESP_LOGW(TAG, "LLM stub mode enabled (QEMU testing)");
#endif
#if CONFIG_ZCLAW_EMULATOR_LIVE_LLM
    ESP_LOGW(TAG, "LLM emulator bridge mode enabled (host-side API bridge required)");
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
    if (s_api_url_override[0] != '\0') {
        return s_api_url_override;
    }

    switch (s_backend) {
        case LLM_BACKEND_OPENAI:
            return LLM_API_URL_OPENAI;
        case LLM_BACKEND_OPENROUTER:
            return LLM_API_URL_OPENROUTER;
        case LLM_BACKEND_OLLAMA:
            return LLM_API_URL_OLLAMA;
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
        case LLM_BACKEND_OLLAMA:
            return LLM_DEFAULT_MODEL_OLLAMA;
        default:
            return LLM_DEFAULT_MODEL_ANTHROPIC;
    }
}

const char *llm_get_model(void)
{
    return s_model;
}

#if CONFIG_ZCLAW_STUB_LLM
bool llm_stub_has_api_key_for_test(void)
{
    return s_api_key[0] != '\0';
}
#endif

bool llm_is_openai_format(void)
{
    return s_backend == LLM_BACKEND_OPENAI ||
           s_backend == LLM_BACKEND_OPENROUTER ||
           s_backend == LLM_BACKEND_OLLAMA;
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
#if CONFIG_ZCLAW_EMULATOR_LIVE_LLM
    // In emulator bridge mode, delegate HTTPS API calls to a host-side proxy.
    esp_err_t bridge_err = channel_llm_bridge_exchange(request_json, response_buf, response_buf_size,
                                                       LLM_HTTP_TIMEOUT_MS + 30000);
    if (bridge_err != ESP_OK) {
        ESP_LOGE(TAG, "Host bridge request failed: %s", esp_err_to_name(bridge_err));
        return bridge_err;
    }
    ESP_LOGI(TAG, "Host bridge response: %d bytes", (int)strlen(response_buf));
    return ESP_OK;
#elif defined(CONFIG_ZCLAW_STUB_LLM)
    const char *stub = get_stub_response(request_json);
    strncpy(response_buf, stub, response_buf_size - 1);
    response_buf[response_buf_size - 1] = '\0';
    ESP_LOGI(TAG, "Stub response: %d bytes", (int)strlen(response_buf));
    return ESP_OK;
#else
    int64_t started_us = esp_timer_get_time();
    net_diag_snapshot_t snapshot_before = {0};
    net_diag_snapshot_t snapshot_after = {0};
    int status = -1;

    capture_net_diag_snapshot(&snapshot_before);

    if (s_api_key[0] == '\0' && llm_backend_requires_api_key(s_backend)) {
        ESP_LOGE(TAG, "No API key configured");
        capture_net_diag_snapshot(&snapshot_after);
        log_http_diag("llm_request", NULL, ESP_ERR_INVALID_STATE, -1, 0, false,
                      started_us, NULL, &snapshot_before, &snapshot_after);
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
        .timeout_ms = LLM_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        capture_net_diag_snapshot(&snapshot_after);
        log_http_diag("llm_request", NULL, ESP_FAIL, -1, 0, false,
                      started_us, NULL, &snapshot_before, &snapshot_after);
        return ESP_FAIL;
    }

    // Set common headers
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    // Set backend-specific headers
    if (s_backend == LLM_BACKEND_ANTHROPIC) {
        esp_http_client_set_header(client, "x-api-key", s_api_key);
        esp_http_client_set_header(client, "anthropic-version", "2023-06-01");
    } else if (s_backend == LLM_BACKEND_OPENAI || s_backend == LLM_BACKEND_OPENROUTER ||
               (s_backend == LLM_BACKEND_OLLAMA && s_api_key[0] != '\0')) {
        // OpenAI/OpenRouter use Bearer token. For Ollama, Bearer is optional and only sent
        // when a key is explicitly provided (e.g. reverse proxy auth).
        char auth_header[LLM_AUTH_HEADER_BUF_SIZE];
        if (!llm_build_bearer_auth_header(s_api_key, auth_header, sizeof(auth_header))) {
            ESP_LOGE(TAG, "API key length exceeds supported authorization header capacity");
            esp_http_client_cleanup(client);
            return ESP_ERR_INVALID_SIZE;
        }
        esp_http_client_set_header(client, "Authorization", auth_header);

        // OpenRouter needs additional headers
        if (s_backend == LLM_BACKEND_OPENROUTER) {
            esp_http_client_set_header(client, "HTTP-Referer", "https://github.com/tnm/zclaw");
            esp_http_client_set_header(client, "X-Title", "zclaw");
        }
    }

    // Set body
    esp_http_client_set_post_field(client, request_json, strlen(request_json));

    ESP_LOGI(TAG, "Sending request to %s...", llm_backend_name(s_backend));

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Response: %d, %d bytes", status, (int)ctx.len);

        if (status != 200) {
            ESP_LOGE(TAG, "API error: %s", response_buf);
            log_http_failure("LLM request", client, ESP_FAIL);
            err = ESP_FAIL;
        } else if (ctx.truncated) {
            ESP_LOGE(TAG, "LLM response truncated");
            err = ESP_ERR_NO_MEM;
        }
    } else {
        log_http_failure("LLM request", client, err);
    }

    capture_net_diag_snapshot(&snapshot_after);
    log_http_diag("llm_request", client, err, status, ctx.len, ctx.truncated,
                  started_us, &ctx, &snapshot_before, &snapshot_after);

    esp_http_client_cleanup(client);

    return err;
#endif
}
