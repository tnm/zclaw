#include "memory_keys.h"
#include "nvs_keys.h"
#include <string.h>

bool memory_keys_is_user_key(const char *key)
{
    if (!key) {
        return false;
    }
    size_t prefix_len = strlen(USER_MEMORY_KEY_PREFIX);
    return strncmp(key, USER_MEMORY_KEY_PREFIX, prefix_len) == 0;
}

bool memory_keys_is_sensitive(const char *key)
{
    const char *sensitive[] = {
        NVS_KEY_API_KEY,
        NVS_KEY_TG_TOKEN,
        NVS_KEY_TG_CHAT_ID,
        NVS_KEY_WIFI_PASS,
        NVS_KEY_LLM_BACKEND,
        NVS_KEY_LLM_MODEL,
        NVS_KEY_WIFI_SSID,
        NULL
    };

    if (!key) {
        return false;
    }

    for (int i = 0; sensitive[i] != NULL; i++) {
        if (strcmp(key, sensitive[i]) == 0) {
            return true;
        }
    }
    return false;
}
