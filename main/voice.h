#ifndef VOICE_H
#define VOICE_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#if CONFIG_ZCLAW_VOICE
esp_err_t voice_start(QueueHandle_t input_queue);
#else
static inline esp_err_t voice_start(QueueHandle_t input_queue)
{
    (void)input_queue;
    return ESP_OK;
}
#endif

#endif // VOICE_H
