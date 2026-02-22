#include "telegram_token.h"

#include <ctype.h>
#include <string.h>

bool telegram_extract_bot_id(const char *token, char *out, size_t out_len)
{
    const char *colon = NULL;
    size_t id_len = 0;

    if (!token || !out || out_len == 0) {
        return false;
    }

    out[0] = '\0';

    colon = strchr(token, ':');
    if (!colon || colon == token) {
        return false;
    }

    id_len = (size_t)(colon - token);
    if (id_len + 1 > out_len) {
        return false;
    }

    for (size_t i = 0; i < id_len; i++) {
        if (!isdigit((unsigned char)token[i])) {
            return false;
        }
    }

    memcpy(out, token, id_len);
    out[id_len] = '\0';
    return true;
}
