/*
 * Host tests for GPIO policy helpers (range and allowlist).
 */

#include <stdio.h>
#include <string.h>
#include "tools_handlers.h"

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while(0)

bool tools_gpio_test_pin_is_allowed(int pin, const char *csv, int min_pin, int max_pin);

TEST(range_policy)
{
    ASSERT(!tools_gpio_test_pin_is_allowed(1, "", 2, 10));
    ASSERT(tools_gpio_test_pin_is_allowed(2, "", 2, 10));
    ASSERT(tools_gpio_test_pin_is_allowed(10, "", 2, 10));
    ASSERT(!tools_gpio_test_pin_is_allowed(11, "", 2, 10));
    return 0;
}

TEST(allowlist_policy_non_contiguous)
{
    const char *pins = "1,2,3,4,5,6,7,8,9,43,44";

    ASSERT(tools_gpio_test_pin_is_allowed(1, pins, 2, 10));
    ASSERT(tools_gpio_test_pin_is_allowed(43, pins, 2, 10));
    ASSERT(tools_gpio_test_pin_is_allowed(44, pins, 2, 10));
    ASSERT(!tools_gpio_test_pin_is_allowed(10, pins, 2, 10));
    ASSERT(!tools_gpio_test_pin_is_allowed(42, pins, 2, 10));
    return 0;
}

TEST(allowlist_policy_tolerates_spaces_and_invalid_tokens)
{
    const char *pins = " 1, two, 3 , , 44";

    ASSERT(tools_gpio_test_pin_is_allowed(1, pins, 0, 0));
    ASSERT(tools_gpio_test_pin_is_allowed(3, pins, 0, 0));
    ASSERT(tools_gpio_test_pin_is_allowed(44, pins, 0, 0));
    ASSERT(!tools_gpio_test_pin_is_allowed(2, pins, 0, 0));
    return 0;
}

int test_tools_gpio_policy_all(void)
{
    int failures = 0;

    printf("\nGPIO Policy Tests:\n");

    printf("  range_policy... ");
    if (test_range_policy() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  allowlist_policy_non_contiguous... ");
    if (test_allowlist_policy_non_contiguous() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  allowlist_policy_tolerates_spaces_and_invalid_tokens... ");
    if (test_allowlist_policy_tolerates_spaces_and_invalid_tokens() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
