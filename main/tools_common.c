#include "tools_common.h"
#include "config.h"
#include "gpio_policy.h"
#include "memory_keys.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

bool tools_validate_string_input(const char *str, size_t max_len, char *error, size_t error_len)
{
    if (!str) {
        snprintf(error, error_len, "Error: null string");
        return false;
    }

    size_t len = strlen(str);
    if (len > max_len) {
        snprintf(error, error_len, "Error: string too long (max %zu chars)", max_len);
        return false;
    }

    // Check for control characters (except newline/tab which may be ok)
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        if (c < 0x20 && c != '\n' && c != '\t' && c != '\r') {
            snprintf(error, error_len, "Error: invalid character in input");
            return false;
        }
    }

    return true;
}

bool tools_validate_nvs_key(const char *key, char *error, size_t error_len)
{
    if (!key || strlen(key) == 0) {
        snprintf(error, error_len, "Error: empty key");
        return false;
    }

    if (strlen(key) > NVS_MAX_KEY_LEN) {
        snprintf(error, error_len, "Error: key max %d chars", NVS_MAX_KEY_LEN);
        return false;
    }

    for (size_t i = 0; key[i]; i++) {
        char c = key[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            snprintf(error, error_len, "Error: key must be alphanumeric/underscore");
            return false;
        }
    }

    return true;
}

bool tools_validate_user_memory_key(const char *key, char *error, size_t error_len)
{
    if (!memory_keys_is_user_key(key)) {
        snprintf(error, error_len, "Error: key must start with '%s' (user memory only)",
                 USER_MEMORY_KEY_PREFIX);
        return false;
    }
    return true;
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
bool tools_append_fmt(char **ptr, size_t *remaining, const char *fmt, ...)
{
    if (!ptr || !*ptr || !remaining || *remaining == 0 || !fmt) {
        return false;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(*ptr, *remaining, fmt, args);
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
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

bool tools_validate_https_url(const char *url, char *error, size_t error_len)
{
    if (!url || strlen(url) < 10) {
        snprintf(error, error_len, "Error: invalid URL");
        return false;
    }

    // Require HTTPS for security
    if (strncmp(url, "https://", 8) != 0) {
        snprintf(error, error_len, "Error: URL must use HTTPS");
        return false;
    }

    if (strlen(url) > 256) {
        snprintf(error, error_len, "Error: URL too long (max 256)");
        return false;
    }

    return true;
}

bool tools_validate_allowed_gpio_pin(int pin, const char *field_name, char *error, size_t error_len)
{
    if (gpio_policy_pin_is_allowed(pin)) {
        return true;
    }

    if (gpio_policy_pin_forbidden_hint(pin, error, error_len)) {
        return false;
    }

    if (GPIO_ALLOWED_PINS_CSV[0] != '\0') {
        if (field_name && field_name[0] != '\0') {
            snprintf(error, error_len, "Error: %s pin %d not allowed", field_name, pin);
        } else {
            snprintf(error, error_len, "Error: pin %d not allowed", pin);
        }
    } else if (field_name && field_name[0] != '\0') {
        snprintf(error, error_len, "Error: %s pin must be %d-%d", field_name, GPIO_MIN_PIN, GPIO_MAX_PIN);
    } else {
        snprintf(error, error_len, "Error: pin must be %d-%d", GPIO_MIN_PIN, GPIO_MAX_PIN);
    }

    return false;
}
