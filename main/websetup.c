#include "websetup.h"
#include "config.h"
#include "memory.h"
#include "nvs_keys.h"
#include "form_urlencoded.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "nvs.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

static const char *TAG = "websetup";

static httpd_handle_t s_server = NULL;
static const uint16_t WIFI_SCAN_MAX_RESULTS = 20;
static const size_t WIFI_SSID_MAX_LEN = 33;  // 32 chars + null
static const size_t WEBSETUP_FORM_MAX_LEN = 2048;

extern const uint8_t setup_html_start[] asm("_binary_setup_html_start");
extern const uint8_t setup_html_end[] asm("_binary_setup_html_end");
extern const uint8_t success_html_start[] asm("_binary_success_html_start");
extern const uint8_t success_html_end[] asm("_binary_success_html_end");

static esp_err_t send_embedded_html(httpd_req_t *req, const uint8_t *start, const uint8_t *end)
{
    size_t len = (size_t)(end - start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)start, len);
    return ESP_OK;
}

static esp_err_t send_json_response(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (status) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void drain_request_body(httpd_req_t *req, size_t content_len)
{
    char scratch[128];
    size_t remaining = content_len;

    while (remaining > 0) {
        size_t to_read = remaining > sizeof(scratch) ? sizeof(scratch) : remaining;
        int ret = httpd_req_recv(req, scratch, to_read);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            break;
        }
        remaining -= (size_t)ret;
    }
}

static bool store_or_clear_field(
    const char *nvs_key,
    bool present,
    const char *value,
    bool clear_requested
)
{
    if (present && value[0] != '\0') {
        return memory_set(nvs_key, value) == ESP_OK;
    } else if (clear_requested) {
        esp_err_t err = memory_delete(nvs_key);
        return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
    }
    return true;
}

// GET / - serve setup page
static esp_err_t root_get_handler(httpd_req_t *req)
{
    return send_embedded_html(req, setup_html_start, setup_html_end);
}

// GET /networks - scan nearby WiFi SSIDs
static esp_err_t networks_get_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_config = {0};
    wifi_ap_record_t ap_records[20];
    uint16_t ap_count = WIFI_SCAN_MAX_RESULTS;
    char unique_ssids[20][33] = {0};
    uint16_t unique_count = 0;
    cJSON *root = NULL;
    cJSON *ssids = NULL;
    char *json = NULL;
    esp_err_t err;

    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;

    err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan start failed: %s", esp_err_to_name(err));
        goto scan_fail;
    }

    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan read failed: %s", esp_err_to_name(err));
        goto scan_fail;
    }

    root = cJSON_CreateObject();
    ssids = cJSON_AddArrayToObject(root, "ssids");
    if (!root || !ssids) {
        goto scan_fail;
    }

    for (uint16_t i = 0; i < ap_count && unique_count < WIFI_SCAN_MAX_RESULTS; i++) {
        const char *ssid = (const char *)ap_records[i].ssid;
        bool duplicate = false;

        if (ssid[0] == '\0') {
            continue;
        }

        for (uint16_t j = 0; j < unique_count; j++) {
            if (strcmp(unique_ssids[j], ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        strncpy(unique_ssids[unique_count], ssid, WIFI_SSID_MAX_LEN - 1);
        unique_ssids[unique_count][WIFI_SSID_MAX_LEN - 1] = '\0';
        cJSON_AddItemToArray(ssids, cJSON_CreateString(unique_ssids[unique_count]));
        unique_count++;
    }

    cJSON_AddNumberToObject(root, "count", unique_count);
    json = cJSON_PrintUnformatted(root);
    if (!json) {
        goto scan_fail;
    }

    ESP_LOGI(TAG, "Scanned %u APs, returning %u SSIDs", ap_count, unique_count);
    send_json_response(req, NULL, json);

    free(json);
    cJSON_Delete(root);
    return ESP_OK;

scan_fail:
    if (json) {
        free(json);
    }
    if (root) {
        cJSON_Delete(root);
    }

    send_json_response(req, "503 Service Unavailable",
                       "{\"error\":\"wifi_scan_unavailable\",\"ssids\":[],\"count\":0}");
    return ESP_FAIL;
}

// POST /save - save configuration
static esp_err_t save_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0) {
        send_json_response(req, "400 Bad Request", "{\"error\":\"empty_body\"}");
        return ESP_FAIL;
    }

    if ((size_t)req->content_len >= WEBSETUP_FORM_MAX_LEN) {
        ESP_LOGW(TAG, "Rejected oversized setup payload: %u bytes", (unsigned)req->content_len);
        drain_request_body(req, (size_t)req->content_len);
        send_json_response(req, "413 Payload Too Large", "{\"error\":\"payload_too_large\"}");
        return ESP_FAIL;
    }

    char body[WEBSETUP_FORM_MAX_LEN];
    size_t total_read = 0;
    size_t expected = (size_t)req->content_len;

    while (total_read < expected) {
        int ret = httpd_req_recv(req, body + total_read, expected - total_read);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            ESP_LOGE(TAG, "Failed to receive config payload");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        total_read += (size_t)ret;
    }
    body[total_read] = '\0';

    char ssid[64] = {0};
    char pass[64] = {0};
    char backend[16] = {0};
    char apikey[128] = {0};
    char model[64] = {0};
    char tgtoken[64] = {0};
    char tgchatid[24] = {0};

    bool has_ssid = form_urlencoded_get_field(body, "ssid", ssid, sizeof(ssid));
    bool has_pass = form_urlencoded_get_field(body, "pass", pass, sizeof(pass));
    bool has_backend = form_urlencoded_get_field(body, "backend", backend, sizeof(backend));
    bool has_apikey = form_urlencoded_get_field(body, "apikey", apikey, sizeof(apikey));
    bool has_model = form_urlencoded_get_field(body, "model", model, sizeof(model));
    bool has_tgtoken = form_urlencoded_get_field(body, "tgtoken", tgtoken, sizeof(tgtoken));
    bool has_tgchatid = form_urlencoded_get_field(body, "tgchatid", tgchatid, sizeof(tgchatid));

    ESP_LOGI(TAG, "Received config: ssid=%s backend=%s", ssid, backend);

    // Save updates and allow explicit clears via clear_* flags.
    bool save_ok = true;
    save_ok &= store_or_clear_field(NVS_KEY_WIFI_SSID, has_ssid, ssid,
                                    form_urlencoded_field_is_truthy(body, "clear_ssid"));
    save_ok &= store_or_clear_field(NVS_KEY_WIFI_PASS, has_pass, pass,
                                    form_urlencoded_field_is_truthy(body, "clear_pass"));
    save_ok &= store_or_clear_field(NVS_KEY_LLM_BACKEND, has_backend, backend,
                                    form_urlencoded_field_is_truthy(body, "clear_backend"));
    save_ok &= store_or_clear_field(NVS_KEY_API_KEY, has_apikey, apikey,
                                    form_urlencoded_field_is_truthy(body, "clear_apikey"));
    save_ok &= store_or_clear_field(NVS_KEY_LLM_MODEL, has_model, model,
                                    form_urlencoded_field_is_truthy(body, "clear_model"));
    save_ok &= store_or_clear_field(NVS_KEY_TG_CHAT_ID, has_tgchatid, tgchatid,
                                    form_urlencoded_field_is_truthy(body, "clear_tgchatid"));
    save_ok &= store_or_clear_field(NVS_KEY_TG_TOKEN, has_tgtoken, tgtoken,
                                    form_urlencoded_field_is_truthy(body, "clear_tgtoken"));

    if (!save_ok) {
        ESP_LOGE(TAG, "Failed to persist one or more setup fields");
        send_json_response(req, "500 Internal Server Error",
                           "{\"error\":\"persist_failed\"}");
        return ESP_FAIL;
    }

    // Send success page
    if (send_embedded_html(req, success_html_start, success_html_end) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Schedule restart
    ESP_LOGI(TAG, "Configuration saved, restarting in 2 seconds...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}

// Captive portal redirect for any other path
static esp_err_t captive_portal_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t start_server(void)
{
    esp_err_t err;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 10;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    // Register handlers
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    err = httpd_register_uri_handler(s_server, &root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register '/' handler: %s", esp_err_to_name(err));
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
    };
    err = httpd_register_uri_handler(s_server, &save);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register '/save' handler: %s", esp_err_to_name(err));
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    httpd_uri_t networks = {
        .uri = "/networks",
        .method = HTTP_GET,
        .handler = networks_get_handler,
    };
    err = httpd_register_uri_handler(s_server, &networks);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register '/networks' handler: %s", esp_err_to_name(err));
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    // Wildcard for captive portal
    httpd_uri_t wildcard = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = captive_portal_handler,
    };
    err = httpd_register_uri_handler(s_server, &wildcard);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register wildcard handler: %s", esp_err_to_name(err));
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

// Generate random alphanumeric password
static void generate_ap_password(char *buf, size_t len)
{
    const char charset[] = "abcdefghijkmnpqrstuvwxyz23456789";  // Avoid confusing chars
    uint32_t random;
    for (size_t i = 0; i < len - 1; i++) {
        esp_fill_random(&random, sizeof(random));
        buf[i] = charset[random % (sizeof(charset) - 1)];
    }
    buf[len - 1] = '\0';
}

esp_err_t websetup_start_ap_mode(void)
{
    // Generate random password for AP security
    char ap_password[9];  // 8 chars + null
    generate_ap_password(ap_password, sizeof(ap_password));

    ESP_LOGI(TAG, "Starting AP mode: %s", WIFI_AP_SSID);

    // Initialize netif and wifi if not already done
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = false },
        },
    };
    memcpy(wifi_config.ap.password, ap_password, sizeof(ap_password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Print password prominently to serial
    printf("\n");
    printf("============================================\n");
    printf("  zclaw Setup WiFi\n");
    printf("============================================\n");
    printf("  Network:  %s\n", WIFI_AP_SSID);
    printf("  Password: %s\n", ap_password);
    printf("  URL:      http://192.168.4.1\n");
    printf("============================================\n");
    printf("\n");

    return start_server();
}

esp_err_t websetup_start_sta_mode(void)
{
    return start_server();
}

void websetup_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}

bool websetup_is_configured(void)
{
    char ssid[64];
    return memory_get(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid)) && ssid[0] != '\0';
}

bool websetup_get_wifi_ssid(char *ssid, size_t len)
{
    return memory_get(NVS_KEY_WIFI_SSID, ssid, len);
}

bool websetup_get_wifi_pass(char *pass, size_t len)
{
    return memory_get(NVS_KEY_WIFI_PASS, pass, len);
}
