/*
 * Host tests for LLM API key sizing and auth header formatting.
 */

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "llm_auth.h"

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while(0)

static void fill_key(char *out, size_t key_len, char ch)
{
    size_t i;
    for (i = 0; i < key_len; i++) {
        out[i] = ch;
    }
    out[key_len] = '\0';
}

TEST(configured_capacity_is_large_enough)
{
    ASSERT(LLM_API_KEY_MAX_LEN >= 256);
    ASSERT(LLM_AUTH_HEADER_BUF_SIZE > (sizeof("Bearer ") - 1 + LLM_API_KEY_MAX_LEN));
    return 0;
}

TEST(copy_supports_long_key)
{
    char src[321];
    char dst[LLM_API_KEY_BUF_SIZE];
    fill_key(src, 320, 'k');
    ASSERT(llm_copy_api_key(dst, sizeof(dst), src));
    ASSERT(strcmp(dst, src) == 0);
    return 0;
}

TEST(copy_rejects_key_above_limit)
{
    char src[LLM_API_KEY_MAX_LEN + 2];
    char dst[LLM_API_KEY_BUF_SIZE];
    fill_key(src, LLM_API_KEY_MAX_LEN + 1, 'x');
    ASSERT(!llm_copy_api_key(dst, sizeof(dst), src));
    return 0;
}

TEST(build_bearer_header_supports_long_key)
{
    char key[301];
    char header[LLM_AUTH_HEADER_BUF_SIZE];
    fill_key(key, 300, 'a');
    ASSERT(llm_build_bearer_auth_header(key, header, sizeof(header)));
    ASSERT(strncmp(header, "Bearer ", 7) == 0);
    ASSERT(strcmp(header + 7, key) == 0);
    return 0;
}

TEST(build_bearer_header_rejects_small_buffer)
{
    char header[8];
    ASSERT(!llm_build_bearer_auth_header("abc", header, sizeof(header)));
    return 0;
}

int test_llm_auth_all(void)
{
    int failures = 0;

    printf("\nLLM Auth Tests:\n");

    printf("  configured_capacity_is_large_enough... ");
    if (test_configured_capacity_is_large_enough() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  copy_supports_long_key... ");
    if (test_copy_supports_long_key() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  copy_rejects_key_above_limit... ");
    if (test_copy_rejects_key_above_limit() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  build_bearer_header_supports_long_key... ");
    if (test_build_bearer_header_supports_long_key() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  build_bearer_header_rejects_small_buffer... ");
    if (test_build_bearer_header_rejects_small_buffer() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
