/*
 * Host tests for tool memory key policy.
 */

#include <stdio.h>

#include "memory_keys.h"
#include "nvs_keys.h"

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while(0)

TEST(user_key_prefix)
{
    ASSERT(memory_keys_is_user_key("u_name"));
    ASSERT(memory_keys_is_user_key("u_temp1"));
    ASSERT(!memory_keys_is_user_key("name"));
    ASSERT(!memory_keys_is_user_key("wifi_ssid"));
    ASSERT(!memory_keys_is_user_key(""));
    ASSERT(!memory_keys_is_user_key(NULL));
    return 0;
}

TEST(sensitive_exact_keys)
{
    ASSERT(memory_keys_is_sensitive(NVS_KEY_API_KEY));
    ASSERT(memory_keys_is_sensitive(NVS_KEY_TG_TOKEN));
    ASSERT(memory_keys_is_sensitive(NVS_KEY_TG_CHAT_ID));
    ASSERT(memory_keys_is_sensitive(NVS_KEY_WIFI_PASS));
    ASSERT(memory_keys_is_sensitive(NVS_KEY_LLM_BACKEND));
    ASSERT(memory_keys_is_sensitive(NVS_KEY_LLM_MODEL));
    ASSERT(memory_keys_is_sensitive(NVS_KEY_WIFI_SSID));

    ASSERT(!memory_keys_is_sensitive("u_name"));
    ASSERT(!memory_keys_is_sensitive("u_api_key"));
    ASSERT(!memory_keys_is_sensitive("nickname"));
    ASSERT(!memory_keys_is_sensitive(NULL));
    return 0;
}

int test_memory_keys_all(void)
{
    int failures = 0;

    printf("\nMemory Key Policy Tests:\n");

    printf("  user_key_prefix... ");
    if (test_user_key_prefix() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  sensitive_exact_keys... ");
    if (test_sensitive_exact_keys() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
