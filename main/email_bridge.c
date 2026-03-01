#include "email_bridge.h"
#include "memory.h"
#include "nvs_keys.h"
#include "text_buffer.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMAIL_BRIDGE_URL_MAX 256
#define EMAIL_BRIDGE_KEY_MAX 128
#define EMAIL_BRIDGE_ENDPOINT_MAX 320
#define EMAIL_BRIDGE_HTTP_TIMEOUT_MS 15000

typedef struct {
    char *buf;
    size_t len;
    size_t max;
    bool truncated;
} email_bridge_http_ctx_t;

static const char *TAG = "email_bridge";

static void normalize_bridge_url(const char *raw, char *out, size_t out_len)
{
    size_t len;

    if (!raw || !out || out_len == 0) {
        return;
    }

    strncpy(out, raw, out_len - 1);
    out[out_len - 1] = '\0';

    len = strlen(out);
    while (len > 0 && out[len - 1] == '/') {
        out[len - 1] = '\0';
        len--;
    }
}

static bool load_bridge_config(char *url_out,
                               size_t url_out_len,
                               char *key_out,
                               size_t key_out_len)
{
    char raw_url[EMAIL_BRIDGE_URL_MAX] = {0};

    if (!memory_get(NVS_KEY_EMAIL_BRIDGE_URL, raw_url, sizeof(raw_url)) || raw_url[0] == '\0') {
        return false;
    }
    if (!memory_get(NVS_KEY_EMAIL_BRIDGE_KEY, key_out, key_out_len) || key_out[0] == '\0') {
        return false;
    }

    normalize_bridge_url(raw_url, url_out, url_out_len);
    return url_out[0] != '\0';
}

static esp_err_t email_bridge_http_event_handler(esp_http_client_event_t *evt)
{
    email_bridge_http_ctx_t *ctx = (email_bridge_http_ctx_t *)evt->user_data;

    if (!ctx) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        bool ok = text_buffer_append(ctx->buf, &ctx->len, ctx->max, (const char *)evt->data, evt->data_len);
        if (!ok && !ctx->truncated) {
            ctx->truncated = true;
            ESP_LOGW(TAG, "Bridge response truncated at %d bytes", (int)(ctx->max - 1));
        }
    }

    return ESP_OK;
}

bool email_bridge_is_configured(void)
{
    char url[EMAIL_BRIDGE_URL_MAX] = {0};
    char key[EMAIL_BRIDGE_KEY_MAX] = {0};
    return load_bridge_config(url, sizeof(url), key, sizeof(key));
}

esp_err_t email_bridge_post_json(const char *path,
                                 const cJSON *payload,
                                 char *response_out,
                                 size_t response_out_len,
                                 int *status_out,
                                 bool *truncated_out)
{
    char bridge_url[EMAIL_BRIDGE_URL_MAX] = {0};
    char bridge_key[EMAIL_BRIDGE_KEY_MAX] = {0};
    char auth_header[EMAIL_BRIDGE_KEY_MAX + 16] = {0};
    char endpoint[EMAIL_BRIDGE_ENDPOINT_MAX] = {0};
    char *payload_json = NULL;
    const char *payload_body = "{}";
    esp_http_client_handle_t client = NULL;
    int status = -1;
    esp_err_t err;
    email_bridge_http_ctx_t ctx = {
        .buf = response_out,
        .len = 0,
        .max = response_out_len,
        .truncated = false,
    };

    if (!response_out || response_out_len == 0 || !path || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    response_out[0] = '\0';
    if (status_out) {
        *status_out = -1;
    }
    if (truncated_out) {
        *truncated_out = false;
    }

    if (!load_bridge_config(bridge_url, sizeof(bridge_url), bridge_key, sizeof(bridge_key))) {
        return ESP_ERR_INVALID_STATE;
    }

    if (path[0] == '/') {
        if (snprintf(endpoint, sizeof(endpoint), "%s%s", bridge_url, path) >= (int)sizeof(endpoint)) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        if (snprintf(endpoint, sizeof(endpoint), "%s/%s", bridge_url, path) >= (int)sizeof(endpoint)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    if (payload) {
        payload_json = cJSON_PrintUnformatted((cJSON *)payload);
        if (!payload_json) {
            return ESP_ERR_NO_MEM;
        }
        payload_body = payload_json;
    }

    esp_http_client_config_t cfg = {
        .url = endpoint,
        .event_handler = email_bridge_http_event_handler,
        .user_data = &ctx,
        .timeout_ms = EMAIL_BRIDGE_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    client = esp_http_client_init(&cfg);
    if (!client) {
        free(payload_json);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (snprintf(auth_header, sizeof(auth_header), "Bearer %s", bridge_key) >= (int)sizeof(auth_header)) {
        esp_http_client_cleanup(client);
        free(payload_json);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "X-Zclaw-Bridge-Key", bridge_key);
    esp_http_client_set_post_field(client, payload_body, (int)strlen(payload_body));

    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);

    if (status_out) {
        *status_out = status;
    }
    if (truncated_out) {
        *truncated_out = ctx.truncated;
    }

    esp_http_client_cleanup(client);
    free(payload_json);

    if (ctx.truncated) {
        return ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (status < 200 || status >= 300) {
        return ESP_FAIL;
    }

    return ESP_OK;
}
