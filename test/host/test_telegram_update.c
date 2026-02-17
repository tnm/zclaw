/*
 * Host tests for Telegram update_id parsing helpers.
 */

#include <stdio.h>
#include <stdint.h>

#include "telegram_update.h"

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while(0)

TEST(parse_single_update_id)
{
    int64_t max_id = -1;
    ASSERT(telegram_extract_max_update_id("{\"result\":[{\"update_id\":123}]}", &max_id));
    ASSERT(max_id == 123);
    return 0;
}

TEST(parse_max_across_multiple)
{
    int64_t max_id = -1;
    const char *json = "{\"result\":[{\"update_id\":10},{\"update_id\":999},{\"update_id\":57}]}";
    ASSERT(telegram_extract_max_update_id(json, &max_id));
    ASSERT(max_id == 999);
    return 0;
}

TEST(parse_above_int32)
{
    int64_t max_id = -1;
    const char *json = "{\"result\":[{\"update_id\":2147483648},{\"update_id\":5000000000}]}";
    ASSERT(telegram_extract_max_update_id(json, &max_id));
    ASSERT(max_id == 5000000000LL);
    return 0;
}

TEST(parse_truncated_buffer_recovery)
{
    int64_t max_id = -1;
    const char *buf = "{\"result\":[{\"update_id\":42},{\"update_id\":9876543210},";
    ASSERT(telegram_extract_max_update_id(buf, &max_id));
    ASSERT(max_id == 9876543210LL);
    return 0;
}

TEST(parse_invalid_input)
{
    int64_t max_id = -1;
    ASSERT(!telegram_extract_max_update_id("{\"result\":[]}", &max_id));
    ASSERT(!telegram_extract_max_update_id("{\"update_id\":-1}", &max_id));
    ASSERT(!telegram_extract_max_update_id(NULL, &max_id));
    ASSERT(!telegram_extract_max_update_id("{\"update_id\":1}", NULL));
    return 0;
}

int test_telegram_update_all(void)
{
    int failures = 0;

    printf("\nTelegram Update Parser Tests:\n");

    printf("  parse_single_update_id... ");
    if (test_parse_single_update_id() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  parse_max_across_multiple... ");
    if (test_parse_max_across_multiple() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  parse_above_int32... ");
    if (test_parse_above_int32() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  parse_truncated_buffer_recovery... ");
    if (test_parse_truncated_buffer_recovery() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  parse_invalid_input... ");
    if (test_parse_invalid_input() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
