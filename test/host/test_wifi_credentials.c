/*
 * Host tests for WiFi credential validation/copy logic.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "wifi_credentials.h"

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while(0)

static void fill_string(char *buf, size_t len, char ch)
{
    size_t i;
    for (i = 0; i < len; i++) {
        buf[i] = ch;
    }
    buf[len] = '\0';
}

TEST(validate_accepts_max_lengths)
{
    char ssid[WIFI_STA_SSID_MAX_BYTES + 1];
    char pass[WIFI_STA_PASS_MAX_BYTES + 1];
    char error[128] = {0};

    fill_string(ssid, WIFI_STA_SSID_MAX_BYTES, 's');
    fill_string(pass, WIFI_STA_PASS_MAX_BYTES, 'p');

    ASSERT(wifi_credentials_validate(ssid, pass, error, sizeof(error)));
    return 0;
}

TEST(validate_accepts_open_network_password)
{
    char error[128] = {0};
    ASSERT(wifi_credentials_validate("MyNetwork", "", error, sizeof(error)));
    return 0;
}

TEST(validate_rejects_ssid_above_max)
{
    char ssid[WIFI_STA_SSID_MAX_BYTES + 2];
    char error[128] = {0};

    fill_string(ssid, WIFI_STA_SSID_MAX_BYTES + 1, 'x');
    ASSERT(!wifi_credentials_validate(ssid, "password123", error, sizeof(error)));
    ASSERT(strstr(error, "SSID") != NULL);
    return 0;
}

TEST(validate_rejects_password_above_max)
{
    char pass[WIFI_STA_PASS_MAX_BYTES + 2];
    char error[128] = {0};

    fill_string(pass, WIFI_STA_PASS_MAX_BYTES + 1, 'y');
    ASSERT(!wifi_credentials_validate("MyNetwork", pass, error, sizeof(error)));
    ASSERT(strstr(error, "password") != NULL);
    return 0;
}

TEST(validate_rejects_short_nonempty_password)
{
    char error[128] = {0};
    ASSERT(!wifi_credentials_validate("MyNetwork", "short7!", error, sizeof(error)));
    ASSERT(strstr(error, "8-63") != NULL);
    return 0;
}

TEST(copy_preserves_full_32_byte_ssid)
{
    char ssid[WIFI_STA_SSID_MAX_BYTES + 1];
    char pass[WIFI_STA_PASS_MAX_BYTES + 1];
    uint8_t ssid_out[WIFI_STA_SSID_MAX_BYTES];
    uint8_t pass_out[WIFI_STA_PASS_MAX_BYTES + 1];
    size_t i;

    fill_string(ssid, WIFI_STA_SSID_MAX_BYTES, 'a');
    fill_string(pass, WIFI_STA_PASS_MAX_BYTES, 'b');

    wifi_credentials_copy_to_sta_config(ssid_out, pass_out, ssid, pass);

    for (i = 0; i < WIFI_STA_SSID_MAX_BYTES; i++) {
        ASSERT(ssid_out[i] == (uint8_t)'a');
    }
    for (i = 0; i < WIFI_STA_PASS_MAX_BYTES; i++) {
        ASSERT(pass_out[i] == (uint8_t)'b');
    }
    ASSERT(pass_out[WIFI_STA_PASS_MAX_BYTES] == '\0');
    return 0;
}

int test_wifi_credentials_all(void)
{
    int failures = 0;

    printf("\nWiFi Credential Tests:\n");

    printf("  validate_accepts_max_lengths... ");
    if (test_validate_accepts_max_lengths() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  validate_accepts_open_network_password... ");
    if (test_validate_accepts_open_network_password() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  validate_rejects_ssid_above_max... ");
    if (test_validate_rejects_ssid_above_max() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  validate_rejects_password_above_max... ");
    if (test_validate_rejects_password_above_max() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  validate_rejects_short_nonempty_password... ");
    if (test_validate_rejects_short_nonempty_password() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  copy_preserves_full_32_byte_ssid... ");
    if (test_copy_preserves_full_32_byte_ssid() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
