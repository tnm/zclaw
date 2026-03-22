#ifndef TOOLS_HANDLERS_H
#define TOOLS_HANDLERS_H

#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Tool handler convention:
// - return true when the operation is handled (including benign "not found" states)
// - return false on validation or execution errors
// - always write a human-readable message to result.

// GPIO
bool tools_gpio_write_handler(const cJSON *input, char *result, size_t result_len);
bool tools_gpio_read_handler(const cJSON *input, char *result, size_t result_len);
bool tools_gpio_read_all_handler(const cJSON *input, char *result, size_t result_len);
bool tools_delay_handler(const cJSON *input, char *result, size_t result_len);
bool tools_i2c_scan_handler(const cJSON *input, char *result, size_t result_len);
bool tools_i2c_write_handler(const cJSON *input, char *result, size_t result_len);
bool tools_i2c_read_handler(const cJSON *input, char *result, size_t result_len);
bool tools_i2c_write_read_handler(const cJSON *input, char *result, size_t result_len);
bool tools_dht_read_handler(const cJSON *input, char *result, size_t result_len);

// Memory
bool tools_memory_set_handler(const cJSON *input, char *result, size_t result_len);
bool tools_memory_get_handler(const cJSON *input, char *result, size_t result_len);
bool tools_memory_list_handler(const cJSON *input, char *result, size_t result_len);
bool tools_memory_delete_handler(const cJSON *input, char *result, size_t result_len);
bool tools_set_persona_handler(const cJSON *input, char *result, size_t result_len);
bool tools_get_persona_handler(const cJSON *input, char *result, size_t result_len);
bool tools_reset_persona_handler(const cJSON *input, char *result, size_t result_len);

// Scheduler / Time
bool tools_cron_set_handler(const cJSON *input, char *result, size_t result_len);
bool tools_cron_list_handler(const cJSON *input, char *result, size_t result_len);
bool tools_cron_delete_handler(const cJSON *input, char *result, size_t result_len);
bool tools_get_time_handler(const cJSON *input, char *result, size_t result_len);
bool tools_set_timezone_handler(const cJSON *input, char *result, size_t result_len);
bool tools_get_timezone_handler(const cJSON *input, char *result, size_t result_len);

// System / User tools
bool tools_get_version_handler(const cJSON *input, char *result, size_t result_len);
bool tools_get_health_handler(const cJSON *input, char *result, size_t result_len);
bool tools_get_diagnostics_handler(const cJSON *input, char *result, size_t result_len);
bool tools_create_tool_handler(const cJSON *input, char *result, size_t result_len);
bool tools_list_user_tools_handler(const cJSON *input, char *result, size_t result_len);
bool tools_delete_user_tool_handler(const cJSON *input, char *result, size_t result_len);

#ifdef TEST_BUILD
bool tools_dht_test_decode_bytes(const char *model_name,
                                 int pin,
                                 const uint8_t data[5],
                                 char *result,
                                 size_t result_len);
void tools_dht_test_reset(void);
void tools_dht_test_set_mock_success(const uint8_t data[5]);
void tools_dht_test_set_mock_failure(const char *error_message);
#endif

#endif // TOOLS_HANDLERS_H
