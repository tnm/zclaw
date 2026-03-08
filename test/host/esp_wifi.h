#ifndef ESP_WIFI_H
#define ESP_WIFI_H

#include "mock_esp.h"

typedef struct {
    int rssi;
} wifi_ap_record_t;

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info);

#endif // ESP_WIFI_H
