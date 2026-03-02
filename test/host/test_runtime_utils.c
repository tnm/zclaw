/*
 * Host tests for shared runtime utility modules
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "config.h"
#include "cron_utils.h"
#include "security.h"
#include "text_buffer.h"
#include "boot_guard.h"
#include "mock_memory.h"

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while(0)

TEST(security_sensitive_key_detection)
{
    ASSERT(security_key_is_sensitive("wifi_pass"));
    ASSERT(security_key_is_sensitive("tg_token"));
    ASSERT(security_key_is_sensitive("api_key"));
    ASSERT(!security_key_is_sensitive("wifi_ssid"));
    ASSERT(!security_key_is_sensitive("nickname"));
    return 0;
}

TEST(cron_validation)
{
    ASSERT(cron_validate_periodic_interval(1));
    ASSERT(cron_validate_periodic_interval(1440));
    ASSERT(!cron_validate_periodic_interval(0));
    ASSERT(!cron_validate_periodic_interval(-1));
    ASSERT(!cron_validate_periodic_interval(1441));

    ASSERT(cron_validate_daily_time(0, 0));
    ASSERT(cron_validate_daily_time(23, 59));
    ASSERT(!cron_validate_daily_time(24, 0));
    ASSERT(!cron_validate_daily_time(-1, 0));
    ASSERT(!cron_validate_daily_time(12, 60));
    return 0;
}

TEST(cron_next_entry_id_allocation)
{
    uint8_t ids1[] = {1, 2, 3};
    ASSERT(cron_next_entry_id(ids1, 3) == 4);

    uint8_t ids2[] = {1, 2, 255};
    ASSERT(cron_next_entry_id(ids2, 3) == 3);

    uint8_t full[255];
    for (int i = 0; i < 255; i++) {
        full[i] = (uint8_t)(i + 1);
    }
    ASSERT(cron_next_entry_id(full, 255) == 0);
    return 0;
}

TEST(text_buffer_append_and_truncate)
{
    char buf[8];
    size_t len = 0;
    memset(buf, 0, sizeof(buf));

    ASSERT(text_buffer_append(buf, &len, sizeof(buf), "hello", 5));
    ASSERT(len == 5);
    ASSERT(strcmp(buf, "hello") == 0);

    ASSERT(text_buffer_append(buf, &len, sizeof(buf), "!!", 2));
    ASSERT(len == 7);
    ASSERT(strcmp(buf, "hello!!") == 0);

    ASSERT(!text_buffer_append(buf, &len, sizeof(buf), "x", 1));
    ASSERT(len == 7);
    ASSERT(strcmp(buf, "hello!!") == 0);
    return 0;
}

TEST(boot_guard_threshold)
{
    ASSERT(boot_guard_next_count(0) == 1);
    ASSERT(boot_guard_next_count(2) == 3);
    ASSERT(boot_guard_next_count(-10) == 1);

    ASSERT(!boot_guard_should_enter_safe_mode(0, 3));
    ASSERT(!boot_guard_should_enter_safe_mode(1, 3));
    ASSERT(boot_guard_should_enter_safe_mode(2, 3));
    ASSERT(boot_guard_should_enter_safe_mode(3, 3));
    ASSERT(!boot_guard_should_enter_safe_mode(0, 0));
    return 0;
}

TEST(boot_guard_persistence)
{
    mock_memory_reset();
    ASSERT(boot_guard_get_persisted_count() == 0);

    ASSERT(boot_guard_set_persisted_count(3) == ESP_OK);
    ASSERT(boot_guard_get_persisted_count() == 3);

    mock_memory_fail_next_set(ESP_FAIL);
    ASSERT(boot_guard_set_persisted_count(0) == ESP_FAIL);
    ASSERT(boot_guard_get_persisted_count() == 3);
    return 0;
}

TEST(boot_ok_task_stack_config)
{
    ASSERT(BOOT_OK_TASK_STACK_SIZE >= 4096);
    return 0;
}

TEST(telegram_channel_capacity_config)
{
    ASSERT(TELEGRAM_MAX_MSG_LEN > CHANNEL_RX_BUF_SIZE);
    ASSERT(TELEGRAM_OUTPUT_QUEUE_LENGTH > 0);
    return 0;
}

int test_runtime_utils_all(void)
{
    int failures = 0;

    printf("\nRuntime Utility Tests:\n");

    printf("  security_sensitive_key_detection... ");
    if (test_security_sensitive_key_detection() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  cron_validation... ");
    if (test_cron_validation() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  cron_next_entry_id_allocation... ");
    if (test_cron_next_entry_id_allocation() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  text_buffer_append_and_truncate... ");
    if (test_text_buffer_append_and_truncate() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  boot_guard_threshold... ");
    if (test_boot_guard_threshold() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  boot_guard_persistence... ");
    if (test_boot_guard_persistence() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  boot_ok_task_stack_config... ");
    if (test_boot_ok_task_stack_config() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  telegram_channel_capacity_config... ");
    if (test_telegram_channel_capacity_config() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
