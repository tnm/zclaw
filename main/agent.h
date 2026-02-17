#ifndef AGENT_H
#define AGENT_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Start the agent task
esp_err_t agent_start(QueueHandle_t input_queue,
                      QueueHandle_t channel_output_queue,
                      QueueHandle_t telegram_output_queue);

#ifdef TEST_BUILD
// Test-only helpers to drive agent logic without spawning FreeRTOS tasks.
void agent_test_reset(void);
void agent_test_set_queues(QueueHandle_t channel_output_queue,
                           QueueHandle_t telegram_output_queue);
void agent_test_process_message(const char *user_message);
#endif

#endif // AGENT_H
