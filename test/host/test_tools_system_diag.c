/*
 * Host tests for system diagnostics tool output formatting.
 */

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "tools_handlers.h"
#include "mock_memory.h"

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while(0)
#define ASSERT_STR_CONTAINS(haystack, needle) do { \
    if (strstr((haystack), (needle)) == NULL) { \
        printf("  FAIL: expected substring '%s' in '%s' (line %d)\n", (needle), (haystack), __LINE__); \
        return 1; \
    } \
} while(0)

TEST(diag_runtime_verbose_uses_seconds_detail_not_microseconds_label)
{
    cJSON *input = cJSON_Parse("{\"scope\":\"runtime\",\"verbose\":true}");
    char result[512] = {0};

    mock_memory_reset();
    mock_memory_set_kv("boot_count", "7");

    ASSERT(input != NULL);
    ASSERT(tools_get_diagnostics_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "Runtime diagnostics:");
    ASSERT_STR_CONTAINS(result, "- Uptime: ");
    ASSERT_STR_CONTAINS(result, "s)");
    ASSERT(strstr(result, " us)") == NULL);
    ASSERT_STR_CONTAINS(result, "- Boot count: 7");

    cJSON_Delete(input);
    return 0;
}

TEST(diag_all_verbose_uses_seconds_detail_not_microseconds_label)
{
    cJSON *input = cJSON_Parse("{\"scope\":\"all\",\"verbose\":true}");
    char result[1024] = {0};

    mock_memory_reset();
    mock_memory_set_kv("boot_count", "3");

    ASSERT(input != NULL);
    ASSERT(tools_get_diagnostics_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "Diagnostics:");
    ASSERT_STR_CONTAINS(result, "- Uptime: ");
    ASSERT_STR_CONTAINS(result, "s)\n");
    ASSERT(strstr(result, " us)") == NULL);
    ASSERT_STR_CONTAINS(result, "- Boot count: 3");

    cJSON_Delete(input);
    return 0;
}

int test_tools_system_diag_all(void)
{
    int failures = 0;

    printf("\nSystem Diagnostics Tests:\n");

    printf("  diag_runtime_verbose_uses_seconds_detail_not_microseconds_label... ");
    if (test_diag_runtime_verbose_uses_seconds_detail_not_microseconds_label() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  diag_all_verbose_uses_seconds_detail_not_microseconds_label... ");
    if (test_diag_all_verbose_uses_seconds_detail_not_microseconds_label() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
