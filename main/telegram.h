#ifndef TELEGRAM_H
#define TELEGRAM_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>

// Initialize Telegram client
esp_err_t telegram_init(void);

// Start Telegram polling task
esp_err_t telegram_start(QueueHandle_t input_queue, QueueHandle_t output_queue);

// Send a message to the configured chat
esp_err_t telegram_send(const char *text);

// Send startup notification
esp_err_t telegram_send_startup(void);

// Check if Telegram is configured (token exists)
bool telegram_is_configured(void);

// Get the chat ID we're responding to
int64_t telegram_get_chat_id(void);

#endif // TELEGRAM_H
