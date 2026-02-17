#ifndef OTA_H
#define OTA_H

#include "esp_err.h"
#include <stdbool.h>

// Initialize OTA subsystem
esp_err_t ota_init(void);

// Check if OTA update is available at the given URL
// Returns true if update available, fills version_out with new version string
bool ota_check_update(const char *url, char *version_out, size_t version_len);

// Perform OTA update from the given firmware URL
// This will download, verify, and install the firmware, then reboot
esp_err_t ota_perform_update(const char *url);

// Get the currently running firmware version
const char *ota_get_version(void);

// Mark current firmware as valid (prevent rollback)
esp_err_t ota_mark_valid(void);

// Mark current firmware as valid only if pending verification.
esp_err_t ota_mark_valid_if_pending(void);

// Returns true when running image is waiting for rollback confirmation.
bool ota_is_pending_verify(void);

// Rollback to previous firmware
esp_err_t ota_rollback(void);

#endif // OTA_H
