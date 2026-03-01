#include "mock_email_bridge.h"
#include "email_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOCK_BRIDGE_PATH_MAX 127
#define MOCK_BRIDGE_PAYLOAD_MAX 4095
#define MOCK_BRIDGE_RESPONSE_MAX 4095

static bool s_configured = true;
static esp_err_t s_response_err = ESP_OK;
static int s_response_status = 200;
static bool s_response_truncated = false;
static char s_response_body[MOCK_BRIDGE_RESPONSE_MAX + 1] = "{}";
static char s_last_path[MOCK_BRIDGE_PATH_MAX + 1] = {0};
static char s_last_payload[MOCK_BRIDGE_PAYLOAD_MAX + 1] = {0};
static int s_post_calls = 0;

void mock_email_bridge_reset(void)
{
    s_configured = true;
    s_response_err = ESP_OK;
    s_response_status = 200;
    s_response_truncated = false;
    snprintf(s_response_body, sizeof(s_response_body), "{}");
    s_last_path[0] = '\0';
    s_last_payload[0] = '\0';
    s_post_calls = 0;
}

void mock_email_bridge_set_configured(bool configured)
{
    s_configured = configured;
}

void mock_email_bridge_set_response(esp_err_t err, int status, bool truncated, const char *response)
{
    s_response_err = err;
    s_response_status = status;
    s_response_truncated = truncated;
    snprintf(s_response_body, sizeof(s_response_body), "%s", response ? response : "");
}

const char *mock_email_bridge_last_path(void)
{
    return s_last_path;
}

const char *mock_email_bridge_last_payload(void)
{
    return s_last_payload;
}

int mock_email_bridge_post_calls(void)
{
    return s_post_calls;
}

bool email_bridge_is_configured(void)
{
    return s_configured;
}

esp_err_t email_bridge_post_json(const char *path,
                                 const cJSON *payload,
                                 char *response_out,
                                 size_t response_out_len,
                                 int *status_out,
                                 bool *truncated_out)
{
    char *payload_json = NULL;

    if (!path || path[0] == '\0' || !response_out || response_out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_post_calls++;
    snprintf(s_last_path, sizeof(s_last_path), "%s", path);

    if (payload) {
        payload_json = cJSON_PrintUnformatted((cJSON *)payload);
    }
    snprintf(s_last_payload, sizeof(s_last_payload), "%s", payload_json ? payload_json : "");
    free(payload_json);

    if (status_out) {
        *status_out = s_response_status;
    }
    if (truncated_out) {
        *truncated_out = s_response_truncated;
    }

    snprintf(response_out, response_out_len, "%s", s_response_body);
    return s_response_err;
}
