#include "text_buffer.h"
#include <string.h>

bool text_buffer_append(char *buf, size_t *len, size_t max_len, const char *data, size_t data_len)
{
    if (!buf || !len || max_len == 0) {
        return false;
    }
    if (data_len > 0 && !data) {
        return false;
    }

    if (*len >= max_len) {
        buf[max_len - 1] = '\0';
        return false;
    }

    size_t available = (max_len - 1) - *len;
    size_t to_copy = data_len < available ? data_len : available;

    if (to_copy > 0) {
        memcpy(buf + *len, data, to_copy);
    }

    *len += to_copy;
    buf[*len] = '\0';

    return to_copy == data_len;
}
