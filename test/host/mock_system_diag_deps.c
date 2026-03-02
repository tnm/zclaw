#include "cron.h"
#include "ota.h"

#include <stdio.h>

const char *ota_get_version(void)
{
    return "test-version";
}

bool cron_is_time_synced(void)
{
    return true;
}

void cron_get_timezone(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }
    snprintf(buf, buf_len, "UTC0");
}

void cron_get_timezone_abbrev(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }
    snprintf(buf, buf_len, "UTC");
}
