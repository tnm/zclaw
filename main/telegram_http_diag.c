#include "telegram_http_diag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <string.h>

static const char *TAG = "telegram";

static const char *http_transport_name(esp_http_client_transport_t transport)
{
    switch (transport) {
        case HTTP_TRANSPORT_OVER_TCP:
            return "tcp";
        case HTTP_TRANSPORT_OVER_SSL:
            return "ssl";
        default:
            return "unknown";
    }
}

static uint32_t elapsed_ms_since(int64_t started_us)
{
    int64_t now_us = esp_timer_get_time();
    if (now_us <= started_us) {
        return 0;
    }
    return (uint32_t)((now_us - started_us) / 1000);
}

void telegram_http_diag_capture_snapshot(telegram_http_diag_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }

    snapshot->free_heap = (uint32_t)esp_get_free_heap_size();
    snapshot->min_heap = (uint32_t)esp_get_minimum_free_heap_size();
    snapshot->largest_block = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    snapshot->rssi = 0;
    snapshot->rssi_valid = false;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        snapshot->rssi = ap_info.rssi;
        snapshot->rssi_valid = true;
    }
}

void telegram_http_diag_log(const char *operation,
                            esp_http_client_handle_t client,
                            esp_err_t err,
                            int status,
                            int64_t started_us,
                            size_t response_len,
                            int result_count,
                            int stale_count,
                            int accepted_count,
                            uint32_t poll_seq,
                            const telegram_http_diag_snapshot_t *before,
                            const telegram_http_diag_snapshot_t *after)
{
    int sock_errno = 0;
    esp_http_client_transport_t transport = HTTP_TRANSPORT_UNKNOWN;
    int heap_delta = 0;
    uint32_t heap_free = 0;
    uint32_t heap_min = 0;
    uint32_t heap_largest = 0;
    int rssi = 0;
    int rssi_ok = 0;
    bool ok = false;

    if (client) {
        if (status < 0) {
            status = esp_http_client_get_status_code(client);
        }
        sock_errno = esp_http_client_get_errno(client);
        transport = esp_http_client_get_transport_type(client);
    }

    if (after) {
        heap_free = after->free_heap;
        heap_min = after->min_heap;
        heap_largest = after->largest_block;
        if (after->rssi_valid) {
            rssi = after->rssi;
            rssi_ok = 1;
        }
    }
    if (before && after) {
        heap_delta = (int)after->free_heap - (int)before->free_heap;
    }

    ok = (err == ESP_OK && status == 200);
    if (ok) {
        ESP_LOGI(TAG,
                 "NETDIAG op=%s ok=1 status=%d err=%s(%d) errno=%d(%s) transport=%s "
                 "dur_ms=%lu poll_seq=%u res=%d stale=%d new=%d body_bytes=%u "
                 "heap_free=%lu heap_delta=%d heap_min=%lu heap_largest=%lu "
                 "rssi=%d rssi_ok=%d",
                 operation ? operation : "telegram_http",
                 status,
                 esp_err_to_name(err), err,
                 sock_errno,
                 sock_errno ? strerror(sock_errno) : "n/a",
                 http_transport_name(transport),
                 (unsigned long)elapsed_ms_since(started_us),
                 (unsigned)poll_seq,
                 result_count,
                 stale_count,
                 accepted_count,
                 (unsigned)response_len,
                 (unsigned long)heap_free,
                 heap_delta,
                 (unsigned long)heap_min,
                 (unsigned long)heap_largest,
                 rssi,
                 rssi_ok);
    } else {
        ESP_LOGW(TAG,
                 "NETDIAG op=%s ok=0 status=%d err=%s(%d) errno=%d(%s) transport=%s "
                 "dur_ms=%lu poll_seq=%u res=%d stale=%d new=%d body_bytes=%u "
                 "heap_free=%lu heap_delta=%d heap_min=%lu heap_largest=%lu "
                 "rssi=%d rssi_ok=%d",
                 operation ? operation : "telegram_http",
                 status,
                 esp_err_to_name(err), err,
                 sock_errno,
                 sock_errno ? strerror(sock_errno) : "n/a",
                 http_transport_name(transport),
                 (unsigned long)elapsed_ms_since(started_us),
                 (unsigned)poll_seq,
                 result_count,
                 stale_count,
                 accepted_count,
                 (unsigned)response_len,
                 (unsigned long)heap_free,
                 heap_delta,
                 (unsigned long)heap_min,
                 (unsigned long)heap_largest,
                 rssi,
                 rssi_ok);
    }
}

void telegram_http_diag_log_failure(const char *operation,
                                    esp_http_client_handle_t client,
                                    esp_err_t err,
                                    int status)
{
    int sock_errno = 0;
    esp_http_client_transport_t transport = HTTP_TRANSPORT_UNKNOWN;

    if (client) {
        sock_errno = esp_http_client_get_errno(client);
        if (status < 0) {
            status = esp_http_client_get_status_code(client);
        }
        transport = esp_http_client_get_transport_type(client);
    }

    ESP_LOGW(TAG,
             "%s failed: err=%s(%d) status=%d errno=%d(%s) transport=%s",
             operation ? operation : "HTTP request",
             esp_err_to_name(err), err,
             status,
             sock_errno,
             sock_errno ? strerror(sock_errno) : "n/a",
             http_transport_name(transport));
}

bool telegram_format_int64_decimal(int64_t value, char *out, size_t out_len)
{
    char reversed[24];
    size_t reversed_len = 0;
    uint64_t magnitude;
    size_t pos = 0;

    if (!out || out_len == 0) {
        return false;
    }

    if (value < 0) {
        out[pos++] = '-';
        magnitude = (uint64_t)(-(value + 1)) + 1ULL;
    } else {
        magnitude = (uint64_t)value;
    }

    do {
        if (reversed_len >= sizeof(reversed)) {
            out[0] = '\0';
            return false;
        }
        reversed[reversed_len++] = (char)('0' + (magnitude % 10ULL));
        magnitude /= 10ULL;
    } while (magnitude > 0);

    if (pos + reversed_len + 1 > out_len) {
        out[0] = '\0';
        return false;
    }

    while (reversed_len > 0) {
        out[pos++] = reversed[--reversed_len];
    }
    out[pos] = '\0';
    return true;
}
