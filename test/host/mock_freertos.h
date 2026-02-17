#ifndef MOCK_FREERTOS_H
#define MOCK_FREERTOS_H

#include "freertos/FreeRTOS.h"
#include <stddef.h>

void mock_freertos_reset(void);
size_t mock_freertos_delay_count(void);
TickType_t mock_freertos_delay_at(size_t index);

#endif // MOCK_FREERTOS_H
