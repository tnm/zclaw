#ifndef RATELIMIT_H
#define RATELIMIT_H

#include <stdbool.h>
#include <stddef.h>

// Initialize rate limiter (loads state from NVS)
void ratelimit_init(void);

// Check if a request is allowed. Returns true if allowed, false if rate limited.
// If denied, writes reason to `reason` buffer.
bool ratelimit_check(char *reason, size_t reason_len);

// Record that a request was made (call after successful LLM response)
void ratelimit_record_request(void);

// Get current usage stats
int ratelimit_get_requests_today(void);
int ratelimit_get_requests_this_hour(void);

// Reset daily counter (called at midnight by cron or manually)
void ratelimit_reset_daily(void);

#endif // RATELIMIT_H
