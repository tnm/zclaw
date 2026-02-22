#ifndef CRON_H
#define CRON_H

#include "config.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stdint.h>

// Cron entry types
typedef enum {
    CRON_TYPE_PERIODIC,     // Every N minutes
    CRON_TYPE_DAILY,        // At specific hour:minute
    CRON_TYPE_CONDITION,    // When condition is met (checked periodically)
    CRON_TYPE_ONCE,         // Run once after N minutes
} cron_type_t;

// Cron entry structure
typedef struct {
    uint8_t id;                         // Unique ID (1-255, 0 = empty)
    cron_type_t type;
    uint16_t interval_minutes;          // For PERIODIC: minutes between runs
    uint8_t hour;                       // For DAILY: 0-23
    uint8_t minute;                     // For DAILY: 0-59
    char action[CRON_MAX_ACTION_LEN];   // Action to execute (sent to agent)
    uint32_t last_run;                  // Unix timestamp of last run
    bool enabled;
} cron_entry_t;

// Initialize cron system and sync NTP
esp_err_t cron_init(void);

// Start cron task
esp_err_t cron_start(QueueHandle_t agent_input_queue);

// Add/update a cron entry (returns entry ID, or 0 on error)
uint8_t cron_set(cron_type_t type, uint16_t interval_or_hour, uint8_t minute, const char *action);

// List all cron entries (fills buffer with JSON array string)
void cron_list(char *buf, size_t buf_len);

// Delete a cron entry by ID
esp_err_t cron_delete(uint8_t id);

// Get current time as string
void cron_get_time_str(char *buf, size_t buf_len);

// Configure timezone using a POSIX TZ string (persists to NVS)
esp_err_t cron_set_timezone(const char *timezone_posix);

// Get configured POSIX timezone string
void cron_get_timezone(char *buf, size_t buf_len);

// Get current timezone abbreviation (for example: UTC, PST, PDT)
void cron_get_timezone_abbrev(char *buf, size_t buf_len);

// Check if time is synced
bool cron_is_time_synced(void);

#endif // CRON_H
