#include "form_urlencoded.h"
#include <string.h>
#include <ctype.h>

static int hex_to_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static void url_decode_segment(
    char *dst,
    size_t dst_size,
    const char *src,
    size_t src_len
)
{
    size_t i = 0;
    size_t j = 0;

    if (!dst || dst_size == 0) {
        return;
    }

    while (j < src_len && i < dst_size - 1) {
        char c = src[j];
        if (c == '+' ) {
            dst[i++] = ' ';
            j++;
            continue;
        }
        if (c == '%' && (j + 2) < src_len) {
            int hi = hex_to_nibble(src[j + 1]);
            int lo = hex_to_nibble(src[j + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[i++] = (char)((hi << 4) | lo);
                j += 3;
                continue;
            }
        }

        dst[i++] = c;
        j++;
    }

    dst[i] = '\0';
}

bool form_urlencoded_get_field(
    const char *body,
    const char *field,
    char *value,
    size_t value_len
)
{
    if (!body || !field || !value || value_len == 0) {
        return false;
    }

    size_t field_len = strlen(field);
    const char *cursor = body;

    while (*cursor != '\0') {
        const char *key_start = cursor;
        const char *sep = strchr(key_start, '=');
        const char *amp = strchr(key_start, '&');

        if (!sep || (amp && amp < sep)) {
            return false;
        }

        size_t key_len = (size_t)(sep - key_start);
        if (key_len == field_len && strncmp(key_start, field, field_len) == 0) {
            const char *val_start = sep + 1;
            const char *val_end = amp ? amp : key_start + strlen(key_start);
            size_t val_len = (size_t)(val_end - val_start);
            url_decode_segment(value, value_len, val_start, val_len);
            return true;
        }

        if (!amp) {
            break;
        }
        cursor = amp + 1;
    }

    return false;
}

static bool str_ieq(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

bool form_urlencoded_field_is_truthy(const char *body, const char *field)
{
    char value[8];
    if (!form_urlencoded_get_field(body, field, value, sizeof(value))) {
        return false;
    }

    return strcmp(value, "1") == 0 || str_ieq(value, "true") || str_ieq(value, "yes");
}
