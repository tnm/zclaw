#ifndef MOCK_LLM_H
#define MOCK_LLM_H

#include "llm.h"
#include <stdbool.h>
#include <stddef.h>

void mock_llm_set_backend(llm_backend_t backend, const char *model);
void mock_llm_reset(void);
bool mock_llm_push_result(esp_err_t err, const char *response_json);
int mock_llm_request_count(void);

#endif // MOCK_LLM_H
