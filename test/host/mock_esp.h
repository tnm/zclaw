/*
 * Mock ESP-IDF types for host testing
 */

#ifndef MOCK_ESP_H
#define MOCK_ESP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// Mock esp_err_t
typedef int esp_err_t;
#define ESP_OK          0
#define ESP_FAIL        -1
#define ESP_ERR_NO_MEM  0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NOT_FOUND 0x105

// Mock logging
#define ESP_LOGE(tag, fmt, ...) printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) /* debug off */

// Mock state helpers for host tests.
void mock_esp_reset(void);
void mock_esp_set_heap_state(size_t free_heap, size_t min_heap, size_t largest_block);
void mock_esp_set_wifi_ap_info(esp_err_t err, int rssi);

#endif // MOCK_ESP_H
