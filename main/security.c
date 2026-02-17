#include "security.h"
#include <ctype.h>
#include <string.h>

static bool contains_token(const char *lower, const char *token)
{
    return strstr(lower, token) != NULL;
}

bool security_key_is_sensitive(const char *key)
{
    if (!key || key[0] == '\0') {
        return false;
    }

    char lower[64];
    size_t i = 0;
    while (key[i] != '\0' && i < sizeof(lower) - 1) {
        lower[i] = (char)tolower((unsigned char)key[i]);
        i++;
    }
    lower[i] = '\0';

    return contains_token(lower, "pass") ||
           contains_token(lower, "token") ||
           contains_token(lower, "secret") ||
           contains_token(lower, "apikey") ||
           contains_token(lower, "api_key") ||
           contains_token(lower, "auth");
}
