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

static int recv_telegram_msg(QueueHandle_t queue, telegram_msg_t *out)
{
    if (!out) {
        return 0;
    }
    if (xQueueReceive(queue, out, 0) != pdTRUE) {
        return 0;
    }
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

TEST(diag_command_bypasses_llm_and_uses_tool)
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

    agent_test_process_message("/diag memory verbose");
    ASSERT(mock_llm_request_count() == 0);
    ASSERT(mock_tools_execute_calls() == 1);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "mock tool executed");
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "mock tool executed");

    // /diag should remain available while paused.
    agent_test_process_message("/stop");
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 1);
    agent_test_process_message("/diag all");
    ASSERT(mock_llm_request_count() == 0);
    ASSERT(mock_tools_execute_calls() == 2);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "mock tool executed");
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "mock tool executed");

    vQueueDelete(channel_q);
    vQueueDelete(telegram_q);
    return 0;
}

TEST(diag_command_rejects_invalid_args)
{
    QueueHandle_t channel_q;
    char text[CHANNEL_TX_BUF_SIZE];

    reset_state();

    channel_q = xQueueCreate(2, sizeof(channel_output_msg_t));
    ASSERT(channel_q != NULL);
    agent_test_set_queues(channel_q, NULL);

    agent_test_process_message("/diag bananas");
    ASSERT(mock_llm_request_count() == 0);
    ASSERT(mock_tools_execute_calls() == 0);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT(strstr(text, "unknown /diag argument") != NULL);

    vQueueDelete(channel_q);
    return 0;
}

TEST(persona_phrases_route_through_llm)
{
    QueueHandle_t channel_q;
    char text[CHANNEL_TX_BUF_SIZE];
    const char *reply_one =
        "{\"content\":[{\"type\":\"text\",\"text\":\"handled by llm\"}],\"stop_reason\":\"end_turn\"}";
    const char *reply_two =
        "{\"content\":[{\"type\":\"text\",\"text\":\"through llm again\"}],\"stop_reason\":\"end_turn\"}";
    const char *last_request = NULL;

    reset_state();

    channel_q = xQueueCreate(4, sizeof(channel_output_msg_t));
    ASSERT(channel_q != NULL);
    agent_test_set_queues(channel_q, NULL);

    ASSERT(mock_llm_push_result(ESP_OK, reply_one));
    agent_test_process_message("set persona witty");
    ASSERT(mock_llm_request_count() == 1);
    ASSERT(mock_tools_execute_calls() == 0);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "handled by llm");

    last_request = mock_llm_last_request_json();
    ASSERT(last_request != NULL);
    ASSERT(strstr(last_request, "set persona witty") != NULL);

    ASSERT(mock_llm_push_result(ESP_OK, reply_two));
    agent_test_process_message("show persona");
    ASSERT(mock_llm_request_count() == 2);
    ASSERT(mock_tools_execute_calls() == 0);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "through llm again");

    last_request = mock_llm_last_request_json();
    ASSERT(last_request != NULL);
    ASSERT(strstr(last_request, "show persona") != NULL);

    vQueueDelete(channel_q);
    return 0;
}

TEST(persona_can_change_via_llm_tool_call)
{
    QueueHandle_t channel_q;
    char text[CHANNEL_TX_BUF_SIZE];
    const char *tool_call =
        "{\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_persona_1\","
        "\"name\":\"set_persona\",\"input\":{\"persona\":\"friendly\"}}],"
        "\"stop_reason\":\"tool_use\"}";
    const char *final_text =
        "{\"content\":[{\"type\":\"text\",\"text\":\"persona changed\"}],\"stop_reason\":\"end_turn\"}";
    const char *last_request = NULL;

    reset_state();

    channel_q = xQueueCreate(4, sizeof(channel_output_msg_t));
    ASSERT(channel_q != NULL);
    agent_test_set_queues(channel_q, NULL);

    ASSERT(mock_llm_push_result(ESP_OK, tool_call));
    ASSERT(mock_llm_push_result(ESP_OK, final_text));

    agent_test_process_message("please switch your personality to friendly");

    ASSERT(mock_llm_request_count() == 2);
    ASSERT(mock_tools_execute_calls() == 1);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "persona changed");

    last_request = mock_llm_last_request_json();
    ASSERT(last_request != NULL);
    ASSERT(strstr(last_request, "Device target is") != NULL);
    ASSERT(strstr(last_request, "Persona mode is 'friendly'") != NULL);

    vQueueDelete(channel_q);
    return 0;
}

TEST(cron_trigger_blocks_cron_set_tool_call)
{
    QueueHandle_t channel_q;
    char text[CHANNEL_TX_BUF_SIZE];
    const char *tool_call =
        "{\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_cron_1\","
        "\"name\":\"cron_set\",\"input\":{\"type\":\"once\",\"delay_minutes\":1,"
        "\"action\":\"arcade_power state=1\"}}],\"stop_reason\":\"tool_use\"}";
    const char *final_text =
        "{\"content\":[{\"type\":\"text\",\"text\":\"running scheduled action now\"}],"
        "\"stop_reason\":\"end_turn\"}";
    const char *last_request = NULL;

    reset_state();

    channel_q = xQueueCreate(4, sizeof(channel_output_msg_t));
    ASSERT(channel_q != NULL);
    agent_test_set_queues(channel_q, NULL);

    ASSERT(mock_llm_push_result(ESP_OK, tool_call));
    ASSERT(mock_llm_push_result(ESP_OK, final_text));

    agent_test_process_message("[CRON 1] arcade_power state=1");

    ASSERT(mock_llm_request_count() == 2);
    ASSERT(mock_tools_execute_calls() == 0);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "running scheduled action now");

    last_request = mock_llm_last_request_json();
    ASSERT(last_request != NULL);
    ASSERT(strstr(last_request, "cron_set is not allowed during scheduled task execution") != NULL);

    vQueueDelete(channel_q);
    return 0;
}

TEST(repeated_non_command_is_suppressed)
{
    QueueHandle_t channel_q;
    QueueHandle_t telegram_q;
    char text[TELEGRAM_MAX_MSG_LEN];
    const char *success =
        "{\"content\":[{\"type\":\"text\",\"text\":\"hi there\"}],\"stop_reason\":\"end_turn\"}";

    reset_state();

    channel_q = xQueueCreate(4, sizeof(channel_output_msg_t));
    telegram_q = xQueueCreate(4, sizeof(telegram_msg_t));
    ASSERT(channel_q != NULL);
    ASSERT(telegram_q != NULL);
    agent_test_set_queues(channel_q, telegram_q);

    ASSERT(mock_llm_push_result(ESP_OK, success));
    agent_test_process_message("What can you do");
    ASSERT(mock_llm_request_count() == 1);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "hi there");
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "hi there");

    // Immediate repeat should be dropped and not trigger another LLM call.
    agent_test_process_message("What can you do");
    ASSERT(mock_llm_request_count() == 1);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 0);
    ASSERT(recv_telegram_text(telegram_q, text, sizeof(text)) == 0);

    vQueueDelete(channel_q);
    vQueueDelete(telegram_q);
    return 0;
}

TEST(repeated_non_command_not_suppressed_after_failure)
{
    QueueHandle_t channel_q;
    char text[CHANNEL_TX_BUF_SIZE];
    const char *success =
        "{\"content\":[{\"type\":\"text\",\"text\":\"recovered\"}],\"stop_reason\":\"end_turn\"}";

    reset_state();

    channel_q = xQueueCreate(4, sizeof(channel_output_msg_t));
    ASSERT(channel_q != NULL);
    agent_test_set_queues(channel_q, NULL);

    ASSERT(mock_llm_push_result(ESP_FAIL, NULL));
    ASSERT(mock_llm_push_result(ESP_FAIL, NULL));
    ASSERT(mock_llm_push_result(ESP_FAIL, NULL));
    ASSERT(mock_llm_push_result(ESP_OK, success));

    agent_test_process_message("retry this");
    ASSERT(mock_llm_request_count() == 3);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "Error: Failed to contact LLM API after retries");
    ASSERT(mock_ratelimit_record_count() == 0);

    // The same message should be allowed immediately after a failed turn.
    agent_test_process_message("retry this");
    ASSERT(mock_llm_request_count() == 4);
    ASSERT(recv_channel_text(channel_q, text, sizeof(text)) == 1);
    ASSERT_STR_EQ(text, "recovered");
    ASSERT(mock_ratelimit_record_count() == 1);

    vQueueDelete(channel_q);
    return 0;
}

TEST(telegram_response_preserves_reply_chat_id)
{
    QueueHandle_t channel_q;
    QueueHandle_t telegram_q;
    telegram_msg_t msg;
    const char *success =
        "{\"content\":[{\"type\":\"text\",\"text\":\"targeted reply\"}],\"stop_reason\":\"end_turn\"}";

    reset_state();

    channel_q = xQueueCreate(2, sizeof(channel_output_msg_t));
    telegram_q = xQueueCreate(2, sizeof(telegram_msg_t));
    ASSERT(channel_q != NULL);
    ASSERT(telegram_q != NULL);
    agent_test_set_queues(channel_q, telegram_q);

    ASSERT(mock_llm_push_result(ESP_OK, success));
    agent_test_process_message_for_chat("hello", -100222333444LL);

    ASSERT(recv_telegram_msg(telegram_q, &msg) == 1);
    ASSERT_STR_EQ(msg.text, "targeted reply");
    ASSERT(msg.chat_id == -100222333444LL);

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

    printf("  diag_command_bypasses_llm_and_uses_tool... ");
    if (test_diag_command_bypasses_llm_and_uses_tool() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  diag_command_rejects_invalid_args... ");
    if (test_diag_command_rejects_invalid_args() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  persona_phrases_route_through_llm... ");
    if (test_persona_phrases_route_through_llm() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  persona_can_change_via_llm_tool_call... ");
    if (test_persona_can_change_via_llm_tool_call() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  cron_trigger_blocks_cron_set_tool_call... ");
    if (test_cron_trigger_blocks_cron_set_tool_call() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  repeated_non_command_is_suppressed... ");
    if (test_repeated_non_command_is_suppressed() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  repeated_non_command_not_suppressed_after_failure... ");
    if (test_repeated_non_command_not_suppressed_after_failure() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  telegram_response_preserves_reply_chat_id... ");
    if (test_telegram_response_preserves_reply_chat_id() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
