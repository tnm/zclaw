/*
 * Host tests for email tool handlers.
 */

#include <stdio.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "mock_email_bridge.h"
#include "tools_handlers.h"

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while(0)
#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("  FAIL: '%s' != '%s' (line %d)\n", (a), (b), __LINE__); \
        return 1; \
    } \
} while(0)
#define ASSERT_STR_CONTAINS(haystack, needle) do { \
    if (strstr((haystack), (needle)) == NULL) { \
        printf("  FAIL: expected substring '%s' in '%s' (line %d)\n", (needle), (haystack), __LINE__); \
        return 1; \
    } \
} while(0)

static void test_setup(void)
{
    mock_email_bridge_reset();
    mock_email_bridge_set_configured(true);
}

static cJSON *parse_last_payload(void)
{
    const char *payload = mock_email_bridge_last_payload();
    if (!payload || payload[0] == '\0') {
        return NULL;
    }
    return cJSON_Parse(payload);
}

static void fill_chars(char *buf, size_t len, char ch)
{
    size_t i;
    for (i = 0; i < len; i++) {
        buf[i] = ch;
    }
    buf[len] = '\0';
}

TEST(send_rejects_unconfigured_bridge)
{
    cJSON *input = cJSON_Parse("{\"to\":\"a@example.com\",\"subject\":\"s\",\"body\":\"b\"}");
    char result[512] = {0};

    test_setup();
    mock_email_bridge_set_configured(false);
    ASSERT(input != NULL);
    ASSERT(!tools_email_send_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "email bridge is not configured");
    ASSERT(mock_email_bridge_post_calls() == 0);

    cJSON_Delete(input);
    return 0;
}

TEST(send_requires_to)
{
    cJSON *input = cJSON_Parse("{\"subject\":\"s\",\"body\":\"b\"}");
    char result[512] = {0};

    test_setup();
    ASSERT(input != NULL);
    ASSERT(!tools_email_send_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "Error: 'to' is required");

    cJSON_Delete(input);
    return 0;
}

TEST(send_rejects_invalid_email_address)
{
    cJSON *input = cJSON_Parse("{\"to\":\"invalid\",\"subject\":\"s\",\"body\":\"b\"}");
    char result[512] = {0};

    test_setup();
    ASSERT(input != NULL);
    ASSERT(!tools_email_send_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "Error: 'to' must be an email address");

    cJSON_Delete(input);
    return 0;
}

TEST(send_forwards_payload_and_uses_summary)
{
    cJSON *input = cJSON_Parse("{\"to\":\"a@example.com\",\"subject\":\"hello\",\"body\":\"body\"}");
    cJSON *payload = NULL;
    cJSON *to_json;
    cJSON *subject_json;
    cJSON *body_json;
    char result[512] = {0};

    test_setup();
    mock_email_bridge_set_response(ESP_OK, 202, false, "{\"summary\":\"queued\"}");
    ASSERT(input != NULL);
    ASSERT(tools_email_send_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "queued");
    ASSERT_STR_EQ(mock_email_bridge_last_path(), "/v1/email/send");
    ASSERT(mock_email_bridge_post_calls() == 1);

    payload = parse_last_payload();
    ASSERT(payload != NULL);
    to_json = cJSON_GetObjectItemCaseSensitive(payload, "to");
    subject_json = cJSON_GetObjectItemCaseSensitive(payload, "subject");
    body_json = cJSON_GetObjectItemCaseSensitive(payload, "body");
    ASSERT(cJSON_IsString(to_json) && strcmp(to_json->valuestring, "a@example.com") == 0);
    ASSERT(cJSON_IsString(subject_json) && strcmp(subject_json->valuestring, "hello") == 0);
    ASSERT(cJSON_IsString(body_json) && strcmp(body_json->valuestring, "body") == 0);

    cJSON_Delete(payload);
    cJSON_Delete(input);
    return 0;
}

TEST(send_uses_message_when_summary_missing)
{
    cJSON *input = cJSON_Parse("{\"to\":\"a@example.com\",\"subject\":\"hello\",\"body\":\"body\"}");
    char result[512] = {0};

    test_setup();
    mock_email_bridge_set_response(ESP_OK, 200, false, "{\"message\":\"accepted\"}");
    ASSERT(input != NULL);
    ASSERT(tools_email_send_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "accepted");

    cJSON_Delete(input);
    return 0;
}

TEST(send_uses_default_when_json_has_no_summary_or_message)
{
    cJSON *input = cJSON_Parse("{\"to\":\"a@example.com\",\"subject\":\"hello\",\"body\":\"body\"}");
    char result[512] = {0};

    test_setup();
    mock_email_bridge_set_response(ESP_OK, 200, false, "{\"ok\":true}");
    ASSERT(input != NULL);
    ASSERT(tools_email_send_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "Email send request accepted.");

    cJSON_Delete(input);
    return 0;
}

TEST(send_uses_first_line_for_non_json_response)
{
    cJSON *input = cJSON_Parse("{\"to\":\"a@example.com\",\"subject\":\"hello\",\"body\":\"body\"}");
    char result[512] = {0};

    test_setup();
    mock_email_bridge_set_response(ESP_OK, 200, false, "queued on provider\ntrace-id:123");
    ASSERT(input != NULL);
    ASSERT(tools_email_send_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "queued on provider");

    cJSON_Delete(input);
    return 0;
}

TEST(send_reports_bridge_error_with_status_and_detail)
{
    cJSON *input = cJSON_Parse("{\"to\":\"a@example.com\",\"subject\":\"hello\",\"body\":\"body\"}");
    char result[512] = {0};

    test_setup();
    mock_email_bridge_set_response(ESP_FAIL, 500, false, "upstream timeout\ntrace");
    ASSERT(input != NULL);
    ASSERT(!tools_email_send_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "email_send failed");
    ASSERT_STR_CONTAINS(result, "status=500");
    ASSERT_STR_CONTAINS(result, "ESP_FAIL");
    ASSERT_STR_CONTAINS(result, "upstream timeout");

    cJSON_Delete(input);
    return 0;
}

TEST(send_reports_truncated_bridge_response)
{
    cJSON *input = cJSON_Parse("{\"to\":\"a@example.com\",\"subject\":\"hello\",\"body\":\"body\"}");
    char result[512] = {0};

    test_setup();
    mock_email_bridge_set_response(ESP_FAIL, 502, true, "ignored");
    ASSERT(input != NULL);
    ASSERT(!tools_email_send_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "response exceeded buffer limits");

    cJSON_Delete(input);
    return 0;
}

TEST(list_defaults_when_input_is_null)
{
    cJSON *payload = NULL;
    cJSON *max_json;
    cJSON *unread_only_json;
    char result[512] = {0};

    test_setup();
    mock_email_bridge_set_response(ESP_OK, 200, false, "{\"summary\":\"3 inbox emails\"}");
    ASSERT(tools_email_list_handler(NULL, result, sizeof(result)));
    ASSERT_STR_EQ(result, "3 inbox emails");
    ASSERT_STR_EQ(mock_email_bridge_last_path(), "/v1/email/list");
    ASSERT(mock_email_bridge_post_calls() == 1);

    payload = parse_last_payload();
    ASSERT(payload != NULL);
    max_json = cJSON_GetObjectItemCaseSensitive(payload, "max");
    unread_only_json = cJSON_GetObjectItemCaseSensitive(payload, "unread_only");
    ASSERT(cJSON_IsNumber(max_json) && max_json->valueint == 5);
    ASSERT(cJSON_IsBool(unread_only_json) && !cJSON_IsTrue(unread_only_json));
    ASSERT(cJSON_GetObjectItemCaseSensitive(payload, "label") == NULL);

    cJSON_Delete(payload);
    return 0;
}

TEST(list_rejects_label_not_string)
{
    cJSON *input = cJSON_Parse("{\"label\":123}");
    char result[512] = {0};

    test_setup();
    ASSERT(input != NULL);
    ASSERT(!tools_email_list_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "Error: 'label' must be a short string");

    cJSON_Delete(input);
    return 0;
}

TEST(list_rejects_label_too_long)
{
    char long_label[80];
    cJSON *input = cJSON_CreateObject();
    char result[512] = {0};

    fill_chars(long_label, 70, 'x');
    test_setup();
    ASSERT(input != NULL);
    cJSON_AddStringToObject(input, "label", long_label);
    ASSERT(!tools_email_list_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "Error: 'label' must be a short string");

    cJSON_Delete(input);
    return 0;
}

TEST(list_rejects_max_not_number)
{
    cJSON *input = cJSON_Parse("{\"max\":\"5\"}");
    char result[512] = {0};

    test_setup();
    ASSERT(input != NULL);
    ASSERT(!tools_email_list_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "Error: 'max' must be an integer between 1 and 20");

    cJSON_Delete(input);
    return 0;
}

TEST(list_rejects_max_out_of_range)
{
    cJSON *input = cJSON_Parse("{\"max\":21}");
    char result[512] = {0};

    test_setup();
    ASSERT(input != NULL);
    ASSERT(!tools_email_list_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "Error: 'max' must be between 1 and 20");

    cJSON_Delete(input);
    return 0;
}

TEST(list_rejects_unread_only_not_bool)
{
    cJSON *input = cJSON_Parse("{\"unread_only\":\"yes\"}");
    char result[512] = {0};

    test_setup();
    ASSERT(input != NULL);
    ASSERT(!tools_email_list_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "Error: 'unread_only' must be boolean");

    cJSON_Delete(input);
    return 0;
}

TEST(list_forwards_optional_fields)
{
    cJSON *input = cJSON_Parse("{\"label\":\"INBOX\",\"max\":7,\"unread_only\":true}");
    cJSON *payload = NULL;
    cJSON *label_json;
    cJSON *max_json;
    cJSON *unread_only_json;
    char result[512] = {0};

    test_setup();
    mock_email_bridge_set_response(ESP_OK, 200, false, "{\"summary\":\"ok\"}");
    ASSERT(input != NULL);
    ASSERT(tools_email_list_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "ok");
    ASSERT_STR_EQ(mock_email_bridge_last_path(), "/v1/email/list");

    payload = parse_last_payload();
    ASSERT(payload != NULL);
    label_json = cJSON_GetObjectItemCaseSensitive(payload, "label");
    max_json = cJSON_GetObjectItemCaseSensitive(payload, "max");
    unread_only_json = cJSON_GetObjectItemCaseSensitive(payload, "unread_only");
    ASSERT(cJSON_IsString(label_json) && strcmp(label_json->valuestring, "INBOX") == 0);
    ASSERT(cJSON_IsNumber(max_json) && max_json->valueint == 7);
    ASSERT(cJSON_IsBool(unread_only_json) && cJSON_IsTrue(unread_only_json));

    cJSON_Delete(payload);
    cJSON_Delete(input);
    return 0;
}

TEST(list_renders_items_and_limits_to_five_lines)
{
    cJSON *input = cJSON_CreateObject();
    char result[2048] = {0};

    test_setup();
    ASSERT(input != NULL);
    mock_email_bridge_set_response(
        ESP_OK,
        200,
        false,
        "{\"items\":["
        "{\"id\":\"id1\",\"from\":\"f1@example.com\",\"subject\":\"s1\"},"
        "{\"id\":\"id2\",\"from\":\"f2@example.com\",\"subject\":\"s2\"},"
        "{\"id\":\"id3\",\"from\":\"f3@example.com\",\"subject\":\"s3\"},"
        "{\"id\":\"id4\",\"from\":\"f4@example.com\",\"subject\":\"s4\"},"
        "{\"id\":\"id5\",\"from\":\"f5@example.com\",\"subject\":\"s5\"},"
        "{\"id\":\"id6\",\"from\":\"f6@example.com\",\"subject\":\"s6\"}"
        "]}");

    ASSERT(tools_email_list_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "Email list (6):");
    ASSERT_STR_CONTAINS(result, "1) [id1] f1@example.com");
    ASSERT_STR_CONTAINS(result, "5) [id5] f5@example.com");
    ASSERT(strstr(result, "id6") == NULL);

    cJSON_Delete(input);
    return 0;
}

TEST(list_reports_no_emails_for_empty_items)
{
    cJSON *input = cJSON_CreateObject();
    char result[512] = {0};

    test_setup();
    ASSERT(input != NULL);
    mock_email_bridge_set_response(ESP_OK, 200, false, "{\"items\":[]}");
    ASSERT(tools_email_list_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "No emails found.");

    cJSON_Delete(input);
    return 0;
}

TEST(list_uses_first_line_for_non_json_response)
{
    cJSON *input = cJSON_CreateObject();
    char result[512] = {0};

    test_setup();
    ASSERT(input != NULL);
    mock_email_bridge_set_response(ESP_OK, 200, false, "list complete\ntrace");
    ASSERT(tools_email_list_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "list complete");

    cJSON_Delete(input);
    return 0;
}

TEST(list_reports_bridge_error)
{
    cJSON *input = cJSON_CreateObject();
    char result[512] = {0};

    test_setup();
    ASSERT(input != NULL);
    mock_email_bridge_set_response(ESP_FAIL, 503, false, "bridge down");
    ASSERT(!tools_email_list_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "email_list failed");
    ASSERT_STR_CONTAINS(result, "status=503");
    ASSERT_STR_CONTAINS(result, "bridge down");

    cJSON_Delete(input);
    return 0;
}

TEST(list_reports_truncated_bridge_response)
{
    cJSON *input = cJSON_CreateObject();
    char result[512] = {0};

    test_setup();
    ASSERT(input != NULL);
    mock_email_bridge_set_response(ESP_FAIL, 500, true, "ignored");
    ASSERT(!tools_email_list_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "response exceeded buffer limits");

    cJSON_Delete(input);
    return 0;
}

TEST(read_requires_id)
{
    cJSON *input = cJSON_Parse("{\"max_chars\":300}");
    char result[512] = {0};

    test_setup();
    ASSERT(input != NULL);
    ASSERT(!tools_email_read_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "Error: 'id' is required");

    cJSON_Delete(input);
    return 0;
}

TEST(read_rejects_max_chars_not_number)
{
    cJSON *input = cJSON_Parse("{\"id\":\"abc\",\"max_chars\":\"300\"}");
    char result[512] = {0};

    test_setup();
    ASSERT(input != NULL);
    ASSERT(!tools_email_read_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "Error: 'max_chars' must be an integer between 200 and 4000");

    cJSON_Delete(input);
    return 0;
}

TEST(read_rejects_max_chars_out_of_range)
{
    cJSON *input = cJSON_Parse("{\"id\":\"abc\",\"max_chars\":100}");
    char result[512] = {0};

    test_setup();
    ASSERT(input != NULL);
    ASSERT(!tools_email_read_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "Error: 'max_chars' must be between 200 and 4000");

    cJSON_Delete(input);
    return 0;
}

TEST(read_forwards_payload_and_formats_email_view)
{
    cJSON *input = cJSON_Parse("{\"id\":\"msg-1\"}");
    cJSON *payload = NULL;
    cJSON *id_json;
    cJSON *max_chars_json;
    char result[1024] = {0};

    test_setup();
    mock_email_bridge_set_response(
        ESP_OK,
        200,
        false,
        "{\"from\":\"sender@example.com\",\"subject\":\"Test\",\"body_text\":\"Hello world\"}");
    ASSERT(input != NULL);
    ASSERT(tools_email_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "Email msg-1");
    ASSERT_STR_CONTAINS(result, "From: sender@example.com");
    ASSERT_STR_CONTAINS(result, "Subject: Test");
    ASSERT_STR_CONTAINS(result, "Body: Hello world");
    ASSERT_STR_EQ(mock_email_bridge_last_path(), "/v1/email/read");

    payload = parse_last_payload();
    ASSERT(payload != NULL);
    id_json = cJSON_GetObjectItemCaseSensitive(payload, "id");
    max_chars_json = cJSON_GetObjectItemCaseSensitive(payload, "max_chars");
    ASSERT(cJSON_IsString(id_json) && strcmp(id_json->valuestring, "msg-1") == 0);
    ASSERT(cJSON_IsNumber(max_chars_json) && max_chars_json->valueint == 1200);

    cJSON_Delete(payload);
    cJSON_Delete(input);
    return 0;
}

TEST(read_uses_summary_when_present)
{
    cJSON *input = cJSON_Parse("{\"id\":\"msg-1\"}");
    char result[512] = {0};

    test_setup();
    mock_email_bridge_set_response(ESP_OK, 200, false, "{\"summary\":\"message unavailable\"}");
    ASSERT(input != NULL);
    ASSERT(tools_email_read_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "message unavailable");

    cJSON_Delete(input);
    return 0;
}

TEST(read_truncates_long_body_preview)
{
    char long_body[700];
    char response[1024];
    cJSON *input = cJSON_Parse("{\"id\":\"msg-2\"}");
    char result[2048] = {0};

    fill_chars(long_body, 640, 'a');
    snprintf(response, sizeof(response),
             "{\"from\":\"sender@example.com\",\"subject\":\"Long\",\"body_text\":\"%s\"}",
             long_body);

    test_setup();
    mock_email_bridge_set_response(ESP_OK, 200, false, response);
    ASSERT(input != NULL);
    ASSERT(tools_email_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "Email msg-2");
    ASSERT_STR_CONTAINS(result, "Body: ");
    ASSERT_STR_CONTAINS(result, "...");

    cJSON_Delete(input);
    return 0;
}

TEST(read_uses_first_line_for_non_json_response)
{
    cJSON *input = cJSON_Parse("{\"id\":\"msg-3\"}");
    char result[512] = {0};

    test_setup();
    mock_email_bridge_set_response(ESP_OK, 200, false, "read complete\ntrace");
    ASSERT(input != NULL);
    ASSERT(tools_email_read_handler(input, result, sizeof(result)));
    ASSERT_STR_EQ(result, "read complete");

    cJSON_Delete(input);
    return 0;
}

TEST(read_reports_bridge_error)
{
    cJSON *input = cJSON_Parse("{\"id\":\"msg-4\"}");
    char result[512] = {0};

    test_setup();
    mock_email_bridge_set_response(ESP_FAIL, 404, false, "not found");
    ASSERT(input != NULL);
    ASSERT(!tools_email_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "email_read failed");
    ASSERT_STR_CONTAINS(result, "status=404");
    ASSERT_STR_CONTAINS(result, "not found");

    cJSON_Delete(input);
    return 0;
}

TEST(read_reports_truncated_bridge_response)
{
    cJSON *input = cJSON_Parse("{\"id\":\"msg-5\"}");
    char result[512] = {0};

    test_setup();
    mock_email_bridge_set_response(ESP_FAIL, 502, true, "ignored");
    ASSERT(input != NULL);
    ASSERT(!tools_email_read_handler(input, result, sizeof(result)));
    ASSERT_STR_CONTAINS(result, "response exceeded buffer limits");

    cJSON_Delete(input);
    return 0;
}

int test_tools_email_all(void)
{
    int failures = 0;

    printf("\nEmail Tool Tests:\n");

#define RUN_TEST(name) do { \
    printf("  " #name "... "); \
    if (test_##name() == 0) { \
        printf("OK\n"); \
    } else { \
        failures++; \
    } \
} while(0)

    RUN_TEST(send_rejects_unconfigured_bridge);
    RUN_TEST(send_requires_to);
    RUN_TEST(send_rejects_invalid_email_address);
    RUN_TEST(send_forwards_payload_and_uses_summary);
    RUN_TEST(send_uses_message_when_summary_missing);
    RUN_TEST(send_uses_default_when_json_has_no_summary_or_message);
    RUN_TEST(send_uses_first_line_for_non_json_response);
    RUN_TEST(send_reports_bridge_error_with_status_and_detail);
    RUN_TEST(send_reports_truncated_bridge_response);

    RUN_TEST(list_defaults_when_input_is_null);
    RUN_TEST(list_rejects_label_not_string);
    RUN_TEST(list_rejects_label_too_long);
    RUN_TEST(list_rejects_max_not_number);
    RUN_TEST(list_rejects_max_out_of_range);
    RUN_TEST(list_rejects_unread_only_not_bool);
    RUN_TEST(list_forwards_optional_fields);
    RUN_TEST(list_renders_items_and_limits_to_five_lines);
    RUN_TEST(list_reports_no_emails_for_empty_items);
    RUN_TEST(list_uses_first_line_for_non_json_response);
    RUN_TEST(list_reports_bridge_error);
    RUN_TEST(list_reports_truncated_bridge_response);

    RUN_TEST(read_requires_id);
    RUN_TEST(read_rejects_max_chars_not_number);
    RUN_TEST(read_rejects_max_chars_out_of_range);
    RUN_TEST(read_forwards_payload_and_formats_email_view);
    RUN_TEST(read_uses_summary_when_present);
    RUN_TEST(read_truncates_long_body_preview);
    RUN_TEST(read_uses_first_line_for_non_json_response);
    RUN_TEST(read_reports_bridge_error);
    RUN_TEST(read_reports_truncated_bridge_response);

#undef RUN_TEST

    return failures;
}
