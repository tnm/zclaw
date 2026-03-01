#ifndef MOCK_EMAIL_BRIDGE_H
#define MOCK_EMAIL_BRIDGE_H

#include <stdbool.h>
#include "esp_err.h"

void mock_email_bridge_reset(void);
void mock_email_bridge_set_configured(bool configured);
void mock_email_bridge_set_response(esp_err_t err, int status, bool truncated, const char *response);
const char *mock_email_bridge_last_path(void);
const char *mock_email_bridge_last_payload(void);
int mock_email_bridge_post_calls(void);

#endif // MOCK_EMAIL_BRIDGE_H
