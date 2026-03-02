#ifndef MOCK_MEMORY_H
#define MOCK_MEMORY_H

#include "mock_esp.h"

void mock_memory_reset(void);
void mock_memory_set_kv(const char *key, const char *value);
void mock_memory_fail_next_set(esp_err_t err);

#endif // MOCK_MEMORY_H
