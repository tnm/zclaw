#ifndef TOOLS_COMMON_H
#define TOOLS_COMMON_H

#include <stdbool.h>
#include <stddef.h>

bool tools_validate_string_input(const char *str, size_t max_len, char *error, size_t error_len);
bool tools_validate_nvs_key(const char *key, char *error, size_t error_len);
bool tools_validate_user_memory_key(const char *key, char *error, size_t error_len);
bool tools_append_fmt(char **ptr, size_t *remaining, const char *fmt, ...);
bool tools_validate_https_url(const char *url, char *error, size_t error_len);

#endif // TOOLS_COMMON_H
