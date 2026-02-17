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
    channel_msg_t msg;
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

    channel_q = xQueueCreate(4, sizeof(channel_msg_t));
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
    char text[CHANNEL_RX_BUF_SIZE];

    reset_state();

    channel_q = xQueueCreate(2, sizeof(channel_msg_t));
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
    char text[CHANNEL_RX_BUF_SIZE];

    reset_state();

    channel_q = xQueueCreate(2, sizeof(channel_msg_t));
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

    return failures;
}
