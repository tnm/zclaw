/*
 * Host tests for Telegram token helper parsing.
 */

#include <stdio.h>
#include <string.h>

#include "telegram_token.h"

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while(0)

TEST(extract_valid_bot_id)
{
    char out[24];
    ASSERT(telegram_extract_bot_id("8291539104:AAGxpPliHXAghCqdmIlQwPMwcrF-4ibBpgk", out, sizeof(out)));
    ASSERT(out[0] == '8');
    ASSERT(strcmp(out, "8291539104") == 0);
    return 0;
}

TEST(reject_missing_colon)
{
    char out[24];
    ASSERT(!telegram_extract_bot_id("8291539104AAGxpPliHXAghCqdmIlQwPMwcrF-4ibBpgk", out, sizeof(out)));
    return 0;
}

TEST(reject_non_numeric_id)
{
    char out[24];
    ASSERT(!telegram_extract_bot_id("bot8291539104:AAGxpPliHXAghCqdmIlQwPMwcrF-4ibBpgk", out, sizeof(out)));
    return 0;
}

TEST(reject_small_output_buffer)
{
    char out[4];
    ASSERT(!telegram_extract_bot_id("8291539104:AAGxpPliHXAghCqdmIlQwPMwcrF-4ibBpgk", out, sizeof(out)));
    return 0;
}

TEST(reject_null_args)
{
    char out[24];
    ASSERT(!telegram_extract_bot_id(NULL, out, sizeof(out)));
    ASSERT(!telegram_extract_bot_id("8291539104:secret", NULL, sizeof(out)));
    ASSERT(!telegram_extract_bot_id("8291539104:secret", out, 0));
    return 0;
}

int test_telegram_token_all(void)
{
    int failures = 0;

    printf("\nTelegram Token Tests:\n");

    printf("  extract_valid_bot_id... ");
    if (test_extract_valid_bot_id() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  reject_missing_colon... ");
    if (test_reject_missing_colon() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  reject_non_numeric_id... ");
    if (test_reject_non_numeric_id() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  reject_small_output_buffer... ");
    if (test_reject_small_output_buffer() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  reject_null_args... ");
    if (test_reject_null_args() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
