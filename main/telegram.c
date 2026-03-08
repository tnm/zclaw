#include "telegram.h"
#include "config.h"
#include "http_gate.h"
#include "messages.h"
#include "memory.h"
#include "nvs_keys.h"
#include "llm.h"
#include "telegram_http_diag.h"
#include "telegram_poll_policy.h"
#include "telegram_targets.h"
#include "telegram_token.h"
#include "telegram_update.h"
#include "text_buffer.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>

static const char *TAG = "telegram";

static QueueHandle_t s_input_queue;
static QueueHandle_t s_output_queue;
static char s_bot_token[64] = {0};
static int64_t s_last_update_id = 0;
static telegram_msg_t s_send_msg;
static uint32_t s_stale_only_poll_streak = 0;
static uint32_t s_poll_sequence = 0;
static int64_t s_last_stale_resync_us = 0;
static portMUX_TYPE s_poll_pause_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_poll_pause_count = 0;

// Exponential backoff state
static int s_consecutive_failures = 0;
#define BACKOFF_BASE_MS     5000    // 5 seconds
#define BACKOFF_MAX_MS      300000  // 5 minutes
#define BACKOFF_MULTIPLIER  2
#define TELEGRAM_POLL_TASK_STACK_SIZE 8192

static esp_err_t telegram_flush_pending_updates(void);

static bool telegram_polling_is_paused(void)
{
    bool paused = false;

    taskENTER_CRITICAL(&s_poll_pause_mux);
    paused = (s_poll_pause_count > 0);
    taskEXIT_CRITICAL(&s_poll_pause_mux);

    return paused;
}

void telegram_pause_polling(void)
{
    uint32_t pause_count = 0;

    taskENTER_CRITICAL(&s_poll_pause_mux);
    s_poll_pause_count++;
    pause_count = s_poll_pause_count;
    taskEXIT_CRITICAL(&s_poll_pause_mux);

    ESP_LOGD(TAG, "Telegram polling paused (count=%u)", (unsigned)pause_count);
}

void telegram_resume_polling(void)
{
    uint32_t pause_count = 0;
    bool underflow = false;

    taskENTER_CRITICAL(&s_poll_pause_mux);
    if (s_poll_pause_count == 0) {
        underflow = true;
    } else {
        s_poll_pause_count--;
    }
    pause_count = s_poll_pause_count;
    taskEXIT_CRITICAL(&s_poll_pause_mux);

    if (underflow) {
        ESP_LOGW(TAG, "telegram_resume_polling called without a matching pause");
        return;
    }

    ESP_LOGD(TAG, "Telegram polling resumed (count=%u)", (unsigned)pause_count);
}

static int64_t resolve_target_chat_id(int64_t requested_chat_id)
{
    int64_t target_chat_id = telegram_targets_resolve_target_chat_id(requested_chat_id);

    if (requested_chat_id != 0 && target_chat_id == 0) {
        ESP_LOGW(TAG, "Refusing outbound send to unauthorized chat ID");
    }

    return target_chat_id;
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

    telegram_targets_clear();

    // Preferred format: comma-separated allowlist.
    char chat_ids_str[128];
    if (memory_get(NVS_KEY_TG_CHAT_IDS, chat_ids_str, sizeof(chat_ids_str))) {
        if (telegram_targets_set_from_string(chat_ids_str)) {
            char chat_id_buf[24];
            if (telegram_format_int64_decimal(telegram_targets_primary_chat_id(),
                                              chat_id_buf,
                                              sizeof(chat_id_buf))) {
                ESP_LOGI(TAG, "Loaded %u authorized chat IDs (primary: %s)",
                         (unsigned)telegram_targets_count(), chat_id_buf);
            } else {
                ESP_LOGI(TAG, "Loaded %u authorized chat IDs",
                         (unsigned)telegram_targets_count());
            }
        } else {
            ESP_LOGW(TAG, "Invalid Telegram chat ID list in NVS: '%s'", chat_ids_str);
        }
    }

    // Backward compatibility: single ID key.
    if (!telegram_targets_has_any()) {
        char chat_id_str[24];
        if (memory_get(NVS_KEY_TG_CHAT_ID, chat_id_str, sizeof(chat_id_str))) {
            if (telegram_targets_set_from_string(chat_id_str)) {
                char chat_id_buf[24];
                if (telegram_format_int64_decimal(telegram_targets_primary_chat_id(),
                                                  chat_id_buf,
                                                  sizeof(chat_id_buf))) {
                    ESP_LOGI(TAG, "Loaded chat ID: %s", chat_id_buf);
                } else {
                    ESP_LOGI(TAG, "Loaded chat ID");
                }
            } else {
                ESP_LOGW(TAG, "Invalid Telegram chat ID in NVS: '%s'", chat_id_str);
            }
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
    return telegram_targets_primary_chat_id();
}

// Build URL for Telegram API
static void build_url(char *buf, size_t buf_size, const char *method)
{
    snprintf(buf, buf_size, "%s%s/%s", TELEGRAM_API_URL, s_bot_token, method);
}

static esp_err_t telegram_send_to_chat(int64_t chat_id, const char *text)
{
    telegram_http_ctx_t *ctx = NULL;
    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_FAIL;
    int status = -1;
    int64_t started_us = esp_timer_get_time();
    telegram_http_diag_snapshot_t snapshot_before = {0};
    telegram_http_diag_snapshot_t snapshot_after = {0};
    bool gate_acquired = false;

    telegram_http_diag_capture_snapshot(&snapshot_before);

    if (!telegram_is_configured() || chat_id == 0) {
        ESP_LOGW(TAG, "Cannot send - not configured or no authorized chat IDs");
        return ESP_ERR_INVALID_STATE;
    }

    char url[256];
    build_url(url, sizeof(url), "sendMessage");

    // Build JSON body
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddNumberToObject(root, "chat_id", (double)chat_id) ||
        !cJSON_AddStringToObject(root, "text", text)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    gate_acquired = http_gate_acquire("telegram_send", pdMS_TO_TICKS(HTTP_TIMEOUT_MS + 1000));
    if (!gate_acquired) {
        telegram_http_diag_capture_snapshot(&snapshot_after);
        telegram_http_diag_log("sendMessage", NULL, ESP_ERR_TIMEOUT, -1, started_us, 0, 0, 0, 0,
                               0, &snapshot_before, &snapshot_after);
        free(body);
        return ESP_ERR_TIMEOUT;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        http_gate_release();
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
        telegram_http_diag_capture_snapshot(&snapshot_after);
        telegram_http_diag_log("sendMessage", NULL, ESP_FAIL, -1, started_us, 0, 0, 0, 0, 0,
                               &snapshot_before, &snapshot_after);
        free(body);
        free(ctx);
        http_gate_release();
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
        if (status != 200) {
            telegram_http_diag_log_failure("sendMessage", client, ESP_FAIL, status);
            if (ctx->buf[0] != '\0') {
                ESP_LOGE(TAG, "sendMessage response: %s", ctx->buf);
            }
            err = ESP_FAIL;
        }
    }

    telegram_http_diag_capture_snapshot(&snapshot_after);
    telegram_http_diag_log("sendMessage", client, err, status, started_us, ctx->len, 0, 0, 0, 0,
                           &snapshot_before, &snapshot_after);

    esp_http_client_cleanup(client);
    free(body);
    free(ctx);
    http_gate_release();
    return err;
}

esp_err_t telegram_send(const char *text)
{
    return telegram_send_to_chat(resolve_target_chat_id(0), text);
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
    int poll_timeout_s = telegram_poll_timeout_for_backend(llm_get_backend());
    telegram_http_diag_snapshot_t snapshot_before = {0};
    telegram_http_diag_snapshot_t snapshot_after = {0};
    bool gate_acquired = false;

    if (telegram_polling_is_paused()) {
        return ESP_OK;
    }

    telegram_http_diag_capture_snapshot(&snapshot_before);

    if (s_last_update_id == INT64_MAX) {
        next_offset = s_last_update_id;
    } else {
        next_offset = s_last_update_id + 1;
    }

    if (!telegram_format_int64_decimal(next_offset, offset_buf, sizeof(offset_buf))) {
        ESP_LOGE(TAG, "Failed to format Telegram update offset");
        telegram_http_diag_capture_snapshot(&snapshot_after);
        telegram_http_diag_log("getUpdates", NULL, ESP_FAIL, -1, started_us, 0, 0, 0, 0,
                               poll_seq, &snapshot_before, &snapshot_after);
        return ESP_FAIL;
    }

    snprintf(url, sizeof(url), "%s%s/getUpdates?timeout=%d&limit=1&offset=%s",
             TELEGRAM_API_URL, s_bot_token, poll_timeout_s, offset_buf);

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        telegram_http_diag_capture_snapshot(&snapshot_after);
        telegram_http_diag_log("getUpdates", NULL, ESP_ERR_NO_MEM, -1, started_us, 0, 0, 0, 0,
                               poll_seq, &snapshot_before, &snapshot_after);
        return ESP_ERR_NO_MEM;
    }

    gate_acquired = http_gate_acquire("telegram_poll", 0);
    if (!gate_acquired) {
        free(ctx);
        return ESP_OK;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = ctx,
        .timeout_ms = (poll_timeout_s + 10) * 1000,  // Add buffer to timeout
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for poll");
        telegram_http_diag_capture_snapshot(&snapshot_after);
        telegram_http_diag_log("getUpdates", NULL, ESP_FAIL, -1, started_us, 0, 0, 0, 0,
                               poll_seq, &snapshot_before, &snapshot_after);
        free(ctx);
        http_gate_release();
        return ESP_FAIL;
    }

    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);

    if (err != ESP_OK || status != 200) {
        telegram_http_diag_log_failure("getUpdates", client, err, status);
        telegram_http_diag_capture_snapshot(&snapshot_after);
        telegram_http_diag_log("getUpdates", client, err, status, started_us, ctx->len,
                               0, 0, 0, poll_seq, &snapshot_before, &snapshot_after);
        esp_http_client_cleanup(client);
        http_gate_release();
        free(ctx);
        return ESP_FAIL;
    }

    esp_http_client_cleanup(client);
    http_gate_release();
    client = NULL;

    if (ctx->truncated) {
        int64_t recovered_update_id = 0;
        if (telegram_extract_max_update_id(ctx->buf, &recovered_update_id)) {
            s_last_update_id = recovered_update_id;
            char recovered_buf[24];
            if (telegram_format_int64_decimal(s_last_update_id,
                                              recovered_buf,
                                              sizeof(recovered_buf))) {
                ESP_LOGW(TAG, "Recovered from truncated response, skipping to update_id=%s",
                         recovered_buf);
            } else {
                ESP_LOGW(TAG, "Recovered from truncated response; update_id unavailable");
            }
            telegram_http_diag_capture_snapshot(&snapshot_after);
            telegram_http_diag_log("getUpdates", NULL, ESP_OK, status, started_us, ctx->len,
                                   0, 0, 0, poll_seq, &snapshot_before, &snapshot_after);
            free(ctx);
            return ESP_OK;
        }

        ESP_LOGE(TAG, "Truncated response without parseable update_id");
        telegram_http_diag_capture_snapshot(&snapshot_after);
        telegram_http_diag_log("getUpdates", NULL, ESP_ERR_INVALID_RESPONSE, status, started_us,
                               ctx->len, 0, 0, 0, poll_seq, &snapshot_before, &snapshot_after);
        free(ctx);
        return ESP_FAIL;
    }

    // Parse response
    cJSON *root = cJSON_Parse(ctx->buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse response");
        telegram_http_diag_capture_snapshot(&snapshot_after);
        telegram_http_diag_log("getUpdates", NULL, ESP_ERR_INVALID_RESPONSE, status, started_us,
                               ctx->len, 0, 0, 0, poll_seq, &snapshot_before, &snapshot_after);
        free(ctx);
        return ESP_FAIL;
    }

    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    if (!ok || !cJSON_IsTrue(ok)) {
        ESP_LOGE(TAG, "API returned not ok");
        telegram_http_diag_capture_snapshot(&snapshot_after);
        telegram_http_diag_log("getUpdates", NULL, ESP_FAIL, status, started_us, ctx->len,
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
        telegram_http_diag_capture_snapshot(&snapshot_after);
        telegram_http_diag_log("getUpdates", NULL, ESP_OK, status, started_us, ctx->len,
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
            if (telegram_format_int64_decimal(incoming_update_id,
                                              incoming_buf,
                                              sizeof(incoming_buf)) &&
                telegram_format_int64_decimal(s_last_update_id, last_buf, sizeof(last_buf))) {
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

                // Require explicit allowlist configuration.
                if (!telegram_targets_has_any()) {
                    char chat_id_buf[24];
                    if (telegram_format_int64_decimal(incoming_chat_id,
                                                      chat_id_buf,
                                                      sizeof(chat_id_buf))) {
                        ESP_LOGW(TAG, "No chat ID configured - ignoring message from %s", chat_id_buf);
                    } else {
                        ESP_LOGW(TAG, "No chat ID configured - ignoring message");
                    }
                    continue;
                }

                // Authentication: reject messages from unknown chat IDs.
                if (!telegram_targets_is_authorized(incoming_chat_id)) {
                    char chat_id_buf[24];
                    if (telegram_format_int64_decimal(incoming_chat_id,
                                                      chat_id_buf,
                                                      sizeof(chat_id_buf))) {
                        ESP_LOGW(TAG, "Rejected message from unauthorized chat: %s", chat_id_buf);
                    } else {
                        ESP_LOGW(TAG, "Rejected message from unauthorized chat");
                    }
                    continue;
                }

                // Push message to input queue
                channel_msg_t msg;
                strncpy(msg.text, text->valuestring, CHANNEL_RX_BUF_SIZE - 1);
                msg.text[CHANNEL_RX_BUF_SIZE - 1] = '\0';
                msg.source = MSG_SOURCE_TELEGRAM;
                msg.chat_id = incoming_chat_id;

                char update_id_buf[24];
                if (telegram_format_int64_decimal(incoming_update_id,
                                                  update_id_buf,
                                                  sizeof(update_id_buf))) {
                    ESP_LOGI(TAG, "Received (update_id=%s): %s", update_id_buf, msg.text);
                } else {
                    ESP_LOGI(TAG, "Received Telegram message: %s", msg.text);
                }

                if (xQueueSend(s_input_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
                    ESP_LOGW(TAG, "Input queue full");
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

    telegram_http_diag_capture_snapshot(&snapshot_after);
    telegram_http_diag_log("getUpdates", NULL, ESP_OK, status, started_us, ctx->len,
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
            int64_t target_chat_id = resolve_target_chat_id(s_send_msg.chat_id);
            if (!telegram_is_configured() || target_chat_id == 0) {
                continue;
            }

            telegram_send_to_chat(target_chat_id, s_send_msg.text);
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
    telegram_http_diag_snapshot_t snapshot_before = {0};
    telegram_http_diag_snapshot_t snapshot_after = {0};
    bool gate_acquired = false;

    telegram_http_diag_capture_snapshot(&snapshot_before);

    snprintf(url, sizeof(url), "%s%s/getUpdates?timeout=0&limit=1&offset=-1",
             TELEGRAM_API_URL, s_bot_token);

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        telegram_http_diag_capture_snapshot(&snapshot_after);
        telegram_http_diag_log("flush getUpdates", NULL, ESP_ERR_NO_MEM, -1, started_us, 0,
                               0, 0, 0, 0, &snapshot_before, &snapshot_after);
        return ESP_ERR_NO_MEM;
    }

    gate_acquired = http_gate_acquire("telegram_flush", pdMS_TO_TICKS(HTTP_TIMEOUT_MS + 1000));
    if (!gate_acquired) {
        telegram_http_diag_capture_snapshot(&snapshot_after);
        telegram_http_diag_log("flush getUpdates", NULL, ESP_ERR_TIMEOUT, -1, started_us, 0,
                               0, 0, 0, 0, &snapshot_before, &snapshot_after);
        free(ctx);
        return ESP_ERR_TIMEOUT;
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
        telegram_http_diag_capture_snapshot(&snapshot_after);
        telegram_http_diag_log("flush getUpdates", NULL, ESP_FAIL, -1, started_us, 0, 0, 0, 0,
                               0, &snapshot_before, &snapshot_after);
        free(ctx);
        http_gate_release();
        return ESP_FAIL;
    }

    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);

    if (err != ESP_OK || status != 200) {
        telegram_http_diag_log_failure("flush getUpdates", client, err, status);
        telegram_http_diag_capture_snapshot(&snapshot_after);
        telegram_http_diag_log("flush getUpdates", client, err, status, started_us, ctx->len,
                               0, 0, 0, 0, &snapshot_before, &snapshot_after);
        esp_http_client_cleanup(client);
        http_gate_release();
        free(ctx);
        return ESP_FAIL;
    }

    esp_http_client_cleanup(client);
    http_gate_release();
    client = NULL;

    int64_t latest_update_id = 0;
    if (telegram_extract_max_update_id(ctx->buf, &latest_update_id)) {
        s_last_update_id = latest_update_id;
        s_stale_only_poll_streak = 0;
        char update_id_buf[24];
        if (telegram_format_int64_decimal(s_last_update_id,
                                          update_id_buf,
                                          sizeof(update_id_buf))) {
            ESP_LOGI(TAG, "Flushed pending updates up to update_id=%s", update_id_buf);
        } else {
            ESP_LOGI(TAG, "Flushed pending updates");
        }
    } else {
        ESP_LOGI(TAG, "No pending Telegram updates to flush");
    }

    telegram_http_diag_capture_snapshot(&snapshot_after);
    telegram_http_diag_log("flush getUpdates", NULL, ESP_OK, status, started_us, ctx->len,
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
