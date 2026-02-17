#ifndef AGENT_H
#define AGENT_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Start the agent task
void agent_start(QueueHandle_t input_queue,
                 QueueHandle_t channel_output_queue,
                 QueueHandle_t telegram_output_queue);

#endif // AGENT_H
