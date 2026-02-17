#ifndef WEBSETUP_H
#define WEBSETUP_H

#include "esp_err.h"
#include <stdbool.h>

// Start web setup server in AP (hotspot) mode
esp_err_t websetup_start_ap_mode(void);

// Start web setup server in STA mode (on existing network)
esp_err_t websetup_start_sta_mode(void);

// Stop web setup server
void websetup_stop(void);

// Check if setup is complete (credentials saved)
bool websetup_is_configured(void);

// Get configured WiFi SSID
bool websetup_get_wifi_ssid(char *ssid, size_t len);

// Get configured WiFi password
bool websetup_get_wifi_pass(char *pass, size_t len);

#endif // WEBSETUP_H
