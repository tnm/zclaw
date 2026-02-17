#ifndef SECURITY_H
#define SECURITY_H

#include <stdbool.h>

// Returns true when a configuration key likely contains secrets.
bool security_key_is_sensitive(const char *key);

#endif // SECURITY_H
