#ifndef CHANNEL_H
#define CHANNEL_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stddef.h>
#include <stdint.h>

// Initialize the channel (USB serial)
void channel_init(void);

// Start the channel task (reads serial, writes responses)
esp_err_t channel_start(QueueHandle_t input_queue, QueueHandle_t output_queue);

// Write a string to the serial output.
// Note: output can interleave with other channel task writes (echo/bridge traffic).
void channel_write(const char *text);

// Exchange one LLM request/response line with a host bridge over serial.
// Used by emulator live-LLM mode.
esp_err_t channel_llm_bridge_exchange(const char *request_json,
                                      char *response_json,
                                      size_t response_json_size,
                                      uint32_t timeout_ms);

#endif // CHANNEL_H
