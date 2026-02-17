#ifndef CHANNEL_H
#define CHANNEL_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Initialize the channel (USB serial)
void channel_init(void);

// Start the channel task (reads serial, writes responses)
void channel_start(QueueHandle_t input_queue, QueueHandle_t output_queue);

// Write a string to the serial output (thread-safe)
void channel_write(const char *text);

#endif // CHANNEL_H
