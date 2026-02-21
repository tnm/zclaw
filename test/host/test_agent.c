/*
 * Host tests for agent retry logic and output queue fanout.
 */

#include <stdio.h>
#include <string.h>

#include "agent.h"
#include "config.h"
#include "messages.h"
#include "mock_freertos.h"
#include "mock_llm.h"
#include "mock_ratelimit.h"
#include "mock_tools.h"
#include "freertos/queue.h"

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while(0)
#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("  FAIL: '%s' != '%s' (line %d)\n", (a), (b), __LINE__); \
        return 1; \
    } \
} while(0)

static int recv_channel_text(QueueHandle_t queue, char *out, size_t out_len)
{
    channel_output_msg_t msg;
    if (xQueueReceive(queue, &msg, 0) != pdTRUE) {
        return 0;
    }
    snprintf(out, out_len, "%s", msg.text);
    return 1;
}

static int recv_telegram_text(QueueHandle_t queue, char *out, size_t out_len)
{
    telegram_msg_t msg;
    if (xQueueReceive(queue, &msg, 0) != pdTRUE) {
        return 0;
    }
    snprintf(out, out_len, "%s", msg.text);
    return 1;
}

static void reset_state(void)
{
    mock_freertos_reset();
    mock_llm_reset();
    mock_ratelimit_reset();
    mock_tools_reset();
    mock_llm_set_backend(LLM_BACKEND_ANTHROPIC, "mock-anthropic");
    agent_test_reset();
}

TEST(retries_with_backoff_and_fanout)
{
    QueueHandle_t channel_q;
    QueueHandle_t telegram_q;
    char text[TELEGRAM_MAX_MSG_LEN];
    const char *success =
        "{\"content\":[{\"type\":\"text\",\"text\":\"retry succeeded\"}],\"stop_reason\":\"end_turn\"}";

    reset_state();

    channel_q = xQueueCreate(4, sizeof(channel_output_msg_t));
    telegram_q = xQueueCreate(4, sizeof(telegram_msg_t));
    ASSERT(channel_q != NULL);
    ASSERT(telegram_q != NULL);
    agent_test_set_queues(channel_q, telegram_q);

    ASSERT(mock_llm_push_result(ESP_FAIL, NULL));
    ASSERT(mock_llm_push_result(ESP_FAIL, NULL));
    ASSERT(mock_llm_push_result(ESP_OK, success));

    agent_test_process_message("hello");

    ASSERT(mock_llm_request_count() == 3);
    ASSERT(mock_freertos_delay_count() == 2);
    ASSERT(mock_freertos_delay_at(0) == pdMS_TO_TICKS(2000));
    ASSERT(mock_freertos_delay_at(1) == pdMS_TO_TICKS(4000));
    ASSERT(mock_ratelimit_record_count() == 1);

    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "retry succeeded");
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "retry succeeded");

    vQueueDelete(channel_q);
    vQueueDelete(telegram_q);
    return 0;
}

TEST(rate_limit_short_circuit)
{
    QueueHandle_t channel_q;
    char text[CHANNEL_TX_BUF_SIZE];

    reset_state();

    channel_q = xQueueCreate(2, sizeof(channel_output_msg_t));
    ASSERT(channel_q != NULL);
    agent_test_set_queues(channel_q, NULL);
    mock_ratelimit_set_allow(false, "Rate limit hit");

    agent_test_process_message("hello");

    ASSERT(mock_llm_request_count() == 0);
    ASSERT(mock_ratelimit_record_count() == 0);
    ASSERT(mock_freertos_delay_count() == 0);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "Rate limit hit");

    vQueueDelete(channel_q);
    return 0;
}

TEST(fails_after_max_retries_without_extra_sleep)
{
    QueueHandle_t channel_q;
    char text[CHANNEL_TX_BUF_SIZE];

    reset_state();

    channel_q = xQueueCreate(2, sizeof(channel_output_msg_t));
    ASSERT(channel_q != NULL);
    agent_test_set_queues(channel_q, NULL);

    ASSERT(mock_llm_push_result(ESP_FAIL, NULL));
    ASSERT(mock_llm_push_result(ESP_FAIL, NULL));
    ASSERT(mock_llm_push_result(ESP_FAIL, NULL));

    agent_test_process_message("hello");

    ASSERT(mock_llm_request_count() == 3);
    ASSERT(mock_freertos_delay_count() == 2);
    ASSERT(mock_freertos_delay_at(0) == pdMS_TO_TICKS(2000));
    ASSERT(mock_freertos_delay_at(1) == pdMS_TO_TICKS(4000));
    ASSERT(mock_ratelimit_record_count() == 0);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "Error: Failed to contact LLM API after retries");

    vQueueDelete(channel_q);
    return 0;
}

TEST(failed_turn_does_not_pollute_followup_prompt)
{
    QueueHandle_t channel_q;
    char text[CHANNEL_TX_BUF_SIZE];
    const char *success =
        "{\"content\":[{\"type\":\"text\",\"text\":\"fresh response\"}],\"stop_reason\":\"end_turn\"}";
    const char *last_request = NULL;

    reset_state();

    channel_q = xQueueCreate(4, sizeof(channel_output_msg_t));
    ASSERT(channel_q != NULL);
    agent_test_set_queues(channel_q, NULL);

    ASSERT(mock_llm_push_result(ESP_FAIL, NULL));
    ASSERT(mock_llm_push_result(ESP_FAIL, NULL));
    ASSERT(mock_llm_push_result(ESP_FAIL, NULL));
    ASSERT(mock_llm_push_result(ESP_OK, success));

    agent_test_process_message("is this really on a tiny board");
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "Error: Failed to contact LLM API after retries");

    agent_test_process_message("hello");
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "fresh response");

    ASSERT(mock_llm_request_count() == 4);
    ASSERT(mock_ratelimit_record_count() == 1);

    last_request = mock_llm_last_request_json();
    ASSERT(last_request != NULL);
    ASSERT(strstr(last_request, "is this really on a tiny board") == NULL);
    ASSERT(strstr(last_request, "hello") != NULL);

    vQueueDelete(channel_q);
    return 0;
}

TEST(channel_output_allows_long_response)
{
    QueueHandle_t channel_q;
    char text[CHANNEL_TX_BUF_SIZE];
    char long_text[801];
    char response_json[1200];

    reset_state();

    memset(long_text, 'x', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    snprintf(response_json, sizeof(response_json),
             "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],\"stop_reason\":\"end_turn\"}",
             long_text);

    channel_q = xQueueCreate(2, sizeof(channel_output_msg_t));
    ASSERT(channel_q != NULL);
    agent_test_set_queues(channel_q, NULL);

    ASSERT(mock_llm_push_result(ESP_OK, response_json));
    agent_test_process_message("long output test");

    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT(strlen(text) == strlen(long_text));
    ASSERT(strcmp(text, long_text) == 0);

    vQueueDelete(channel_q);
    return 0;
}

TEST(start_command_bypasses_llm_and_debounces)
{
    QueueHandle_t channel_q;
    QueueHandle_t telegram_q;
    char text[TELEGRAM_MAX_MSG_LEN];

    reset_state();

    channel_q = xQueueCreate(4, sizeof(channel_output_msg_t));
    telegram_q = xQueueCreate(4, sizeof(telegram_msg_t));
    ASSERT(channel_q != NULL);
    ASSERT(telegram_q != NULL);
    agent_test_set_queues(channel_q, telegram_q);

    agent_test_process_message("/start");

    ASSERT(mock_llm_request_count() == 0);
    ASSERT(mock_ratelimit_record_count() == 0);

    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT(strstr(text, "zclaw online.") != NULL);
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 1);
    ASSERT(strstr(text, "zclaw online.") != NULL);

    // Immediate duplicate should be suppressed to stop burst spam.
    agent_test_process_message("/start");
    ASSERT(mock_llm_request_count() == 0);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 0);
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 0);

    vQueueDelete(channel_q);
    vQueueDelete(telegram_q);
    return 0;
}

TEST(stop_and_resume_pause_message_processing)
{
    QueueHandle_t channel_q;
    QueueHandle_t telegram_q;
    char text[TELEGRAM_MAX_MSG_LEN];
    const char *success =
        "{\"content\":[{\"type\":\"text\",\"text\":\"normal response\"}],\"stop_reason\":\"end_turn\"}";

    reset_state();

    channel_q = xQueueCreate(4, sizeof(channel_output_msg_t));
    telegram_q = xQueueCreate(4, sizeof(telegram_msg_t));
    ASSERT(channel_q != NULL);
    ASSERT(telegram_q != NULL);
    agent_test_set_queues(channel_q, telegram_q);

    agent_test_process_message("/stop");
    ASSERT(mock_llm_request_count() == 0);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT(strstr(text, "zclaw paused.") != NULL);
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 1);
    ASSERT(strstr(text, "/resume") != NULL);

    // While paused, regular messages are ignored and never hit the LLM.
    agent_test_process_message("hello");
    ASSERT(mock_llm_request_count() == 0);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 0);
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 0);

    // /start should also be ignored while paused.
    agent_test_process_message("/start");
    ASSERT(mock_llm_request_count() == 0);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 0);
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 0);

    agent_test_process_message("/resume");
    ASSERT(mock_llm_request_count() == 0);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT(strstr(text, "zclaw resumed.") != NULL);
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 1);
    ASSERT(strstr(text, "/start") != NULL);

    ASSERT(mock_llm_push_result(ESP_OK, success));
    agent_test_process_message("hello");
    ASSERT(mock_llm_request_count() == 1);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "normal response");
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "normal response");

    vQueueDelete(channel_q);
    vQueueDelete(telegram_q);
    return 0;
}

TEST(help_and_settings_commands_bypass_llm)
{
    QueueHandle_t channel_q;
    QueueHandle_t telegram_q;
    char text[TELEGRAM_MAX_MSG_LEN];

    reset_state();

    channel_q = xQueueCreate(4, sizeof(channel_output_msg_t));
    telegram_q = xQueueCreate(4, sizeof(telegram_msg_t));
    ASSERT(channel_q != NULL);
    ASSERT(telegram_q != NULL);
    agent_test_set_queues(channel_q, telegram_q);

    agent_test_process_message("/help");
    ASSERT(mock_llm_request_count() == 0);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT(strstr(text, "zclaw online.") != NULL);
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 1);
    ASSERT(strstr(text, "zclaw online.") != NULL);

    agent_test_process_message("/settings");
    ASSERT(mock_llm_request_count() == 0);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT(strstr(text, "Message intake: active") != NULL);
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 1);
    ASSERT(strstr(text, "Message intake: active") != NULL);

    // /settings should remain available while paused.
    agent_test_process_message("/stop");
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 1);
    agent_test_process_message("/settings");
    ASSERT(mock_llm_request_count() == 0);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT(strstr(text, "Message intake: paused") != NULL);
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 1);
    ASSERT(strstr(text, "Message intake: paused") != NULL);

    vQueueDelete(channel_q);
    vQueueDelete(telegram_q);
    return 0;
}

int test_agent_all(void)
{
    int failures = 0;

    printf("\nAgent Tests:\n");

    printf("  retries_with_backoff_and_fanout... ");
    if (test_retries_with_backoff_and_fanout() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  rate_limit_short_circuit... ");
    if (test_rate_limit_short_circuit() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  fails_after_max_retries_without_extra_sleep... ");
    if (test_fails_after_max_retries_without_extra_sleep() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  failed_turn_does_not_pollute_followup_prompt... ");
    if (test_failed_turn_does_not_pollute_followup_prompt() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  channel_output_allows_long_response... ");
    if (test_channel_output_allows_long_response() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  start_command_bypasses_llm_and_debounces... ");
    if (test_start_command_bypasses_llm_and_debounces() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  stop_and_resume_pause_message_processing... ");
    if (test_stop_and_resume_pause_message_processing() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  help_and_settings_commands_bypass_llm... ");
    if (test_help_and_settings_commands_bypass_llm() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
