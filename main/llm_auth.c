#include "llm_auth.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

bool llm_copy_api_key(char *dst, size_t dst_size, const char *src)
{
    size_t key_len;

    if (!dst || dst_size == 0 || !src) {
        return false;
    }

    key_len = strlen(src);
    if (key_len == 0 || key_len > LLM_API_KEY_MAX_LEN || key_len >= dst_size) {
        if (dst_size > 0) {
            dst[0] = '\0';
        }
        return false;
    }

    memcpy(dst, src, key_len + 1);
    return true;
}

bool llm_build_bearer_auth_header(const char *api_key, char *auth_header, size_t auth_header_size)
{
    int written;
    size_t key_len;

    if (!api_key || !auth_header || auth_header_size == 0) {
        return false;
    }

    key_len = strlen(api_key);
    if (key_len == 0 || key_len > LLM_API_KEY_MAX_LEN) {
        auth_header[0] = '\0';
        return false;
    }

    written = snprintf(auth_header, auth_header_size, "Bearer %s", api_key);
    if (written < 0 || (size_t)written >= auth_header_size) {
        auth_header[0] = '\0';
        return false;
    }

    return true;
}
