#ifndef FORM_URLENCODED_H
#define FORM_URLENCODED_H

#include <stdbool.h>
#include <stddef.h>

// Extracts a URL-encoded form field value. Returns true if the key exists.
bool form_urlencoded_get_field(
    const char *body,
    const char *field,
    char *value,
    size_t value_len
);

// Reads a field and treats "1", "true", and "yes" as true.
bool form_urlencoded_field_is_truthy(const char *body, const char *field);

#endif // FORM_URLENCODED_H
