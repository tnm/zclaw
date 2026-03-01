#include "tools_handlers.h"
#include "email_bridge.h"
#include "tools_common.h"
#include "cJSON.h"
#include "esp_err.h"
#include <stdio.h>
#include <string.h>

#define EMAIL_TO_MAX_LEN 256
#define EMAIL_SUBJECT_MAX_LEN 160
#define EMAIL_BODY_MAX_LEN 2000
#define EMAIL_LIST_LABEL_MAX_LEN 64
#define EMAIL_MESSAGE_ID_MAX_LEN 256
#define EMAIL_TOOL_RESPONSE_BUF_SIZE 2048

static const char *EMAIL_SEND_PATH = "/v1/email/send";
static const char *EMAIL_LIST_PATH = "/v1/email/list";
static const char *EMAIL_READ_PATH = "/v1/email/read";

static void first_line_or_fallback(const char *input, char *out, size_t out_len, const char *fallback)
{
    size_t i = 0;

    if (!out || out_len == 0) {
        return;
    }

    out[0] = '\0';
    if (!input || input[0] == '\0') {
        snprintf(out, out_len, "%s", fallback ? fallback : "");
        return;
    }

    while (input[i] != '\0' && input[i] != '\n' && input[i] != '\r' && i < out_len - 1) {
        out[i] = input[i];
        i++;
    }
    out[i] = '\0';

    if (out[0] == '\0' && fallback) {
        snprintf(out, out_len, "%s", fallback);
    }
}

static bool get_required_string_field(const cJSON *input,
                                      const char *name,
                                      size_t max_len,
                                      char *result,
                                      size_t result_len,
                                      const char **value_out)
{
    const cJSON *field;
    char err[96];

    if (!input || !name || !value_out) {
        snprintf(result, result_len, "Error: internal input validation failed");
        return false;
    }

    field = cJSON_GetObjectItemCaseSensitive((cJSON *)input, name);
    if (!field || !cJSON_IsString(field) || !field->valuestring || field->valuestring[0] == '\0') {
        snprintf(result, result_len, "Error: '%s' is required", name);
        return false;
    }

    if (!tools_validate_string_input(field->valuestring, max_len, err, sizeof(err))) {
        snprintf(result, result_len, "Error: invalid '%s' (%s)", name, err + 7);
        return false;
    }

    *value_out = field->valuestring;
    return true;
}

static bool check_email_bridge_ready(char *result, size_t result_len)
{
    if (email_bridge_is_configured()) {
        return true;
    }

    snprintf(result, result_len,
             "Error: email bridge is not configured. Provision email_bridge_url and email_bridge_key first.");
    return false;
}

static bool report_bridge_call_result(const char *operation,
                                      esp_err_t err,
                                      int status,
                                      const char *response,
                                      bool truncated,
                                      char *result,
                                      size_t result_len)
{
    char detail[192];

    if (err == ESP_OK) {
        return true;
    }

    if (truncated) {
        snprintf(result, result_len,
                 "Error: %s response exceeded buffer limits. Increase bridge response size or reduce payload.",
                 operation);
        return false;
    }

    first_line_or_fallback(response, detail, sizeof(detail), "no error details from bridge");
    snprintf(result, result_len,
             "Error: %s failed (status=%d, err=%s): %s",
             operation,
             status,
             esp_err_to_name(err),
             detail);
    return false;
}

bool tools_email_send_handler(const cJSON *input, char *result, size_t result_len)
{
    const char *to = NULL;
    const char *subject = NULL;
    const char *body = NULL;
    cJSON *req = NULL;
    char response[EMAIL_TOOL_RESPONSE_BUF_SIZE];
    bool truncated = false;
    int status = -1;
    esp_err_t err;
    cJSON *root = NULL;
    cJSON *summary_json;
    cJSON *message_json;

    if (!check_email_bridge_ready(result, result_len)) {
        return false;
    }

    if (!get_required_string_field(input, "to", EMAIL_TO_MAX_LEN, result, result_len, &to) ||
        !get_required_string_field(input, "subject", EMAIL_SUBJECT_MAX_LEN, result, result_len, &subject) ||
        !get_required_string_field(input, "body", EMAIL_BODY_MAX_LEN, result, result_len, &body)) {
        return false;
    }

    if (!strchr(to, '@')) {
        snprintf(result, result_len, "Error: 'to' must be an email address");
        return false;
    }

    req = cJSON_CreateObject();
    if (!req) {
        snprintf(result, result_len, "Error: out of memory");
        return false;
    }
    cJSON_AddStringToObject(req, "to", to);
    cJSON_AddStringToObject(req, "subject", subject);
    cJSON_AddStringToObject(req, "body", body);

    err = email_bridge_post_json(EMAIL_SEND_PATH, req, response, sizeof(response), &status, &truncated);
    cJSON_Delete(req);

    if (!report_bridge_call_result("email_send", err, status, response, truncated, result, result_len)) {
        return false;
    }

    root = cJSON_Parse(response);
    if (!root) {
        first_line_or_fallback(response, result, result_len, "Email send request accepted.");
        return true;
    }

    summary_json = cJSON_GetObjectItemCaseSensitive(root, "summary");
    message_json = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (summary_json && cJSON_IsString(summary_json) && summary_json->valuestring) {
        snprintf(result, result_len, "%s", summary_json->valuestring);
    } else if (message_json && cJSON_IsString(message_json) && message_json->valuestring) {
        snprintf(result, result_len, "%s", message_json->valuestring);
    } else {
        snprintf(result, result_len, "Email send request accepted.");
    }

    cJSON_Delete(root);
    return true;
}

bool tools_email_list_handler(const cJSON *input, char *result, size_t result_len)
{
    const cJSON *label_json = NULL;
    const cJSON *max_json = NULL;
    const cJSON *unread_only_json = NULL;
    cJSON *req = NULL;
    cJSON *root = NULL;
    cJSON *summary_json = NULL;
    cJSON *items_json = NULL;
    char response[EMAIL_TOOL_RESPONSE_BUF_SIZE];
    bool truncated = false;
    int status = -1;
    int max_items = 5;
    bool unread_only = false;
    esp_err_t err;
    char *ptr = result;
    size_t remaining = result_len;

    if (!check_email_bridge_ready(result, result_len)) {
        return false;
    }

    if (input) {
        label_json = cJSON_GetObjectItemCaseSensitive((cJSON *)input, "label");
        if (label_json) {
            char err_msg[96];
            if (!cJSON_IsString(label_json) || !label_json->valuestring ||
                !tools_validate_string_input(label_json->valuestring, EMAIL_LIST_LABEL_MAX_LEN,
                                             err_msg, sizeof(err_msg))) {
                snprintf(result, result_len, "Error: 'label' must be a short string");
                return false;
            }
        }

        max_json = cJSON_GetObjectItemCaseSensitive((cJSON *)input, "max");
        if (max_json) {
            if (!cJSON_IsNumber(max_json)) {
                snprintf(result, result_len, "Error: 'max' must be an integer between 1 and 20");
                return false;
            }
            max_items = max_json->valueint;
            if (max_items < 1 || max_items > 20) {
                snprintf(result, result_len, "Error: 'max' must be between 1 and 20");
                return false;
            }
        }

        unread_only_json = cJSON_GetObjectItemCaseSensitive((cJSON *)input, "unread_only");
        if (unread_only_json) {
            if (!cJSON_IsBool(unread_only_json)) {
                snprintf(result, result_len, "Error: 'unread_only' must be boolean");
                return false;
            }
            unread_only = cJSON_IsTrue(unread_only_json);
        }
    }

    req = cJSON_CreateObject();
    if (!req) {
        snprintf(result, result_len, "Error: out of memory");
        return false;
    }
    cJSON_AddNumberToObject(req, "max", max_items);
    cJSON_AddBoolToObject(req, "unread_only", unread_only);
    if (label_json && cJSON_IsString(label_json) && label_json->valuestring && label_json->valuestring[0] != '\0') {
        cJSON_AddStringToObject(req, "label", label_json->valuestring);
    }

    err = email_bridge_post_json(EMAIL_LIST_PATH, req, response, sizeof(response), &status, &truncated);
    cJSON_Delete(req);

    if (!report_bridge_call_result("email_list", err, status, response, truncated, result, result_len)) {
        return false;
    }

    root = cJSON_Parse(response);
    if (!root) {
        first_line_or_fallback(response, result, result_len, "Email list request completed.");
        return true;
    }

    summary_json = cJSON_GetObjectItemCaseSensitive(root, "summary");
    if (summary_json && cJSON_IsString(summary_json) && summary_json->valuestring) {
        snprintf(result, result_len, "%s", summary_json->valuestring);
        cJSON_Delete(root);
        return true;
    }

    items_json = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (items_json && cJSON_IsArray(items_json)) {
        int count = cJSON_GetArraySize(items_json);
        if (count <= 0) {
            snprintf(result, result_len, "No emails found.");
            cJSON_Delete(root);
            return true;
        }

        tools_append_fmt(&ptr, &remaining, "Email list (%d):", count);
        for (int i = 0; i < count && i < 5; i++) {
            const cJSON *item = cJSON_GetArrayItem(items_json, i);
            const cJSON *id_json = cJSON_GetObjectItemCaseSensitive((cJSON *)item, "id");
            const cJSON *from_json = cJSON_GetObjectItemCaseSensitive((cJSON *)item, "from");
            const cJSON *subject_json = cJSON_GetObjectItemCaseSensitive((cJSON *)item, "subject");
            const char *id = (id_json && cJSON_IsString(id_json) && id_json->valuestring) ? id_json->valuestring : "?";
            const char *from = (from_json && cJSON_IsString(from_json) && from_json->valuestring) ? from_json->valuestring : "?";
            const char *subject = (subject_json && cJSON_IsString(subject_json) && subject_json->valuestring) ? subject_json->valuestring : "(no subject)";
            tools_append_fmt(&ptr, &remaining, "\n%d) [%s] %s — %s", i + 1, id, from, subject);
        }
        cJSON_Delete(root);
        return true;
    }

    first_line_or_fallback(response, result, result_len, "Email list request completed.");
    cJSON_Delete(root);
    return true;
}

bool tools_email_read_handler(const cJSON *input, char *result, size_t result_len)
{
    const char *id = NULL;
    const cJSON *max_chars_json = NULL;
    int max_chars = 1200;
    cJSON *req = NULL;
    cJSON *root = NULL;
    cJSON *summary_json = NULL;
    cJSON *subject_json = NULL;
    cJSON *from_json = NULL;
    cJSON *body_json = NULL;
    const char *subject = "(no subject)";
    const char *from = "(unknown sender)";
    const char *body = "";
    char response[EMAIL_TOOL_RESPONSE_BUF_SIZE];
    bool truncated = false;
    int status = -1;
    esp_err_t err;
    char body_preview[512];
    size_t body_len;

    if (!check_email_bridge_ready(result, result_len)) {
        return false;
    }

    if (!get_required_string_field(input, "id", EMAIL_MESSAGE_ID_MAX_LEN, result, result_len, &id)) {
        return false;
    }

    max_chars_json = input ? cJSON_GetObjectItemCaseSensitive((cJSON *)input, "max_chars") : NULL;
    if (max_chars_json) {
        if (!cJSON_IsNumber(max_chars_json)) {
            snprintf(result, result_len, "Error: 'max_chars' must be an integer between 200 and 4000");
            return false;
        }
        max_chars = max_chars_json->valueint;
        if (max_chars < 200 || max_chars > 4000) {
            snprintf(result, result_len, "Error: 'max_chars' must be between 200 and 4000");
            return false;
        }
    }

    req = cJSON_CreateObject();
    if (!req) {
        snprintf(result, result_len, "Error: out of memory");
        return false;
    }
    cJSON_AddStringToObject(req, "id", id);
    cJSON_AddNumberToObject(req, "max_chars", max_chars);

    err = email_bridge_post_json(EMAIL_READ_PATH, req, response, sizeof(response), &status, &truncated);
    cJSON_Delete(req);

    if (!report_bridge_call_result("email_read", err, status, response, truncated, result, result_len)) {
        return false;
    }

    root = cJSON_Parse(response);
    if (!root) {
        first_line_or_fallback(response, result, result_len, "Email read request completed.");
        return true;
    }

    summary_json = cJSON_GetObjectItemCaseSensitive(root, "summary");
    if (summary_json && cJSON_IsString(summary_json) && summary_json->valuestring) {
        snprintf(result, result_len, "%s", summary_json->valuestring);
        cJSON_Delete(root);
        return true;
    }

    subject_json = cJSON_GetObjectItemCaseSensitive(root, "subject");
    from_json = cJSON_GetObjectItemCaseSensitive(root, "from");
    body_json = cJSON_GetObjectItemCaseSensitive(root, "body_text");
    if (subject_json && cJSON_IsString(subject_json) && subject_json->valuestring) {
        subject = subject_json->valuestring;
    }
    if (from_json && cJSON_IsString(from_json) && from_json->valuestring) {
        from = from_json->valuestring;
    }
    if (body_json && cJSON_IsString(body_json) && body_json->valuestring) {
        body = body_json->valuestring;
    }

    body_len = strlen(body);
    if (body_len >= sizeof(body_preview)) {
        memcpy(body_preview, body, sizeof(body_preview) - 4);
        body_preview[sizeof(body_preview) - 4] = '.';
        body_preview[sizeof(body_preview) - 3] = '.';
        body_preview[sizeof(body_preview) - 2] = '.';
        body_preview[sizeof(body_preview) - 1] = '\0';
    } else {
        snprintf(body_preview, sizeof(body_preview), "%s", body);
    }

    snprintf(result, result_len,
             "Email %s\nFrom: %s\nSubject: %s\nBody: %s",
             id, from, subject, body_preview);
    cJSON_Delete(root);
    return true;
}
