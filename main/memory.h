#ifndef MEMORY_H
#define MEMORY_H

#include "esp_err.h"
#include <stdbool.h>

// Initialize NVS flash storage
esp_err_t memory_init(void);

// Store a string value (persists across reboots)
esp_err_t memory_set(const char *key, const char *value);

// Retrieve a string value (returns false if key not found)
bool memory_get(const char *key, char *value, size_t max_len);

// Delete a key
esp_err_t memory_delete(const char *key);

#endif // MEMORY_H
