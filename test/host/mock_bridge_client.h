#ifndef MOCK_BRIDGE_CLIENT_H
#define MOCK_BRIDGE_CLIENT_H

#include <stdbool.h>
#include "esp_err.h"

void mock_bridge_client_reset(void);
void mock_bridge_client_set_configured(bool configured);
void mock_bridge_client_set_response(esp_err_t err, int status, bool truncated, const char *response);
const char *mock_bridge_client_last_path(void);
const char *mock_bridge_client_last_payload(void);
int mock_bridge_client_post_calls(void);

#endif // MOCK_BRIDGE_CLIENT_H
