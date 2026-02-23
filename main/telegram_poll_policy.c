#include "telegram_poll_policy.h"

int telegram_poll_timeout_for_backend(llm_backend_t backend)
{
    if (backend == LLM_BACKEND_OPENROUTER) {
        return TELEGRAM_POLL_TIMEOUT_OPENROUTER;
    }

    return TELEGRAM_POLL_TIMEOUT;
}
