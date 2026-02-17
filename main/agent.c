#include "agent.h"
#include "config.h"
#include "llm.h"
#include "tools.h"
#include "user_tools.h"
#include "json_util.h"
#include "messages.h"
#include "ratelimit.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "agent";

// LLM retry configuration
#define LLM_MAX_RETRIES     3
#define LLM_RETRY_BASE_MS   2000
#define LLM_RETRY_MAX_MS    10000

// Queues
static QueueHandle_t s_input_queue;
static QueueHandle_t s_channel_output_queue;
static QueueHandle_t s_telegram_output_queue;

// Conversation history (ring buffer)
static conversation_msg_t s_history[MAX_HISTORY_TURNS * 2];  // Room for user + assistant pairs
static int s_history_len = 0;

// Buffers (static to avoid stack overflow)
static char s_response_buf[LLM_RESPONSE_BUF_SIZE];
static char s_tool_result_buf[TOOL_RESULT_BUF_SIZE];

// Add a message to history
static void history_add(const char *role, const char *content,
                        bool is_tool_use, bool is_tool_result,
                        const char *tool_id, const char *tool_name)
{
    // Shift if full
    if (s_history_len >= MAX_HISTORY_TURNS * 2) {
        memmove(&s_history[0], &s_history[2], (MAX_HISTORY_TURNS * 2 - 2) * sizeof(conversation_msg_t));
        s_history_len -= 2;
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

    channel_msg_t msg;
    strncpy(msg.text, text, CHANNEL_RX_BUF_SIZE - 1);
    msg.text[CHANNEL_RX_BUF_SIZE - 1] = '\0';

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

// Process a single user message
static void process_message(const char *user_message)
{
    ESP_LOGI(TAG, "Processing: %s", user_message);

    // Get tools
    int tool_count;
    const tool_def_t *tools = tools_get_all(&tool_count);

    // Add user message to history
    history_add("user", user_message, false, false, NULL, NULL);

    int rounds = 0;
    bool done = false;

    while (!done && rounds < MAX_TOOL_ROUNDS) {
        rounds++;

        // Build request JSON (user message already in history)
        char *request = json_build_request(
            SYSTEM_PROMPT,
            s_history,
            s_history_len,
            NULL,  // User message already in history
            tools,
            tool_count
        );

        if (!request) {
            ESP_LOGE(TAG, "Failed to build request JSON");
            send_response("Error: Failed to build request");
            return;
        }

        ESP_LOGI(TAG, "Request: %d bytes", (int)strlen(request));

        // Check rate limit before making request
        char rate_reason[128];
        if (!ratelimit_check(rate_reason, sizeof(rate_reason))) {
            free(request);
            send_response(rate_reason);
            return;
        }

        // Send to LLM with retry
        esp_err_t err = ESP_FAIL;
        int retry_delay_ms = LLM_RETRY_BASE_MS;

        for (int retry = 0; retry < LLM_MAX_RETRIES; retry++) {
            err = llm_request(request, s_response_buf, sizeof(s_response_buf));
            if (err == ESP_OK) {
                break;
            }

            if (retry == LLM_MAX_RETRIES - 1) {
                break;
            }

            ESP_LOGW(TAG, "LLM request failed (attempt %d/%d), retrying in %dms",
                     retry + 1, LLM_MAX_RETRIES, retry_delay_ms);
            vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));

            // Exponential backoff
            retry_delay_ms *= 2;
            if (retry_delay_ms > LLM_RETRY_MAX_MS) {
                retry_delay_ms = LLM_RETRY_MAX_MS;
            }
        }

        free(request);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LLM request failed after %d retries", LLM_MAX_RETRIES);
            send_response("Error: Failed to contact LLM API after retries");
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
            send_response("Error: Failed to parse LLM response");
            json_free_parsed_response();
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
            if (user_tool) {
                // User tool: return the action as "instruction" for Claude to execute
                snprintf(s_tool_result_buf, sizeof(s_tool_result_buf),
                         "Execute this action now: %s", user_tool->action);
                ESP_LOGI(TAG, "User tool '%s' action: %s", tool_name, user_tool->action);
            } else {
                // Built-in tool: execute directly
                tools_execute(tool_name, tool_input, s_tool_result_buf, sizeof(s_tool_result_buf));
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
                send_response("(No response from Claude)");
            }
            json_free_parsed_response();
            done = true;
        }
    }

    if (!done) {
        ESP_LOGW(TAG, "Max tool rounds reached");
        send_response("(Reached max tool iterations)");
    }
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

    if (xTaskCreate(agent_task, "agent", AGENT_TASK_STACK_SIZE, NULL,
                    AGENT_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create agent task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Agent started");
    return ESP_OK;
}
