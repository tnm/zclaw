#ifndef MOCK_RATELIMIT_H
#define MOCK_RATELIMIT_H

#include <stdbool.h>

void mock_ratelimit_reset(void);
void mock_ratelimit_set_allow(bool allow, const char *reason);
int mock_ratelimit_record_count(void);

#endif // MOCK_RATELIMIT_H
