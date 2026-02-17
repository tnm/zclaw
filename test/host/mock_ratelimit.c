#include "ratelimit.h"
#include "mock_ratelimit.h"
#include <string.h>

static bool s_allow = true;
static int s_record_count = 0;
static char s_reason[128] = "Rate limited";

void mock_ratelimit_reset(void)
{
    s_allow = true;
    s_record_count = 0;
    strncpy(s_reason, "Rate limited", sizeof(s_reason) - 1);
    s_reason[sizeof(s_reason) - 1] = '\0';
}

void mock_ratelimit_set_allow(bool allow, const char *reason)
{
    s_allow = allow;
    if (reason && reason[0] != '\0') {
        strncpy(s_reason, reason, sizeof(s_reason) - 1);
        s_reason[sizeof(s_reason) - 1] = '\0';
    }
}

int mock_ratelimit_record_count(void)
{
    return s_record_count;
}

void ratelimit_init(void)
{
}

bool ratelimit_check(char *reason, size_t reason_len)
{
    if (s_allow) {
        return true;
    }
    if (reason && reason_len > 0) {
        strncpy(reason, s_reason, reason_len - 1);
        reason[reason_len - 1] = '\0';
    }
    return false;
}

void ratelimit_record_request(void)
{
    s_record_count++;
}

int ratelimit_get_requests_today(void)
{
    return 0;
}

int ratelimit_get_requests_this_hour(void)
{
    return 0;
}

void ratelimit_reset_daily(void)
{
}
