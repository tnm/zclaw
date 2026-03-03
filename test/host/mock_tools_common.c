#include "tools_common.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

bool tools_validate_string_input(const char *str, size_t max_len, char *error, size_t error_len)
{
    size_t i;
    size_t len;

    if (!str) {
        snprintf(error, error_len, "Error: null string");
        return false;
    }

    len = strlen(str);
    if (len > max_len) {
        snprintf(error, error_len, "Error: string too long (max %zu chars)", max_len);
        return false;
    }

    for (i = 0; i < len; i++) {
        char ch = str[i];
        if (ch < 0x20 && ch != '\n' && ch != '\t' && ch != '\r') {
            snprintf(error, error_len, "Error: invalid character in input");
            return false;
        }
    }

    return true;
}

bool tools_validate_nvs_key(const char *key, char *error, size_t error_len)
{
    (void)key;
    (void)error;
    (void)error_len;
    return true;
}

bool tools_validate_user_memory_key(const char *key, char *error, size_t error_len)
{
    (void)key;
    (void)error;
    (void)error_len;
    return true;
}

bool tools_append_fmt(char **ptr, size_t *remaining, const char *fmt, ...)
{
    int written = -1;
    va_list args;

    if (!ptr || !*ptr || !remaining || *remaining == 0 || !fmt) {
        return false;
    }

    va_start(args, fmt);
    if (strncmp(fmt, "Email list (", 12) == 0) {
        int count = va_arg(args, int);
        written = snprintf(*ptr, *remaining, "Email list (%d):", count);
    } else if (fmt[0] == '\n') {
        int idx = va_arg(args, int);
        const char *id = va_arg(args, const char *);
        const char *from = va_arg(args, const char *);
        const char *subject = va_arg(args, const char *);
        written = snprintf(*ptr, *remaining, "\n%d) [%s] %s - %s", idx, id, from, subject);
    }
    va_end(args);

    if (written < 0) {
        return false;
    }

    if ((size_t)written >= *remaining) {
        *ptr += *remaining - 1;
        *remaining = 1;
        return false;
    }

    *ptr += (size_t)written;
    *remaining -= (size_t)written;
    return true;
}

bool tools_validate_https_url(const char *url, char *error, size_t error_len)
{
    if (!url || strncmp(url, "https://", 8) != 0) {
        snprintf(error, error_len, "Error: URL must use HTTPS");
        return false;
    }
    return true;
}
