/*
 * Host-based test runner for zclaw
 * Runs on development machine without hardware
 */

#include <stdio.h>
#include <stdlib.h>

// Test declarations
extern int test_json_all(void);
extern int test_tools_parse_all(void);
extern int test_json_util_integration_all(void);
extern int test_runtime_utils_all(void);
extern int test_memory_keys_all(void);
extern int test_telegram_update_all(void);
extern int test_telegram_token_all(void);
extern int test_telegram_chat_ids_all(void);
extern int test_telegram_poll_policy_all(void);
extern int test_agent_all(void);
extern int test_tools_gpio_policy_all(void);
extern int test_llm_auth_all(void);
extern int test_wifi_credentials_all(void);

int main(int argc, char *argv[])
{
    int failures = 0;
    (void)argc;
    (void)argv;

    printf("zclaw Host Tests\n");
    printf("===================\n\n");

    failures += test_json_all();
    failures += test_tools_parse_all();
    failures += test_json_util_integration_all();
    failures += test_runtime_utils_all();
    failures += test_memory_keys_all();
    failures += test_telegram_update_all();
    failures += test_telegram_token_all();
    failures += test_telegram_chat_ids_all();
    failures += test_telegram_poll_policy_all();
    failures += test_agent_all();
    failures += test_tools_gpio_policy_all();
    failures += test_llm_auth_all();
    failures += test_wifi_credentials_all();

    printf("\n===================\n");
    if (failures == 0) {
        printf("All tests passed!\n");
        return 0;
    } else {
        printf("%d test(s) failed\n", failures);
        return 1;
    }
}
