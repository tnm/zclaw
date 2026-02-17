#include "freertos/queue.h"
#include "freertos/task.h"
#include "mock_freertos.h"
#include <stdlib.h>
#include <string.h>

#define MOCK_MAX_DELAYS 64

typedef struct mock_queue {
    UBaseType_t capacity;
    UBaseType_t item_size;
    UBaseType_t count;
    UBaseType_t head;
    UBaseType_t tail;
    unsigned char *storage;
} mock_queue_t;

static TickType_t s_delays[MOCK_MAX_DELAYS];
static size_t s_delay_count = 0;

void mock_freertos_reset(void)
{
    s_delay_count = 0;
    memset(s_delays, 0, sizeof(s_delays));
}

size_t mock_freertos_delay_count(void)
{
    return s_delay_count;
}

TickType_t mock_freertos_delay_at(size_t index)
{
    if (index >= s_delay_count) {
        return 0;
    }
    return s_delays[index];
}

QueueHandle_t xQueueCreate(UBaseType_t queue_length, UBaseType_t item_size)
{
    mock_queue_t *queue;
    size_t bytes;

    if (queue_length == 0 || item_size == 0) {
        return NULL;
    }

    queue = (mock_queue_t *)calloc(1, sizeof(*queue));
    if (!queue) {
        return NULL;
    }

    bytes = (size_t)queue_length * (size_t)item_size;
    queue->storage = (unsigned char *)calloc(1, bytes);
    if (!queue->storage) {
        free(queue);
        return NULL;
    }

    queue->capacity = queue_length;
    queue->item_size = item_size;
    return (QueueHandle_t)queue;
}

BaseType_t xQueueSend(QueueHandle_t handle, const void *item, TickType_t timeout_ticks)
{
    mock_queue_t *queue = (mock_queue_t *)handle;
    size_t offset;
    (void)timeout_ticks;

    if (!queue || !item) {
        return pdFALSE;
    }

    if (queue->count >= queue->capacity) {
        return pdFALSE;
    }

    offset = (size_t)queue->tail * (size_t)queue->item_size;
    memcpy(queue->storage + offset, item, queue->item_size);
    queue->tail = (queue->tail + 1u) % queue->capacity;
    queue->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t handle, void *item, TickType_t timeout_ticks)
{
    mock_queue_t *queue = (mock_queue_t *)handle;
    size_t offset;
    (void)timeout_ticks;

    if (!queue || !item) {
        return pdFALSE;
    }

    if (queue->count == 0) {
        return pdFALSE;
    }

    offset = (size_t)queue->head * (size_t)queue->item_size;
    memcpy(item, queue->storage + offset, queue->item_size);
    queue->head = (queue->head + 1u) % queue->capacity;
    queue->count--;
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t handle)
{
    mock_queue_t *queue = (mock_queue_t *)handle;
    if (!queue) {
        return 0;
    }
    return queue->count;
}

void vQueueDelete(QueueHandle_t handle)
{
    mock_queue_t *queue = (mock_queue_t *)handle;
    if (!queue) {
        return;
    }

    free(queue->storage);
    free(queue);
}

BaseType_t xTaskCreate(TaskFunction_t task_fn,
                       const char *name,
                       uint32_t stack_depth,
                       void *task_arg,
                       UBaseType_t priority,
                       TaskHandle_t *out_handle)
{
    (void)task_fn;
    (void)name;
    (void)stack_depth;
    (void)task_arg;
    (void)priority;
    if (out_handle) {
        *out_handle = NULL;
    }
    return pdPASS;
}

void vTaskDelay(TickType_t ticks_to_delay)
{
    if (s_delay_count < MOCK_MAX_DELAYS) {
        s_delays[s_delay_count++] = ticks_to_delay;
    }
}

void vTaskDelete(TaskHandle_t task_to_delete)
{
    (void)task_to_delete;
}
