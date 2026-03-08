/*
 * Mock ESP-IDF functions for host testing
 */

#include "mock_esp.h"
#include "esp_wifi.h"

static size_t s_free_heap = 0;
static size_t s_min_heap = 0;
static size_t s_largest_block = 0;
static esp_err_t s_wifi_ap_info_err = ESP_ERR_NOT_FOUND;
static int s_wifi_rssi = 0;

void mock_esp_reset(void)
{
    s_free_heap = 0;
    s_min_heap = 0;
    s_largest_block = 0;
    s_wifi_ap_info_err = ESP_ERR_NOT_FOUND;
    s_wifi_rssi = 0;
}

void mock_esp_set_heap_state(size_t free_heap, size_t min_heap, size_t largest_block)
{
    s_free_heap = free_heap;
    s_min_heap = min_heap;
    s_largest_block = largest_block;
}

void mock_esp_set_wifi_ap_info(esp_err_t err, int rssi)
{
    s_wifi_ap_info_err = err;
    s_wifi_rssi = rssi;
}

size_t esp_get_free_heap_size(void)
{
    return s_free_heap;
}

size_t esp_get_minimum_free_heap_size(void)
{
    return s_min_heap;
}

size_t heap_caps_get_largest_free_block(unsigned int caps)
{
    (void)caps;
    return s_largest_block;
}

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info)
{
    if (s_wifi_ap_info_err != ESP_OK) {
        return s_wifi_ap_info_err;
    }
    if (!ap_info) {
        return ESP_ERR_INVALID_ARG;
    }
    ap_info->rssi = s_wifi_rssi;
    return ESP_OK;
}
