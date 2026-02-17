#ifndef MEMORY_KEYS_H
#define MEMORY_KEYS_H

#include <stdbool.h>

#define USER_MEMORY_KEY_PREFIX "u_"

// Tool memory keys must be user-scoped and start with USER_MEMORY_KEY_PREFIX.
bool memory_keys_is_user_key(const char *key);

// Exact system keys that tools must never access or modify.
bool memory_keys_is_sensitive(const char *key);

#endif // MEMORY_KEYS_H
