#ifndef FREERTOS_QUEUE_H
#define FREERTOS_QUEUE_H

#include "freertos/FreeRTOS.h"
#include <stddef.h>

typedef struct mock_queue *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t queue_length, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout_ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout_ticks);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
void vQueueDelete(QueueHandle_t queue);

#endif // FREERTOS_QUEUE_H
