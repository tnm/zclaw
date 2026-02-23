/*
 * Host tests for backend-specific Telegram poll timeout policy.
 */

#include <stdio.h>

#include "telegram_poll_policy.h"

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while(0)

TEST(default_backends_keep_standard_timeout)
{
    ASSERT(telegram_poll_timeout_for_backend(LLM_BACKEND_ANTHROPIC) == TELEGRAM_POLL_TIMEOUT);
    ASSERT(telegram_poll_timeout_for_backend(LLM_BACKEND_OPENAI) == TELEGRAM_POLL_TIMEOUT);
    return 0;
}

TEST(openrouter_uses_shorter_timeout)
{
    ASSERT(telegram_poll_timeout_for_backend(LLM_BACKEND_OPENROUTER) ==
           TELEGRAM_POLL_TIMEOUT_OPENROUTER);
    return 0;
}

TEST(unknown_backend_falls_back_to_standard_timeout)
{
    ASSERT(telegram_poll_timeout_for_backend((llm_backend_t)999) == TELEGRAM_POLL_TIMEOUT);
    return 0;
}

int test_telegram_poll_policy_all(void)
{
    int failures = 0;

    printf("\nTelegram Poll Policy Tests:\n");

    printf("  default_backends_keep_standard_timeout... ");
    if (test_default_backends_keep_standard_timeout() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  openrouter_uses_shorter_timeout... ");
    if (test_openrouter_uses_shorter_timeout() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  unknown_backend_falls_back_to_standard_timeout... ");
    if (test_unknown_backend_falls_back_to_standard_timeout() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
