#include "wifi_credentials.h"

#include <stdio.h>
#include <string.h>

static void set_error(char *error, size_t error_len, const char *message)
{
    if (!error || error_len == 0) {
        return;
    }
    snprintf(error, error_len, "%s", message ? message : "Invalid WiFi credentials");
}

bool wifi_credentials_validate(const char *ssid,
                               const char *pass,
                               char *error,
                               size_t error_len)
{
    size_t ssid_len;
    size_t pass_len;

    if (!ssid || !pass) {
        set_error(error, error_len, "WiFi credentials missing");
        return false;
    }

    ssid_len = strlen(ssid);
    pass_len = strlen(pass);

    if (ssid_len == 0) {
        set_error(error, error_len, "WiFi SSID is required");
        return false;
    }
    if (ssid_len > WIFI_STA_SSID_MAX_BYTES) {
        set_error(error, error_len, "WiFi SSID exceeds 32 bytes");
        return false;
    }
    if (pass_len > WIFI_STA_PASS_MAX_BYTES) {
        set_error(error, error_len, "WiFi password exceeds 63 characters");
        return false;
    }
    if (pass_len > 0 && pass_len < WIFI_STA_PASS_MIN_BYTES) {
        set_error(error, error_len, "WiFi password must be 8-63 characters or empty for open network");
        return false;
    }

    return true;
}

void wifi_credentials_copy_to_sta_config(uint8_t ssid_out[WIFI_STA_SSID_MAX_BYTES],
                                         uint8_t pass_out[WIFI_STA_PASS_MAX_BYTES + 1],
                                         const char *ssid,
                                         const char *pass)
{
    size_t ssid_len;
    size_t pass_len;

    if (!ssid_out || !pass_out || !ssid || !pass) {
        return;
    }

    ssid_len = strlen(ssid);
    pass_len = strlen(pass);

    memset(ssid_out, 0, WIFI_STA_SSID_MAX_BYTES);
    memset(pass_out, 0, WIFI_STA_PASS_MAX_BYTES + 1);

    if (ssid_len > WIFI_STA_SSID_MAX_BYTES) {
        ssid_len = WIFI_STA_SSID_MAX_BYTES;
    }
    if (pass_len > WIFI_STA_PASS_MAX_BYTES) {
        pass_len = WIFI_STA_PASS_MAX_BYTES;
    }

    /* Allow full 32-byte SSIDs even when no trailing NUL fits. */
    if (ssid_len > 0) {
        memcpy(ssid_out, ssid, ssid_len);
    }
    if (pass_len > 0) {
        memcpy(pass_out, pass, pass_len);
    }
}
