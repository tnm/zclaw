#ifndef ESP_HTTP_CLIENT_H
#define ESP_HTTP_CLIENT_H

#include "mock_esp.h"

typedef enum {
    HTTP_TRANSPORT_UNKNOWN = 0,
    HTTP_TRANSPORT_OVER_TCP,
    HTTP_TRANSPORT_OVER_SSL,
} esp_http_client_transport_t;

typedef struct {
    int status_code;
    int sock_errno;
    esp_http_client_transport_t transport_type;
} mock_esp_http_client_t;

typedef mock_esp_http_client_t *esp_http_client_handle_t;

static inline int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    return client ? client->status_code : -1;
}

static inline int esp_http_client_get_errno(esp_http_client_handle_t client)
{
    return client ? client->sock_errno : 0;
}

static inline esp_http_client_transport_t esp_http_client_get_transport_type(
    esp_http_client_handle_t client)
{
    return client ? client->transport_type : HTTP_TRANSPORT_UNKNOWN;
}

#endif // ESP_HTTP_CLIENT_H
