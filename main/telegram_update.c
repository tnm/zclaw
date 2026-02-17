#include "telegram_update.h"
#include <string.h>
#include <stdlib.h>

bool telegram_extract_max_update_id(const char *buf, int64_t *max_id_out)
{
    if (!buf || !max_id_out) {
        return false;
    }

    bool found = false;
    int64_t max_id = -1;
    const char *cursor = buf;
    const char *needle = "\"update_id\"";

    while ((cursor = strstr(cursor, needle)) != NULL) {
        const char *colon = strchr(cursor, ':');
        if (!colon) {
            break;
        }

        const char *num_start = colon + 1;
        while (*num_start == ' ' || *num_start == '\t') {
            num_start++;
        }

        char *endptr = NULL;
        long long parsed = strtoll(num_start, &endptr, 10);
        if (endptr != num_start && parsed >= 0) {
            if (!found || parsed > max_id) {
                max_id = parsed;
            }
            found = true;
            cursor = endptr;
        } else {
            cursor = colon + 1;
        }
    }

    if (found) {
        *max_id_out = max_id;
        return true;
    }

    return false;
}
