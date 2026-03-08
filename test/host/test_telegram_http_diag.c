/*
 * Host tests for telegram HTTP diagnostics helpers.
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mock_esp.h"
#include "esp_http_client.h"
#include "telegram_http_diag.h"

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while (0)
#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("  FAIL: '%s' != '%s' (line %d)\n", (a), (b), __LINE__); \
        return 1; \
    } \
} while (0)

static void reset_state(void)
{
    mock_esp_reset();
}

TEST(format_int64_decimal_handles_common_and_edge_values)
{
    char out[32];

    reset_state();

    ASSERT(telegram_format_int64_decimal(0, out, sizeof(out)));
    ASSERT_STR_EQ(out, "0");

    ASSERT(telegram_format_int64_decimal(42, out, sizeof(out)));
    ASSERT_STR_EQ(out, "42");

    ASSERT(telegram_format_int64_decimal(-42, out, sizeof(out)));
    ASSERT_STR_EQ(out, "-42");

    ASSERT(telegram_format_int64_decimal(INT64_MIN, out, sizeof(out)));
    ASSERT_STR_EQ(out, "-9223372036854775808");

    return 0;
}

TEST(format_int64_decimal_rejects_bad_buffers)
{
    char tiny[2];

    reset_state();

    tiny[0] = 'x';
    tiny[1] = '\0';
    ASSERT(!telegram_format_int64_decimal(42, tiny, sizeof(tiny)));
    ASSERT(tiny[0] == '\0');
    ASSERT(!telegram_format_int64_decimal(42, NULL, sizeof(tiny)));
    ASSERT(!telegram_format_int64_decimal(42, tiny, 0));

    return 0;
}

TEST(capture_snapshot_reads_mock_heap_and_wifi_state)
{
    telegram_http_diag_snapshot_t snapshot = {0};

    reset_state();
    mock_esp_set_heap_state(53788, 1052, 14848);
    mock_esp_set_wifi_ap_info(ESP_OK, -64);

    telegram_http_diag_capture_snapshot(&snapshot);

    ASSERT(snapshot.free_heap == 53788U);
    ASSERT(snapshot.min_heap == 1052U);
    ASSERT(snapshot.largest_block == 14848U);
    ASSERT(snapshot.rssi_valid);
    ASSERT(snapshot.rssi == -64);

    mock_esp_set_wifi_ap_info(ESP_ERR_NOT_FOUND, 0);
    memset(&snapshot, 0, sizeof(snapshot));
    telegram_http_diag_capture_snapshot(&snapshot);

    ASSERT(snapshot.free_heap == 53788U);
    ASSERT(snapshot.min_heap == 1052U);
    ASSERT(snapshot.largest_block == 14848U);
    ASSERT(!snapshot.rssi_valid);
    ASSERT(snapshot.rssi == 0);

    return 0;
}

TEST(log_helpers_accept_mock_http_client)
{
    telegram_http_diag_snapshot_t before = {
        .free_heap = 60000,
        .min_heap = 4000,
        .largest_block = 20000,
        .rssi = -58,
        .rssi_valid = true,
    };
    telegram_http_diag_snapshot_t after = {
        .free_heap = 58000,
        .min_heap = 3500,
        .largest_block = 16000,
        .rssi = -61,
        .rssi_valid = true,
    };
    mock_esp_http_client_t client = {
        .status_code = 503,
        .sock_errno = 54,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
    };

    reset_state();

    telegram_http_diag_log("getUpdates",
                           &client,
                           ESP_FAIL,
                           -1,
                           0,
                           128,
                           2,
                           1,
                           1,
                           7,
                           &before,
                           &after);
    telegram_http_diag_log_failure("sendMessage", &client, ESP_FAIL, -1);

    return 0;
}

int test_telegram_http_diag_all(void)
{
    int failures = 0;

    printf("\nTelegram HTTP Diagnostics Tests:\n");

    printf("  format_int64_decimal_handles_common_and_edge_values... ");
    if (test_format_int64_decimal_handles_common_and_edge_values() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  format_int64_decimal_rejects_bad_buffers... ");
    if (test_format_int64_decimal_rejects_bad_buffers() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  capture_snapshot_reads_mock_heap_and_wifi_state... ");
    if (test_capture_snapshot_reads_mock_heap_and_wifi_state() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    printf("  log_helpers_accept_mock_http_client... ");
    if (test_log_helpers_accept_mock_http_client() == 0) {
        printf("OK\n");
    } else {
        failures++;
    }

    return failures;
}
