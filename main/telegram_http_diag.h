#ifndef TELEGRAM_HTTP_DIAG_H
#define TELEGRAM_HTTP_DIAG_H

#include "esp_err.h"
#include "esp_http_client.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char buf[4096];
    size_t len;
    bool truncated;
} telegram_http_ctx_t;

typedef struct {
    uint32_t free_heap;
    uint32_t min_heap;
    uint32_t largest_block;
    int rssi;
    bool rssi_valid;
} telegram_http_diag_snapshot_t;

void telegram_http_diag_capture_snapshot(telegram_http_diag_snapshot_t *snapshot);
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
                            const telegram_http_diag_snapshot_t *after);
void telegram_http_diag_log_failure(const char *operation,
                                    esp_http_client_handle_t client,
                                    esp_err_t err,
                                    int status);
bool telegram_format_int64_decimal(int64_t value, char *out, size_t out_len);

#endif // TELEGRAM_HTTP_DIAG_H
