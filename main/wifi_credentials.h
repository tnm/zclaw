#ifndef WIFI_CREDENTIALS_H
#define WIFI_CREDENTIALS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_STA_SSID_MAX_BYTES 32
#define WIFI_STA_PASS_MAX_BYTES 63
#define WIFI_STA_PASS_MIN_BYTES 8

bool wifi_credentials_validate(const char *ssid,
                               const char *pass,
                               char *error,
                               size_t error_len);

void wifi_credentials_copy_to_sta_config(uint8_t ssid_out[WIFI_STA_SSID_MAX_BYTES],
                                         uint8_t pass_out[WIFI_STA_PASS_MAX_BYTES + 1],
                                         const char *ssid,
                                         const char *pass);

#endif // WIFI_CREDENTIALS_H
