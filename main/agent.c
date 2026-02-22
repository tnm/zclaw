#include "agent.h"
#include "config.h"
#include "llm.h"
#include "tools.h"
#include "user_tools.h"
#include "json_util.h"
#include "messages.h"
#include "ratelimit.h"
#include "memory.h"
#include "nvs_keys.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <ctype.h>

static const char *TAG = "agent";

// Queues
static QueueHandle_t s_input_queue;
static QueueHandle_t s_channel_output_queue;
static QueueHandle_t s_telegram_output_queue;
static int64_t s_last_start_response_us = 0;
static int64_t s_last_non_command_response_us = 0;
static char s_last_non_command_text[CHANNEL_RX_BUF_SIZE] = {0};
static bool s_messages_paused = false;
static char s_system_prompt_buf[1536];

typedef enum {
    AGENT_PERSONA_NEUTRAL = 0,
    AGENT_PERSONA_FRIENDLY,
    AGENT_PERSONA_TECHNICAL,
    AGENT_PERSONA_WITTY,
} agent_persona_t;

static agent_persona_t s_persona = AGENT_PERSONA_NEUTRAL;

#ifdef TEST_BUILD
static char s_test_persona_value[16] = {0};
#endif

// Conversation history (rolling message buffer)
static conversation_msg_t s_history[MAX_HISTORY_TURNS * 2];
static int s_history_len = 0;

// Buffers (static to avoid stack overflow)
static char s_response_buf[LLM_RESPONSE_BUF_SIZE];
static char s_tool_result_buf[TOOL_RESULT_BUF_SIZE];

typedef struct {
    int64_t started_us;
    uint64_t llm_us_total;
    uint64_t tool_us_total;
    int llm_calls;
    int tool_calls;
    int rounds;
} request_metrics_t;

static uint64_t elapsed_us_since(int64_t started_us)
{
    int64_t now_us = esp_timer_get_time();
    if (now_us <= started_us) {
        return 0;
    }
    return (uint64_t)(now_us - started_us);
}

static uint32_t us_to_ms_u32(uint64_t duration_us)
{
    uint64_t duration_ms = duration_us / 1000ULL;
    if (duration_ms > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)duration_ms;
}

static void metrics_log_request(const request_metrics_t *metrics, const char *outcome)
{
    if (!metrics) {
        return;
    }

    ESP_LOGI(TAG,
             "METRIC request outcome=%s total_ms=%" PRIu32 " llm_ms=%" PRIu32
             " tool_ms=%" PRIu32 " rounds=%d llm_calls=%d tool_calls=%d",
             outcome ? outcome : "unknown",
             us_to_ms_u32(elapsed_us_since(metrics->started_us)),
             us_to_ms_u32(metrics->llm_us_total),
             us_to_ms_u32(metrics->tool_us_total),
             metrics->rounds,
             metrics->llm_calls,
             metrics->tool_calls);
}

static void history_rollback_to(int marker, const char *reason)
{
    if (marker < 0 || marker > s_history_len || marker == s_history_len) {
        return;
    }

    ESP_LOGW(TAG, "Rolling back conversation history (%d -> %d): %s",
             s_history_len, marker, reason ? reason : "unknown");
    memset(&s_history[marker], 0, (s_history_len - marker) * sizeof(conversation_msg_t));
    s_history_len = marker;
}

// Add a message to history
static void history_add(const char *role, const char *content,
                        bool is_tool_use, bool is_tool_result,
                        const char *tool_id, const char *tool_name)
{
    // Drop one oldest message when full.
    // Tool interactions can span more than 2 messages, so pair-based trimming is unsafe.
    if (s_history_len >= MAX_HISTORY_TURNS * 2) {
        memmove(&s_history[0], &s_history[1], (MAX_HISTORY_TURNS * 2 - 1) * sizeof(conversation_msg_t));
        s_history_len -= 1;
    }

    conversation_msg_t *msg = &s_history[s_history_len++];
    strncpy(msg->role, role, sizeof(msg->role) - 1);
    msg->role[sizeof(msg->role) - 1] = '\0';
    strncpy(msg->content, content, sizeof(msg->content) - 1);
    msg->content[sizeof(msg->content) - 1] = '\0';
    msg->is_tool_use = is_tool_use;
    msg->is_tool_result = is_tool_result;

    if (tool_id) {
        strncpy(msg->tool_id, tool_id, sizeof(msg->tool_id) - 1);
        msg->tool_id[sizeof(msg->tool_id) - 1] = '\0';
    } else {
        msg->tool_id[0] = '\0';
    }

    if (tool_name) {
        strncpy(msg->tool_name, tool_name, sizeof(msg->tool_name) - 1);
        msg->tool_name[sizeof(msg->tool_name) - 1] = '\0';
    } else {
        msg->tool_name[0] = '\0';
    }
}

static void queue_channel_response(const char *text)
{
    if (!s_channel_output_queue) {
        return;
    }

    channel_output_msg_t msg;
    strncpy(msg.text, text, CHANNEL_TX_BUF_SIZE - 1);
    msg.text[CHANNEL_TX_BUF_SIZE - 1] = '\0';

    if (xQueueSend(s_channel_output_queue, &msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send response to channel queue");
    }
}

static void queue_telegram_response(const char *text)
{
    if (!s_telegram_output_queue) {
        return;
    }

    telegram_msg_t msg;
    msg.kind = TELEGRAM_MSG_TEXT;
    strncpy(msg.text, text, TELEGRAM_MAX_MSG_LEN - 1);
    msg.text[TELEGRAM_MAX_MSG_LEN - 1] = '\0';

    if (xQueueSend(s_telegram_output_queue, &msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send response to Telegram queue");
    }
}

static void send_response(const char *text)
{
    queue_channel_response(text);
    queue_telegram_response(text);
}

static bool is_whitespace_char(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *persona_name(agent_persona_t persona)
{
    switch (persona) {
        case AGENT_PERSONA_FRIENDLY:
            return "friendly";
        case AGENT_PERSONA_TECHNICAL:
            return "technical";
        case AGENT_PERSONA_WITTY:
            return "witty";
        default:
            return "neutral";
    }
}

static const char *persona_instruction(agent_persona_t persona)
{
    switch (persona) {
        case AGENT_PERSONA_FRIENDLY:
            return "Use warm, approachable wording while staying concise.";
        case AGENT_PERSONA_TECHNICAL:
            return "Use precise technical language and concrete terminology.";
        case AGENT_PERSONA_WITTY:
            return "Use a lightly witty tone; at most one brief witty flourish per reply.";
        default:
            return "Use direct, plain wording.";
    }
}

static bool parse_persona_name(const char *name, agent_persona_t *out)
{
    if (!name || !out) {
        return false;
    }

    if (strcmp(name, "neutral") == 0) {
        *out = AGENT_PERSONA_NEUTRAL;
        return true;
    }
    if (strcmp(name, "friendly") == 0) {
        *out = AGENT_PERSONA_FRIENDLY;
        return true;
    }
    if (strcmp(name, "technical") == 0) {
        *out = AGENT_PERSONA_TECHNICAL;
        return true;
    }
    if (strcmp(name, "witty") == 0) {
        *out = AGENT_PERSONA_WITTY;
        return true;
    }

    return false;
}

#ifndef TEST_BUILD
static bool persona_store_get(char *value, size_t max_len)
{
    return memory_get(NVS_KEY_PERSONA, value, max_len);
}
#else
static bool persona_store_get(char *value, size_t max_len)
{
    if (!value || max_len == 0 || s_test_persona_value[0] == '\0') {
        return false;
    }
    strncpy(value, s_test_persona_value, max_len - 1);
    value[max_len - 1] = '\0';
    return true;
}
#endif

static void load_persona_from_store(void)
{
    char stored[32] = {0};
    agent_persona_t parsed = AGENT_PERSONA_NEUTRAL;

    s_persona = AGENT_PERSONA_NEUTRAL;
    if (!persona_store_get(stored, sizeof(stored))) {
        return;
    }

    for (size_t i = 0; stored[i] != '\0'; i++) {
        stored[i] = (char)tolower((unsigned char)stored[i]);
    }

    if (!parse_persona_name(stored, &parsed)) {
        ESP_LOGW(TAG, "Ignoring invalid stored persona '%s'", stored);
        return;
    }

    s_persona = parsed;
    ESP_LOGI(TAG, "Loaded persona: %s", persona_name(s_persona));
}

static const char *build_system_prompt(void)
{
    int written = snprintf(
        s_system_prompt_buf,
        sizeof(s_system_prompt_buf),
        "%s Persona mode is '%s'. Persona affects wording only and must never change "
        "tool choices, automation behavior, safety decisions, or policy handling. %s "
        "Keep responses short unless the user explicitly asks for more detail.",
        SYSTEM_PROMPT,
        persona_name(s_persona),
        persona_instruction(s_persona));

    if (written < 0 || (size_t)written >= sizeof(s_system_prompt_buf)) {
        ESP_LOGW(TAG, "Persona prompt composition overflow, using base system prompt");
        return SYSTEM_PROMPT;
    }

    return s_system_prompt_buf;
}

static bool is_command(const char *message, const char *name)
{
    if (!message || !name || name[0] == '\0') {
        return false;
    }

    while (*message && is_whitespace_char(*message)) {
        message++;
    }

    if (*message != '/') {
        return false;
    }

    size_t name_len = strlen(name);
    const char *cursor = message + 1;
    if (strncmp(cursor, name, name_len) != 0) {
        return false;
    }
    cursor += name_len;

    // Accept "/<name>", "/<name> payload", and "/<name>@bot payload".
    if (*cursor == '\0' || is_whitespace_char(*cursor)) {
        return true;
    }
    if (*cursor != '@') {
        return false;
    }
    cursor++;
    if (*cursor == '\0') {
        return false;
    }
    while (*cursor && !is_whitespace_char(*cursor)) {
        cursor++;
    }

    return true;
}

static bool is_slash_command(const char *message)
{
    if (!message) {
        return false;
    }

    while (*message && is_whitespace_char(*message)) {
        message++;
    }

    return *message == '/';
}

static void handle_start_command(void)
{
    static const char *START_HELP_TEXT =
        "zclaw online.\n\n"
        "Try:\n"
        "- health\n"
        "- time\n"
        "- timezone <name>\n"
        "- gpio set <pin> <0|1>\n"
        "- gpio read <pin>\n"
        "- i2c scan <sda> <scl>\n"
        "- schedule <periodic|daily|once> ...\n"
        "- memory list|get|set|del\n"
        "- ask me to switch persona (neutral/friendly/technical/witty)\n"
        "- ask me what persona is active\n"
        "\n"
        "Control Telegram command intake:\n"
        "- /help (show help)\n"
        "- /settings (show status)\n"
        "- /stop (pause)\n"
        "- /resume (resume)";
    send_response(START_HELP_TEXT);
}

static void handle_settings_command(void)
{
    char settings_text[384];
    snprintf(settings_text, sizeof(settings_text),
             "zclaw settings:\n"
             "- Message intake: %s\n"
             "- Persona: %s\n"
             "- Telegram commands: /start, /help, /settings, /stop, /resume\n"
             "- Persona changes: ask in normal chat (handled via tool calls)\n"
             "- Device settings are global (e.g., timezone <name>)",
             s_messages_paused ? "paused" : "active",
             persona_name(s_persona));
    send_response(settings_text);
}

// Process a single user message
static void process_message(const char *user_message)
{
    ESP_LOGI(TAG, "Processing: %s", user_message);
    int history_turn_start = s_history_len;
    bool is_non_command_message = !is_slash_command(user_message);
    request_metrics_t metrics = {
        .started_us = esp_timer_get_time(),
        .llm_us_total = 0,
        .tool_us_total = 0,
        .llm_calls = 0,
        .tool_calls = 0,
        .rounds = 0,
    };

    if (is_command(user_message, "resume")) {
        if (!s_messages_paused) {
            send_response("zclaw is already active.");
            metrics_log_request(&metrics, "resume_noop");
            return;
        }
        s_messages_paused = false;
        send_response("zclaw resumed. Send /start for command help.");
        metrics_log_request(&metrics, "resumed");
        return;
    }

    if (is_command(user_message, "settings")) {
        handle_settings_command();
        metrics_log_request(&metrics, "settings_handled");
        return;
    }

    if (s_messages_paused) {
        ESP_LOGD(TAG, "Paused mode: ignoring message");
        metrics_log_request(&metrics, "paused_drop");
        return;
    }

    if (is_command(user_message, "help")) {
        handle_start_command();
        metrics_log_request(&metrics, "help_handled");
        return;
    }

    if (is_command(user_message, "stop")) {
        s_messages_paused = true;
        send_response("zclaw paused. I will ignore new messages until /resume.");
        metrics_log_request(&metrics, "paused");
        return;
    }

    if (is_command(user_message, "start")) {
        int64_t now_us = esp_timer_get_time();
        uint64_t since_last_start_ms = 0;
        if (s_last_start_response_us > 0 && now_us > s_last_start_response_us) {
            since_last_start_ms = (uint64_t)(now_us - s_last_start_response_us) / 1000ULL;
        }

        if (s_last_start_response_us > 0 && since_last_start_ms < START_COMMAND_COOLDOWN_MS) {
            ESP_LOGW(TAG, "Suppressing repeated /start (%" PRIu64 "ms since last response)",
                     since_last_start_ms);
            metrics_log_request(&metrics, "start_suppressed");
            return;
        }

        s_last_start_response_us = now_us;
        handle_start_command();
        metrics_log_request(&metrics, "start_handled");
        return;
    }

    if (is_non_command_message) {
        int64_t now_us = esp_timer_get_time();
        uint64_t since_last_ms = 0;

        if (s_last_non_command_response_us > 0 && now_us > s_last_non_command_response_us) {
            since_last_ms = (uint64_t)(now_us - s_last_non_command_response_us) / 1000ULL;
        }

        if (s_last_non_command_text[0] != '\0' &&
            strcmp(user_message, s_last_non_command_text) == 0 &&
            s_last_non_command_response_us > 0 &&
            since_last_ms < MESSAGE_REPLAY_COOLDOWN_MS) {
            ESP_LOGW(TAG, "Suppressing repeated message replay (%" PRIu64 "ms since last response)",
                     since_last_ms);
            metrics_log_request(&metrics, "replay_suppressed");
            return;
        }
    }

    // Get tools
    int tool_count;
    const tool_def_t *tools = tools_get_all(&tool_count);

    // Add user message to history
    history_add("user", user_message, false, false, NULL, NULL);

    int rounds = 0;
    bool done = false;

    while (!done && rounds < MAX_TOOL_ROUNDS) {
        rounds++;
        metrics.rounds = rounds;

        // Build request JSON (user message already in history)
        char *request = json_build_request(
            build_system_prompt(),
            s_history,
            s_history_len,
            NULL,  // User message already in history
            tools,
            tool_count
        );

        if (!request) {
            ESP_LOGE(TAG, "Failed to build request JSON");
            history_rollback_to(history_turn_start, "request build failed");
            send_response("Error: Failed to build request");
            metrics_log_request(&metrics, "request_build_error");
            return;
        }

        ESP_LOGI(TAG, "Request: %d bytes", (int)strlen(request));

        // Check rate limit before making request
        char rate_reason[128];
        if (!ratelimit_check(rate_reason, sizeof(rate_reason))) {
            free(request);
            history_rollback_to(history_turn_start, "rate limited");
            send_response(rate_reason);
            metrics_log_request(&metrics, "rate_limited");
            return;
        }

        // Send to LLM with retry
        esp_err_t err = ESP_FAIL;
        int retry_delay_ms = LLM_RETRY_BASE_MS;
        int64_t retry_window_started_us = esp_timer_get_time();

        for (int retry = 0; retry < LLM_MAX_RETRIES; retry++) {
            uint32_t retry_elapsed_ms = us_to_ms_u32(elapsed_us_since(retry_window_started_us));
            if (retry > 0 && retry_elapsed_ms >= LLM_RETRY_BUDGET_MS) {
                ESP_LOGW(TAG,
                         "LLM retry budget exhausted before attempt %d/%d (%" PRIu32 "ms/%dms)",
                         retry + 1, LLM_MAX_RETRIES, retry_elapsed_ms, LLM_RETRY_BUDGET_MS);
                break;
            }

            int64_t llm_started_us = esp_timer_get_time();
            err = llm_request(request, s_response_buf, sizeof(s_response_buf));
            metrics.llm_us_total += elapsed_us_since(llm_started_us);
            metrics.llm_calls++;
            if (err == ESP_OK) {
                break;
            }

            if (retry == LLM_MAX_RETRIES - 1) {
                break;
            }

            retry_elapsed_ms = us_to_ms_u32(elapsed_us_since(retry_window_started_us));
            if (retry_elapsed_ms >= LLM_RETRY_BUDGET_MS) {
                ESP_LOGW(TAG,
                         "LLM retry budget exhausted after attempt %d/%d (%" PRIu32 "ms/%dms)",
                         retry + 1, LLM_MAX_RETRIES, retry_elapsed_ms, LLM_RETRY_BUDGET_MS);
                break;
            }

            uint32_t remaining_budget_ms = (uint32_t)(LLM_RETRY_BUDGET_MS - retry_elapsed_ms);
            int delay_ms = retry_delay_ms;
            if ((uint32_t)delay_ms > remaining_budget_ms) {
                delay_ms = (int)remaining_budget_ms;
            }

            if (delay_ms <= 0) {
                ESP_LOGW(TAG,
                         "LLM retry budget left no delay before next attempt (%" PRIu32 "ms/%dms)",
                         retry_elapsed_ms, LLM_RETRY_BUDGET_MS);
                break;
            }

            ESP_LOGW(TAG,
                     "LLM request failed (attempt %d/%d), retrying in %dms (budget %" PRIu32 "/%dms)",
                     retry + 1, LLM_MAX_RETRIES, delay_ms, retry_elapsed_ms, LLM_RETRY_BUDGET_MS);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));

            // Exponential backoff
            retry_delay_ms *= 2;
            if (retry_delay_ms > LLM_RETRY_MAX_MS) {
                retry_delay_ms = LLM_RETRY_MAX_MS;
            }
        }

        free(request);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LLM request failed after %d retries", LLM_MAX_RETRIES);
            history_rollback_to(history_turn_start, "llm request failed");
            send_response("Error: Failed to contact LLM API after retries");
            metrics_log_request(&metrics, "llm_error");
            return;
        }

        // Record successful request for rate limiting
        ratelimit_record_request();

        // Parse response
        char text_out[MAX_MESSAGE_LEN] = {0};
        char tool_name[32] = {0};
        char tool_id[64] = {0};
        cJSON *tool_input = NULL;

        if (!json_parse_response(s_response_buf, text_out, sizeof(text_out),
                                  tool_name, sizeof(tool_name),
                                  tool_id, sizeof(tool_id),
                                  &tool_input)) {
            ESP_LOGE(TAG, "Failed to parse response");
            history_rollback_to(history_turn_start, "llm response parse failed");
            send_response("Error: Failed to parse LLM response");
            json_free_parsed_response();
            metrics_log_request(&metrics, "parse_error");
            return;
        }

        // Check if it's a tool use
        if (tool_name[0] != '\0' && tool_input) {
            ESP_LOGI(TAG, "Tool call: %s (round %d)", tool_name, rounds);

            // Store the tool_input as JSON string for history
            char *input_str = cJSON_PrintUnformatted(tool_input);

            // Add tool_use to history
            history_add("assistant", input_str ? input_str : "{}",
                        true, false, tool_id, tool_name);
            free(input_str);

            // Check if it's a user-defined tool
            const user_tool_t *user_tool = user_tools_find(tool_name);
            metrics.tool_calls++;
            if (user_tool) {
                // User tool: return the action as "instruction" for Claude to execute
                snprintf(s_tool_result_buf, sizeof(s_tool_result_buf),
                         "Execute this action now: %s", user_tool->action);
                ESP_LOGI(TAG, "User tool '%s' action: %s", tool_name, user_tool->action);
            } else {
                // Built-in tool: execute directly
                int64_t tool_started_us = esp_timer_get_time();
                bool tool_ok = tools_execute(tool_name, tool_input,
                                             s_tool_result_buf, sizeof(s_tool_result_buf));
                metrics.tool_us_total += elapsed_us_since(tool_started_us);

                // Keep runtime persona state aligned when persona tools run via LLM.
                if (tool_ok && strcmp(tool_name, "set_persona") == 0) {
                    cJSON *persona_json = cJSON_GetObjectItem(tool_input, "persona");
                    agent_persona_t parsed_persona = AGENT_PERSONA_NEUTRAL;
                    if (persona_json && cJSON_IsString(persona_json) &&
                        parse_persona_name(persona_json->valuestring, &parsed_persona)) {
                        s_persona = parsed_persona;
                    }
                } else if (tool_ok && strcmp(tool_name, "reset_persona") == 0) {
                    s_persona = AGENT_PERSONA_NEUTRAL;
                }

                ESP_LOGI(TAG, "Tool result: %s", s_tool_result_buf);
            }

            // Add tool_result to history
            history_add("user", s_tool_result_buf, false, true, tool_id, NULL);

            json_free_parsed_response();
            // Continue loop to let Claude see the result
        } else {
            // Text response - we're done
            if (text_out[0] != '\0') {
                history_add("assistant", text_out, false, false, NULL, NULL);
                send_response(text_out);
            } else {
                history_add("assistant", "(No response from Claude)", false, false, NULL, NULL);
                send_response("(No response from Claude)");
            }
            json_free_parsed_response();
            done = true;
        }
    }

    if (!done) {
        ESP_LOGW(TAG, "Max tool rounds reached");
        history_add("assistant", "(Reached max tool iterations)", false, false, NULL, NULL);
        send_response("(Reached max tool iterations)");
        metrics_log_request(&metrics, "max_rounds");
        return;
    }

    if (is_non_command_message) {
        strncpy(s_last_non_command_text, user_message, sizeof(s_last_non_command_text) - 1);
        s_last_non_command_text[sizeof(s_last_non_command_text) - 1] = '\0';
        s_last_non_command_response_us = esp_timer_get_time();
    }

    metrics_log_request(&metrics, "success");
}

#ifdef TEST_BUILD
void agent_test_reset(void)
{
    memset(s_history, 0, sizeof(s_history));
    s_history_len = 0;
    memset(s_response_buf, 0, sizeof(s_response_buf));
    memset(s_tool_result_buf, 0, sizeof(s_tool_result_buf));
    s_channel_output_queue = NULL;
    s_telegram_output_queue = NULL;
    s_last_start_response_us = 0;
    s_last_non_command_response_us = 0;
    memset(s_last_non_command_text, 0, sizeof(s_last_non_command_text));
    s_messages_paused = false;
    memset(s_test_persona_value, 0, sizeof(s_test_persona_value));
    load_persona_from_store();
}

void agent_test_set_queues(QueueHandle_t channel_output_queue,
                           QueueHandle_t telegram_output_queue)
{
    s_channel_output_queue = channel_output_queue;
    s_telegram_output_queue = telegram_output_queue;
}

void agent_test_process_message(const char *user_message)
{
    process_message(user_message);
}
#endif

// Agent task
static void agent_task(void *arg)
{
    (void)arg;
    channel_msg_t msg;

    ESP_LOGI(TAG, "Agent task started");

    while (1) {
        if (xQueueReceive(s_input_queue, &msg, portMAX_DELAY) == pdTRUE) {
            process_message(msg.text);
        }
    }
}

esp_err_t agent_start(QueueHandle_t input_queue,
                      QueueHandle_t channel_output_queue,
                      QueueHandle_t telegram_output_queue)
{
    if (!input_queue || !channel_output_queue) {
        ESP_LOGE(TAG, "Invalid queues for agent startup");
        return ESP_ERR_INVALID_ARG;
    }

    s_input_queue = input_queue;
    s_channel_output_queue = channel_output_queue;
    s_telegram_output_queue = telegram_output_queue;
    load_persona_from_store();

    if (xTaskCreate(agent_task, "agent", AGENT_TASK_STACK_SIZE, NULL,
                    AGENT_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create agent task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Agent started");
    return ESP_OK;
}
