#ifndef FREERTOS_TASK_H
#define FREERTOS_TASK_H

#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

BaseType_t xTaskCreate(TaskFunction_t task_fn,
                       const char *name,
                       uint32_t stack_depth,
                       void *task_arg,
                       UBaseType_t priority,
                       TaskHandle_t *out_handle);
void vTaskDelay(TickType_t ticks_to_delay);
void vTaskDelete(TaskHandle_t task_to_delete);

#endif // FREERTOS_TASK_H
