#ifndef TEXT_BUFFER_H
#define TEXT_BUFFER_H

#include <stddef.h>
#include <stdbool.h>

// Appends data to a NUL-terminated buffer.
// Returns true if all bytes were appended, false if truncated or invalid input.
bool text_buffer_append(char *buf, size_t *len, size_t max_len, const char *data, size_t data_len);

#endif // TEXT_BUFFER_H
