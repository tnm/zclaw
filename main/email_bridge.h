#ifndef EMAIL_BRIDGE_H
#define EMAIL_BRIDGE_H

#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

// Returns true when both email bridge URL and key are provisioned.
bool email_bridge_is_configured(void);

// POST JSON payload to configured bridge endpoint path (e.g. "/v1/email/send").
// response_out always receives a null-terminated string (possibly empty).
esp_err_t email_bridge_post_json(const char *path,
                                 const cJSON *payload,
                                 char *response_out,
                                 size_t response_out_len,
                                 int *status_out,
                                 bool *truncated_out);

#endif // EMAIL_BRIDGE_H
