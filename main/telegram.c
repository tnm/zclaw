#include "telegram.h"
#include "config.h"
#include "messages.h"
#include "memory.h"
#include "nvs_keys.h"
#include "telegram_token.h"
#include "telegram_update.h"
#include "text_buffer.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>
#include <limits.h>

static const char *TAG = "telegram";

static QueueHandle_t s_input_queue;
static QueueHandle_t s_output_queue;
static char s_bot_token[64] = {0};
static int64_t s_chat_id = 0;
static int64_t s_last_update_id = 0;
static telegram_msg_t s_send_msg;
static uint32_t s_stale_only_poll_streak = 0;
static uint32_t s_poll_sequence = 0;
static int64_t s_last_stale_resync_us = 0;

// Exponential backoff state
static int s_consecutive_failures = 0;
#define BACKOFF_BASE_MS     5000    // 5 seconds
#define BACKOFF_MAX_MS      300000  // 5 minutes
#define BACKOFF_MULTIPLIER  2
#define TELEGRAM_ACTION_TIMEOUT_MS 5000
#define TELEGRAM_TYPING_DEBOUNCE_MS 4000
#define TELEGRAM_POLL_TASK_STACK_SIZE 8192

static int64_t s_last_typing_sent_us = 0;

typedef struct {
    char buf[4096];
    size_t len;
    bool truncated;
} telegram_http_ctx_t;

typedef struct {
    uint32_t free_heap;
    uint32_t min_heap;
    uint32_t largest_block;
    int rssi;
    bool rssi_valid;
} net_diag_snapshot_t;

static esp_err_t telegram_flush_pending_updates(void);

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

static void capture_net_diag_snapshot(net_diag_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }

    snapshot->free_heap = (uint32_t)esp_get_free_heap_size();
    snapshot->min_heap = (uint32_t)esp_get_minimum_free_heap_size();
    snapshot->largest_block = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    snapshot->rssi = 0;
    snapshot->rssi_valid = false;

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
                          int64_t started_us,
                          size_t response_len,
                          int result_count,
                          int stale_count,
                          int accepted_count,
                          uint32_t poll_seq,
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
            rssi = after->rssi;
            rssi_ok = 1;
        }
    }
    if (before && after) {
        heap_delta = (int)after->free_heap - (int)before->free_heap;
    }

    ok = (err == ESP_OK && status == 200);
    if (ok) {
        ESP_LOGI(TAG,
                 "NETDIAG op=%s ok=1 status=%d err=%s(%d) errno=%d(%s) transport=%s "
                 "dur_ms=%lu poll_seq=%u res=%d stale=%d new=%d body_bytes=%u "
                 "heap_free=%lu heap_delta=%d heap_min=%lu heap_largest=%lu "
                 "rssi=%d rssi_ok=%d",
                 operation ? operation : "telegram_http",
                 status,
                 esp_err_to_name(err), err,
                 sock_errno,
                 sock_errno ? strerror(sock_errno) : "n/a",
                 http_transport_name(transport),
                 (unsigned long)elapsed_ms_since(started_us),
                 (unsigned)poll_seq,
                 result_count,
                 stale_count,
                 accepted_count,
                 (unsigned)response_len,
                 (unsigned long)heap_free,
                 heap_delta,
                 (unsigned long)heap_min,
                 (unsigned long)heap_largest,
                 rssi,
                 rssi_ok);
    } else {
        ESP_LOGW(TAG,
                 "NETDIAG op=%s ok=0 status=%d err=%s(%d) errno=%d(%s) transport=%s "
                 "dur_ms=%lu poll_seq=%u res=%d stale=%d new=%d body_bytes=%u "
                 "heap_free=%lu heap_delta=%d heap_min=%lu heap_largest=%lu "
                 "rssi=%d rssi_ok=%d",
                 operation ? operation : "telegram_http",
                 status,
                 esp_err_to_name(err), err,
                 sock_errno,
                 sock_errno ? strerror(sock_errno) : "n/a",
                 http_transport_name(transport),
                 (unsigned long)elapsed_ms_since(started_us),
                 (unsigned)poll_seq,
                 result_count,
                 stale_count,
                 accepted_count,
                 (unsigned)response_len,
                 (unsigned long)heap_free,
                 heap_delta,
                 (unsigned long)heap_min,
                 (unsigned long)heap_largest,
                 rssi,
                 rssi_ok);
    }
}

static void log_http_failure(const char *operation,
                             esp_http_client_handle_t client,
                             esp_err_t err,
                             int status)
{
    int sock_errno = 0;
    esp_http_client_transport_t transport = HTTP_TRANSPORT_UNKNOWN;

    if (client) {
        sock_errno = esp_http_client_get_errno(client);
        if (status < 0) {
            status = esp_http_client_get_status_code(client);
        }
        transport = esp_http_client_get_transport_type(client);
    }

    ESP_LOGW(TAG,
             "%s failed: err=%s(%d) status=%d errno=%d(%s) transport=%s",
             operation ? operation : "HTTP request",
             esp_err_to_name(err), err,
             status,
             sock_errno,
             sock_errno ? strerror(sock_errno) : "n/a",
             http_transport_name(transport));
}

static bool format_int64_decimal(int64_t value, char *out, size_t out_len)
{
    char reversed[24];
    size_t reversed_len = 0;
    uint64_t magnitude;
    size_t pos = 0;

    if (!out || out_len == 0) {
        return false;
    }

    if (value < 0) {
        out[pos++] = '-';
        magnitude = (uint64_t)(-(value + 1)) + 1ULL;
    } else {
        magnitude = (uint64_t)value;
    }

    do {
        if (reversed_len >= sizeof(reversed)) {
            out[0] = '\0';
            return false;
        }
        reversed[reversed_len++] = (char)('0' + (magnitude % 10ULL));
        magnitude /= 10ULL;
    } while (magnitude > 0);

    if (pos + reversed_len + 1 > out_len) {
        out[0] = '\0';
        return false;
    }

    while (reversed_len > 0) {
        out[pos++] = reversed[--reversed_len];
    }
    out[pos] = '\0';
    return true;
}

static bool parse_chat_id_string(const char *input, int64_t *chat_id_out)
{
    const unsigned char *cursor = (const unsigned char *)input;
    char *endptr = NULL;
    long long parsed;

    if (!input || !chat_id_out) {
        return false;
    }

    while (*cursor != '\0' && isspace(*cursor)) {
        cursor++;
    }
    if (*cursor == '\0') {
        return false;
    }

    parsed = strtoll((const char *)cursor, &endptr, 10);
    if (!endptr || endptr == (const char *)cursor) {
        return false;
    }

    while (*endptr != '\0' && isspace((unsigned char)*endptr)) {
        endptr++;
    }

    if (*endptr != '\0' || parsed == 0) {
        return false;
    }

    *chat_id_out = (int64_t)parsed;
    return true;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    telegram_http_ctx_t *ctx = (telegram_http_ctx_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (ctx) {
                bool ok = text_buffer_append(ctx->buf, &ctx->len, sizeof(ctx->buf),
                                             (const char *)evt->data, evt->data_len);
                if (!ok && !ctx->truncated) {
                    ctx->truncated = true;
                    ESP_LOGW(TAG, "Telegram HTTP response truncated");
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

esp_err_t telegram_init(void)
{
    char bot_id[24];

    // Load bot token from NVS
    if (!memory_get(NVS_KEY_TG_TOKEN, s_bot_token, sizeof(s_bot_token))) {
        ESP_LOGW(TAG, "No Telegram token configured");
        return ESP_ERR_NOT_FOUND;
    }

    if (telegram_extract_bot_id(s_bot_token, bot_id, sizeof(bot_id))) {
        ESP_LOGI(TAG, "Loaded bot ID: %s (safe identifier; token remains secret)", bot_id);
    } else {
        ESP_LOGW(TAG, "Telegram token format invalid (bot ID unavailable)");
    }

    // Load last known chat ID (optional)
    char chat_id_str[24];
    if (memory_get(NVS_KEY_TG_CHAT_ID, chat_id_str, sizeof(chat_id_str))) {
        int64_t parsed_chat_id = 0;
        if (parse_chat_id_string(chat_id_str, &parsed_chat_id)) {
            s_chat_id = parsed_chat_id;
            char chat_id_buf[24];
            if (format_int64_decimal(s_chat_id, chat_id_buf, sizeof(chat_id_buf))) {
                ESP_LOGI(TAG, "Loaded chat ID: %s", chat_id_buf);
            } else {
                ESP_LOGI(TAG, "Loaded chat ID");
            }
        } else {
            s_chat_id = 0;
            ESP_LOGW(TAG, "Invalid Telegram chat ID in NVS: '%s'", chat_id_str);
        }
    }

    ESP_LOGI(TAG, "Telegram initialized");
    return ESP_OK;
}

bool telegram_is_configured(void)
{
    return s_bot_token[0] != '\0';
}

int64_t telegram_get_chat_id(void)
{
    return s_chat_id;
}

// Build URL for Telegram API
static void build_url(char *buf, size_t buf_size, const char *method)
{
    snprintf(buf, buf_size, "%s%s/%s", TELEGRAM_API_URL, s_bot_token, method);
}

static void telegram_send_typing_indicator(void)
{
    telegram_http_ctx_t *ctx = NULL;
    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_FAIL;
    int status = -1;
    int64_t started_us = esp_timer_get_time();
    net_diag_snapshot_t snapshot_before = {0};
    net_diag_snapshot_t snapshot_after = {0};
    char url[256];

    capture_net_diag_snapshot(&snapshot_before);

    if (!telegram_is_configured() || s_chat_id == 0) {
        return;
    }

    build_url(url, sizeof(url), "sendChatAction");

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    if (!cJSON_AddNumberToObject(root, "chat_id", (double)s_chat_id) ||
        !cJSON_AddStringToObject(root, "action", "typing")) {
        cJSON_Delete(root);
        return;
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        free(body);
        return;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = ctx,
        .timeout_ms = TELEGRAM_ACTION_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    client = esp_http_client_init(&config);
    if (!client) {
        free(body);
        free(ctx);
        capture_net_diag_snapshot(&snapshot_after);
        log_http_diag("sendChatAction", NULL, ESP_FAIL, -1, started_us, 0, 0, 0, 0, 0,
                      &snapshot_before, &snapshot_after);
        return;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
        if (status != 200) {
            log_http_failure("sendChatAction", client, ESP_FAIL, status);
            err = ESP_FAIL;
        }
    } else {
        log_http_failure("sendChatAction", client, err, -1);
    }

    capture_net_diag_snapshot(&snapshot_after);
    log_http_diag("sendChatAction", client, err, status, started_us, ctx->len, 0, 0, 0, 0,
                  &snapshot_before, &snapshot_after);

    esp_http_client_cleanup(client);
    free(body);
    free(ctx);
}

static void telegram_queue_typing_indicator(void)
{
    telegram_msg_t msg = {0};

    if (!s_output_queue) {
        return;
    }

    msg.kind = TELEGRAM_MSG_TYPING;
    if (xQueueSend(s_output_queue, &msg, 0) != pdTRUE) {
        ESP_LOGD(TAG, "Skipping typing indicator: output queue full");
    }
}

esp_err_t telegram_send(const char *text)
{
    telegram_http_ctx_t *ctx = NULL;
    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_FAIL;
    int status = -1;
    int64_t started_us = esp_timer_get_time();
    net_diag_snapshot_t snapshot_before = {0};
    net_diag_snapshot_t snapshot_after = {0};

    capture_net_diag_snapshot(&snapshot_before);

    if (!telegram_is_configured() || s_chat_id == 0) {
        ESP_LOGW(TAG, "Cannot send - not configured or no chat ID");
        return ESP_ERR_INVALID_STATE;
    }

    char url[256];
    build_url(url, sizeof(url), "sendMessage");

    // Build JSON body
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddNumberToObject(root, "chat_id", (double)s_chat_id) ||
        !cJSON_AddStringToObject(root, "text", text)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = ctx,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        capture_net_diag_snapshot(&snapshot_after);
        log_http_diag("sendMessage", NULL, ESP_FAIL, -1, started_us, 0, 0, 0, 0, 0,
                      &snapshot_before, &snapshot_after);
        free(body);
        free(ctx);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
        if (status != 200) {
            log_http_failure("sendMessage", client, ESP_FAIL, status);
            if (ctx->buf[0] != '\0') {
                ESP_LOGE(TAG, "sendMessage response: %s", ctx->buf);
            }
            err = ESP_FAIL;
        }
    }

    capture_net_diag_snapshot(&snapshot_after);
    log_http_diag("sendMessage", client, err, status, started_us, ctx->len, 0, 0, 0, 0,
                  &snapshot_before, &snapshot_after);

    esp_http_client_cleanup(client);
    free(body);
    free(ctx);
    return err;
}

esp_err_t telegram_send_startup(void)
{
    return telegram_send("I'm back online. What can I help you with?");
}

// Poll for updates using long polling
static esp_err_t telegram_poll(void)
{
    char url[384];
    char offset_buf[24];
    telegram_http_ctx_t *ctx = NULL;
    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_FAIL;
    int status = -1;
    int64_t next_offset;
    int64_t started_us = esp_timer_get_time();
    uint32_t poll_seq = ++s_poll_sequence;
    int result_count = 0;
    int stale_count = 0;
    int accepted_count = 0;
    net_diag_snapshot_t snapshot_before = {0};
    net_diag_snapshot_t snapshot_after = {0};

    capture_net_diag_snapshot(&snapshot_before);

    if (s_last_update_id == INT64_MAX) {
        next_offset = s_last_update_id;
    } else {
        next_offset = s_last_update_id + 1;
    }

    if (!format_int64_decimal(next_offset, offset_buf, sizeof(offset_buf))) {
        ESP_LOGE(TAG, "Failed to format Telegram update offset");
        capture_net_diag_snapshot(&snapshot_after);
        log_http_diag("getUpdates", NULL, ESP_FAIL, -1, started_us, 0,
                      0, 0, 0, poll_seq, &snapshot_before, &snapshot_after);
        return ESP_FAIL;
    }

    snprintf(url, sizeof(url), "%s%s/getUpdates?timeout=%d&limit=1&offset=%s",
             TELEGRAM_API_URL, s_bot_token, TELEGRAM_POLL_TIMEOUT, offset_buf);

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        capture_net_diag_snapshot(&snapshot_after);
        log_http_diag("getUpdates", NULL, ESP_ERR_NO_MEM, -1, started_us, 0,
                      0, 0, 0, poll_seq, &snapshot_before, &snapshot_after);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = ctx,
        .timeout_ms = (TELEGRAM_POLL_TIMEOUT + 10) * 1000,  // Add buffer to timeout
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for poll");
        capture_net_diag_snapshot(&snapshot_after);
        log_http_diag("getUpdates", NULL, ESP_FAIL, -1, started_us, 0,
                      0, 0, 0, poll_seq, &snapshot_before, &snapshot_after);
        free(ctx);
        return ESP_FAIL;
    }

    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);

    if (err != ESP_OK || status != 200) {
        log_http_failure("getUpdates", client, err, status);
        capture_net_diag_snapshot(&snapshot_after);
        log_http_diag("getUpdates", client, err, status, started_us, ctx->len,
                      0, 0, 0, poll_seq, &snapshot_before, &snapshot_after);
        esp_http_client_cleanup(client);
        free(ctx);
        return ESP_FAIL;
    }

    esp_http_client_cleanup(client);
    client = NULL;

    if (ctx->truncated) {
        int64_t recovered_update_id = 0;
        if (telegram_extract_max_update_id(ctx->buf, &recovered_update_id)) {
            s_last_update_id = recovered_update_id;
            char recovered_buf[24];
            if (format_int64_decimal(s_last_update_id, recovered_buf, sizeof(recovered_buf))) {
                ESP_LOGW(TAG, "Recovered from truncated response, skipping to update_id=%s",
                         recovered_buf);
            } else {
                ESP_LOGW(TAG, "Recovered from truncated response; update_id unavailable");
            }
            capture_net_diag_snapshot(&snapshot_after);
            log_http_diag("getUpdates", NULL, ESP_OK, status, started_us, ctx->len,
                          0, 0, 0, poll_seq, &snapshot_before, &snapshot_after);
            free(ctx);
            return ESP_OK;
        }

        ESP_LOGE(TAG, "Truncated response without parseable update_id");
        capture_net_diag_snapshot(&snapshot_after);
        log_http_diag("getUpdates", NULL, ESP_ERR_INVALID_RESPONSE, status, started_us, ctx->len,
                      0, 0, 0, poll_seq, &snapshot_before, &snapshot_after);
        free(ctx);
        return ESP_FAIL;
    }

    // Parse response
    cJSON *root = cJSON_Parse(ctx->buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse response");
        capture_net_diag_snapshot(&snapshot_after);
        log_http_diag("getUpdates", NULL, ESP_ERR_INVALID_RESPONSE, status, started_us, ctx->len,
                      0, 0, 0, poll_seq, &snapshot_before, &snapshot_after);
        free(ctx);
        return ESP_FAIL;
    }

    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    if (!ok || !cJSON_IsTrue(ok)) {
        ESP_LOGE(TAG, "API returned not ok");
        capture_net_diag_snapshot(&snapshot_after);
        log_http_diag("getUpdates", NULL, ESP_FAIL, status, started_us, ctx->len,
                      0, 0, 0, poll_seq, &snapshot_before, &snapshot_after);
        cJSON_Delete(root);
        free(ctx);
        return ESP_FAIL;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!result || !cJSON_IsArray(result)) {
        if (s_stale_only_poll_streak > 0) {
            ESP_LOGI(TAG, "Stale-only poll streak cleared at %u (empty result)",
                     (unsigned)s_stale_only_poll_streak);
            s_stale_only_poll_streak = 0;
        }
        capture_net_diag_snapshot(&snapshot_after);
        log_http_diag("getUpdates", NULL, ESP_OK, status, started_us, ctx->len,
                      0, 0, 0, poll_seq, &snapshot_before, &snapshot_after);
        cJSON_Delete(root);
        free(ctx);
        return ESP_OK;  // No updates, that's fine
    }

    cJSON *update;
    cJSON_ArrayForEach(update, result) {
        cJSON *update_id = cJSON_GetObjectItem(update, "update_id");
        int64_t incoming_update_id = -1;
        result_count++;
        if (!update_id || !cJSON_IsNumber(update_id)) {
            ESP_LOGW(TAG, "Skipping update without numeric update_id");
            continue;
        }

        // Note: cJSON stores numbers as double (53-bit precision).
        // Telegram update IDs fit safely within this range.
        incoming_update_id = (int64_t)update_id->valuedouble;
        if (incoming_update_id <= s_last_update_id) {
            stale_count++;
            char incoming_buf[24];
            char last_buf[24];
            if (format_int64_decimal(incoming_update_id, incoming_buf, sizeof(incoming_buf)) &&
                format_int64_decimal(s_last_update_id, last_buf, sizeof(last_buf))) {
                ESP_LOGW(TAG, "Skipping stale/duplicate update_id=%s (last=%s)",
                         incoming_buf, last_buf);
            } else {
                ESP_LOGW(TAG, "Skipping stale/duplicate update");
            }
            continue;
        }
        s_last_update_id = incoming_update_id;
        accepted_count++;

        cJSON *message = cJSON_GetObjectItem(update, "message");
        if (!message) continue;

        cJSON *chat = cJSON_GetObjectItem(message, "chat");
        cJSON *text = cJSON_GetObjectItem(message, "text");

        if (chat && text && cJSON_IsString(text)) {
            cJSON *chat_id = cJSON_GetObjectItem(chat, "id");
            if (chat_id && cJSON_IsNumber(chat_id)) {
                // Note: cJSON stores numbers as double (53-bit precision)
                // Telegram chat IDs fit within this range
                int64_t incoming_chat_id = (int64_t)chat_id->valuedouble;

                // Sanity check for precision loss (chat IDs > 2^53)
                if (chat_id->valuedouble > 9007199254740992.0) {
                    ESP_LOGW(TAG, "Chat ID may have precision loss");
                }

                // Authentication: reject messages from unknown chat IDs
                if (s_chat_id != 0 && incoming_chat_id != s_chat_id) {
                    char chat_id_buf[24];
                    if (format_int64_decimal(incoming_chat_id, chat_id_buf, sizeof(chat_id_buf))) {
                        ESP_LOGW(TAG, "Rejected message from unauthorized chat: %s", chat_id_buf);
                    } else {
                        ESP_LOGW(TAG, "Rejected message from unauthorized chat");
                    }
                    continue;
                }

                // If no chat ID configured, reject all (must be set during provisioning)
                if (s_chat_id == 0) {
                    char chat_id_buf[24];
                    if (format_int64_decimal(incoming_chat_id, chat_id_buf, sizeof(chat_id_buf))) {
                        ESP_LOGW(TAG, "No chat ID configured - ignoring message from %s", chat_id_buf);
                    } else {
                        ESP_LOGW(TAG, "No chat ID configured - ignoring message");
                    }
                    continue;
                }

                // Push message to input queue
                channel_msg_t msg;
                strncpy(msg.text, text->valuestring, CHANNEL_RX_BUF_SIZE - 1);
                msg.text[CHANNEL_RX_BUF_SIZE - 1] = '\0';

                char update_id_buf[24];
                if (format_int64_decimal(incoming_update_id, update_id_buf, sizeof(update_id_buf))) {
                    ESP_LOGI(TAG, "Received (update_id=%s): %s", update_id_buf, msg.text);
                } else {
                    ESP_LOGI(TAG, "Received Telegram message: %s", msg.text);
                }

                if (xQueueSend(s_input_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
                    ESP_LOGW(TAG, "Input queue full");
                } else {
                    // Queue async "working..." feedback without blocking poll loop.
                    telegram_queue_typing_indicator();
                }
            }
        }
    }

    if (result_count > 0 && stale_count == result_count && accepted_count == 0) {
        s_stale_only_poll_streak++;
        if ((s_stale_only_poll_streak % TELEGRAM_STALE_POLL_LOG_INTERVAL) == 0) {
            ESP_LOGW(TAG, "Stale-only poll streak=%u (poll_seq=%u, result_count=%d)",
                     (unsigned)s_stale_only_poll_streak, (unsigned)poll_seq, result_count);
        }

        int64_t now_us = esp_timer_get_time();
        bool cooldown_elapsed = (s_last_stale_resync_us == 0) ||
                                ((now_us - s_last_stale_resync_us) >=
                                 ((int64_t)TELEGRAM_STALE_POLL_RESYNC_COOLDOWN_MS * 1000LL));
        if (s_stale_only_poll_streak >= TELEGRAM_STALE_POLL_RESYNC_STREAK && cooldown_elapsed) {
            ESP_LOGW(TAG, "Stale-only poll anomaly: streak=%u; forcing Telegram resync",
                     (unsigned)s_stale_only_poll_streak);
            s_last_stale_resync_us = now_us;
            esp_err_t flush_err = telegram_flush_pending_updates();
            if (flush_err != ESP_OK) {
                ESP_LOGW(TAG, "Auto-resync failed: %s", esp_err_to_name(flush_err));
            } else {
                ESP_LOGI(TAG, "Auto-resync completed");
            }
            s_stale_only_poll_streak = 0;
        }
    } else if (s_stale_only_poll_streak > 0) {
        ESP_LOGI(TAG, "Stale-only poll streak cleared at %u",
                 (unsigned)s_stale_only_poll_streak);
        s_stale_only_poll_streak = 0;
    }

    capture_net_diag_snapshot(&snapshot_after);
    log_http_diag("getUpdates", NULL, ESP_OK, status, started_us, ctx->len,
                  result_count, stale_count, accepted_count, poll_seq,
                  &snapshot_before, &snapshot_after);

    cJSON_Delete(root);
    free(ctx);
    return ESP_OK;
}

// Telegram response task - watches output queue, sends to Telegram
static void telegram_send_task(void *arg)
{
    (void)arg;
    while (1) {
        if (xQueueReceive(s_output_queue, &s_send_msg, portMAX_DELAY) == pdTRUE) {
            if (!telegram_is_configured() || s_chat_id == 0) {
                continue;
            }

            if (s_send_msg.kind == TELEGRAM_MSG_TYPING) {
                int64_t now_us = esp_timer_get_time();
                if (s_last_typing_sent_us > 0 && now_us > s_last_typing_sent_us) {
                    uint64_t since_last_ms =
                        (uint64_t)(now_us - s_last_typing_sent_us) / 1000ULL;
                    if (since_last_ms < TELEGRAM_TYPING_DEBOUNCE_MS) {
                        continue;
                    }
                }

                s_last_typing_sent_us = now_us;
                telegram_send_typing_indicator();
                continue;
            }

            if (s_send_msg.kind == TELEGRAM_MSG_TEXT) {
                telegram_send(s_send_msg.text);
                continue;
            }

            ESP_LOGW(TAG, "Ignoring unknown telegram queue message type=%d", (int)s_send_msg.kind);
        }
    }
}

static esp_err_t telegram_flush_pending_updates(void)
{
#if TELEGRAM_FLUSH_ON_START
    char url[384];
    telegram_http_ctx_t *ctx = NULL;
    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_FAIL;
    int status = -1;
    int64_t started_us = esp_timer_get_time();
    net_diag_snapshot_t snapshot_before = {0};
    net_diag_snapshot_t snapshot_after = {0};

    capture_net_diag_snapshot(&snapshot_before);

    snprintf(url, sizeof(url), "%s%s/getUpdates?timeout=0&limit=1&offset=-1",
             TELEGRAM_API_URL, s_bot_token);

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        capture_net_diag_snapshot(&snapshot_after);
        log_http_diag("flush getUpdates", NULL, ESP_ERR_NO_MEM, -1, started_us, 0,
                      0, 0, 0, 0, &snapshot_before, &snapshot_after);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = ctx,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    client = esp_http_client_init(&config);
    if (!client) {
        capture_net_diag_snapshot(&snapshot_after);
        log_http_diag("flush getUpdates", NULL, ESP_FAIL, -1, started_us, 0,
                      0, 0, 0, 0, &snapshot_before, &snapshot_after);
        free(ctx);
        return ESP_FAIL;
    }

    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);

    if (err != ESP_OK || status != 200) {
        log_http_failure("flush getUpdates", client, err, status);
        capture_net_diag_snapshot(&snapshot_after);
        log_http_diag("flush getUpdates", client, err, status, started_us, ctx->len,
                      0, 0, 0, 0, &snapshot_before, &snapshot_after);
        esp_http_client_cleanup(client);
        free(ctx);
        return ESP_FAIL;
    }

    esp_http_client_cleanup(client);
    client = NULL;

    int64_t latest_update_id = 0;
    if (telegram_extract_max_update_id(ctx->buf, &latest_update_id)) {
        s_last_update_id = latest_update_id;
        s_stale_only_poll_streak = 0;
        char update_id_buf[24];
        if (format_int64_decimal(s_last_update_id, update_id_buf, sizeof(update_id_buf))) {
            ESP_LOGI(TAG, "Flushed pending updates up to update_id=%s", update_id_buf);
        } else {
            ESP_LOGI(TAG, "Flushed pending updates");
        }
    } else {
        ESP_LOGI(TAG, "No pending Telegram updates to flush");
    }

    capture_net_diag_snapshot(&snapshot_after);
    log_http_diag("flush getUpdates", NULL, ESP_OK, status, started_us, ctx->len,
                  0, 0, 0, 0, &snapshot_before, &snapshot_after);

    free(ctx);
#endif
    return ESP_OK;
}

// Calculate exponential backoff delay
static int get_backoff_delay_ms(void)
{
    if (s_consecutive_failures == 0) {
        return 0;
    }

    int delay = BACKOFF_BASE_MS;
    for (int i = 1; i < s_consecutive_failures && delay < BACKOFF_MAX_MS; i++) {
        delay *= BACKOFF_MULTIPLIER;
    }

    if (delay > BACKOFF_MAX_MS) {
        delay = BACKOFF_MAX_MS;
    }

    return delay;
}

// Telegram polling task - polls for new messages
static void telegram_poll_task(void *arg)
{
    ESP_LOGI(TAG, "Polling task started");

    while (1) {
        if (telegram_is_configured()) {
            esp_err_t err = telegram_poll();
            if (err != ESP_OK) {
                s_consecutive_failures++;
                int backoff_ms = get_backoff_delay_ms();
                ESP_LOGW(TAG, "Poll failed (%d consecutive), backoff %dms",
                         s_consecutive_failures, backoff_ms);
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            } else {
                // Success - reset backoff
                if (s_consecutive_failures > 0) {
                    ESP_LOGI(TAG, "Poll recovered after %d failures", s_consecutive_failures);
                    s_consecutive_failures = 0;
                }
            }
        } else {
            // Not configured, check again later
            vTaskDelay(pdMS_TO_TICKS(10000));
        }

        // Small delay between successful polls
        vTaskDelay(pdMS_TO_TICKS(TELEGRAM_POLL_INTERVAL));
    }
}

esp_err_t telegram_start(QueueHandle_t input_queue, QueueHandle_t output_queue)
{
    if (!input_queue || !output_queue) {
        ESP_LOGE(TAG, "Invalid queues for Telegram startup");
        return ESP_ERR_INVALID_ARG;
    }

    s_input_queue = input_queue;
    s_output_queue = output_queue;

    // Sync to the latest pending update to avoid replaying stale backlog on reboot.
    esp_err_t flush_err = telegram_flush_pending_updates();
    if (flush_err != ESP_OK) {
        ESP_LOGW(TAG, "Proceeding without startup flush; pending updates may replay");
    }

    TaskHandle_t poll_task = NULL;
    if (xTaskCreate(telegram_poll_task, "tg_poll", TELEGRAM_POLL_TASK_STACK_SIZE, NULL,
                    CHANNEL_TASK_PRIORITY, &poll_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Telegram poll task");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(telegram_send_task, "tg_send", CHANNEL_TASK_STACK_SIZE, NULL,
                    CHANNEL_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Telegram send task");
        vTaskDelete(poll_task);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Telegram tasks started");
    return ESP_OK;
}
