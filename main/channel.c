#include "channel.h"
#include "config.h"
#include "messages.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#if CONFIG_ZCLAW_CHANNEL_UART
#include "driver/uart.h"
#else
#include "driver/usb_serial_jtag.h"
#endif
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "channel";

static QueueHandle_t s_input_queue;
static QueueHandle_t s_output_queue;

#define LLM_BRIDGE_REQ_PREFIX  "__zclaw_llm_req__:"
#define LLM_BRIDGE_RESP_PREFIX "__zclaw_llm_resp__:"

#if CONFIG_ZCLAW_EMULATOR_LIVE_LLM
typedef struct {
    size_t payload_len;
    bool truncated;
} llm_bridge_response_t;

static QueueHandle_t s_llm_bridge_queue = NULL;
static char s_llm_bridge_payload[LLM_RESPONSE_BUF_SIZE];
#endif

#if CONFIG_ZCLAW_CHANNEL_UART
#define CHANNEL_UART_PORT UART_NUM_0
#define CHANNEL_UART_BAUDRATE 115200

static void channel_io_init(void)
{
    const uart_config_t uart_cfg = {
        .baud_rate = CHANNEL_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(CHANNEL_UART_PORT,
                                        CHANNEL_RX_BUF_SIZE * 2,
                                        CHANNEL_RX_BUF_SIZE * 2,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CHANNEL_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(CHANNEL_UART_PORT,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
}

static int channel_io_read_byte(uint8_t *byte, TickType_t timeout_ticks)
{
    return uart_read_bytes(CHANNEL_UART_PORT, byte, 1, timeout_ticks);
}

static void channel_io_write_bytes(const uint8_t *data, size_t len, TickType_t timeout_ticks)
{
    (void)timeout_ticks;
    if (len > 0) {
        uart_write_bytes(CHANNEL_UART_PORT, (const char *)data, len);
    }
}
#else
static void channel_io_init(void)
{
    usb_serial_jtag_driver_config_t config = {
        .rx_buffer_size = CHANNEL_RX_BUF_SIZE,
        .tx_buffer_size = CHANNEL_RX_BUF_SIZE,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
}

static int channel_io_read_byte(uint8_t *byte, TickType_t timeout_ticks)
{
    return usb_serial_jtag_read_bytes(byte, 1, timeout_ticks);
}

static void channel_io_write_bytes(const uint8_t *data, size_t len, TickType_t timeout_ticks)
{
    usb_serial_jtag_write_bytes((uint8_t *)data, len, timeout_ticks);
}
#endif

static void channel_write_normalized_text(const char *text, TickType_t timeout_ticks)
{
    const char *segment_start = text;
    const char *cursor = text;

    while (*cursor != '\0') {
        if (*cursor == '\n') {
            size_t segment_len = (size_t)(cursor - segment_start);
            if (segment_len > 0) {
                channel_io_write_bytes((const uint8_t *)segment_start, segment_len, timeout_ticks);
            }

            if (cursor > segment_start && *(cursor - 1) == '\r') {
                channel_io_write_bytes((const uint8_t *)"\n", 1, timeout_ticks);
            } else {
                channel_io_write_bytes((const uint8_t *)"\r\n", 2, timeout_ticks);
            }

            cursor++;
            segment_start = cursor;
            continue;
        }

        cursor++;
    }

    if (cursor > segment_start) {
        channel_io_write_bytes((const uint8_t *)segment_start,
                               (size_t)(cursor - segment_start),
                               timeout_ticks);
    }
}

void channel_init(void)
{
    channel_io_init();
#if CONFIG_ZCLAW_CHANNEL_UART
    ESP_LOGI(TAG, "UART0 channel initialized");
#else
    ESP_LOGI(TAG, "USB serial initialized");
#endif
}

// Read task: accumulate characters into lines, push to input queue
static void channel_read_task(void *arg)
{
    char line_buf[CHANNEL_RX_BUF_SIZE];
    int line_pos = 0;
    uint8_t byte;
#if CONFIG_ZCLAW_EMULATOR_LIVE_LLM
    char held_echo[sizeof(LLM_BRIDGE_RESP_PREFIX)];
    int held_echo_len = 0;
    bool prefix_check_active = true;
    bool bridge_line = false;
    size_t bridge_payload_pos = 0;
    bool bridge_payload_truncated = false;
    const size_t bridge_prefix_len = strlen(LLM_BRIDGE_RESP_PREFIX);
#endif

    while (1) {
        int len = channel_io_read_byte(&byte, portMAX_DELAY);
        if (len > 0) {
            if (byte == '\r' || byte == '\n') {
#if CONFIG_ZCLAW_EMULATOR_LIVE_LLM
                if (bridge_line) {
                    if (s_llm_bridge_queue) {
                        llm_bridge_response_t resp = {
                            .payload_len = bridge_payload_pos,
                            .truncated = bridge_payload_truncated,
                        };
                        s_llm_bridge_payload[bridge_payload_pos] = '\0';
                        xQueueOverwrite(s_llm_bridge_queue, &resp);
                    }

                    line_pos = 0;
                    held_echo_len = 0;
                    prefix_check_active = true;
                    bridge_line = false;
                    bridge_payload_pos = 0;
                    bridge_payload_truncated = false;
                    continue;
                }

                if (held_echo_len > 0) {
                    for (int i = 0; i < held_echo_len; i++) {
                        if (line_pos < CHANNEL_RX_BUF_SIZE - 1) {
                            line_buf[line_pos++] = held_echo[i];
                        }
                        channel_io_write_bytes((const uint8_t *)&held_echo[i], 1, portMAX_DELAY);
                    }
                    held_echo_len = 0;
                    prefix_check_active = false;
                }
#endif

                if (line_pos > 0) {
                    line_buf[line_pos] = '\0';
                    channel_io_write_bytes((const uint8_t *)"\r\n", 2, portMAX_DELAY);

                    // Push to input queue
                    channel_msg_t msg;
                    strncpy(msg.text, line_buf, CHANNEL_RX_BUF_SIZE - 1);
                    msg.text[CHANNEL_RX_BUF_SIZE - 1] = '\0';

                    if (xQueueSend(s_input_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
                        ESP_LOGW(TAG, "Input queue full, dropping message");
                    }
                }

                line_pos = 0;
#if CONFIG_ZCLAW_EMULATOR_LIVE_LLM
                held_echo_len = 0;
                prefix_check_active = true;
                bridge_line = false;
                bridge_payload_pos = 0;
                bridge_payload_truncated = false;
#endif
            } else if (byte == 0x7F || byte == 0x08) {
#if CONFIG_ZCLAW_EMULATOR_LIVE_LLM
                if (bridge_line) {
                    continue;
                }

                if (held_echo_len > 0) {
                    held_echo_len--;
                    channel_io_write_bytes((const uint8_t *)"\b \b", 3, portMAX_DELAY);
                    continue;
                }
#endif

                // Backspace
                if (line_pos > 0) {
                    line_pos--;
                    channel_io_write_bytes((const uint8_t *)"\b \b", 3, portMAX_DELAY);
                }
#if CONFIG_ZCLAW_EMULATOR_LIVE_LLM
            } else {
                if (bridge_line) {
                    if (bridge_payload_pos < sizeof(s_llm_bridge_payload) - 1) {
                        s_llm_bridge_payload[bridge_payload_pos++] = (char)byte;
                    } else {
                        bridge_payload_truncated = true;
                    }
                    continue;
                }

                if (prefix_check_active) {
                    if ((size_t)held_echo_len < bridge_prefix_len &&
                        byte == (uint8_t)LLM_BRIDGE_RESP_PREFIX[held_echo_len]) {
                        if (held_echo_len < (int)sizeof(held_echo)) {
                            held_echo[held_echo_len++] = (char)byte;
                        }
                        if ((size_t)held_echo_len == bridge_prefix_len) {
                            bridge_line = true;
                            prefix_check_active = false;
                            bridge_payload_pos = 0;
                            bridge_payload_truncated = false;
                        }
                        continue;
                    }

                    prefix_check_active = false;
                    if (held_echo_len > 0) {
                        for (int i = 0; i < held_echo_len; i++) {
                            if (line_pos < CHANNEL_RX_BUF_SIZE - 1) {
                                line_buf[line_pos++] = held_echo[i];
                            }
                            channel_io_write_bytes((const uint8_t *)&held_echo[i], 1, portMAX_DELAY);
                        }
                        held_echo_len = 0;
                    }
                }

                if (line_pos < CHANNEL_RX_BUF_SIZE - 1) {
                    line_buf[line_pos++] = (char)byte;
                }
                channel_io_write_bytes(&byte, 1, portMAX_DELAY);
#else
            } else {
                if (line_pos < CHANNEL_RX_BUF_SIZE - 1) {
                    line_buf[line_pos++] = (char)byte;
                }
                channel_io_write_bytes(&byte, 1, portMAX_DELAY);
#endif
            }
        }
    }
}

// Write task: watch output queue, print responses
static void channel_write_task(void *arg)
{
    channel_output_msg_t msg;

    while (1) {
        if (xQueueReceive(s_output_queue, &msg, portMAX_DELAY) == pdTRUE) {
            // Print response with newlines
            const char *text = msg.text;
            channel_write_normalized_text(text, portMAX_DELAY);
            channel_io_write_bytes((const uint8_t *)"\r\n\r\n", 4, portMAX_DELAY);
        }
    }
}

esp_err_t channel_start(QueueHandle_t input_queue, QueueHandle_t output_queue)
{
    TaskHandle_t read_task = NULL;
    TaskHandle_t write_task = NULL;

    if (!input_queue || !output_queue) {
        ESP_LOGE(TAG, "Invalid queues for channel startup");
        return ESP_ERR_INVALID_ARG;
    }

    s_input_queue = input_queue;
    s_output_queue = output_queue;

#if CONFIG_ZCLAW_EMULATOR_LIVE_LLM
    s_llm_bridge_queue = xQueueCreate(1, sizeof(llm_bridge_response_t));
    if (!s_llm_bridge_queue) {
        ESP_LOGE(TAG, "Failed to create LLM bridge queue");
        return ESP_ERR_NO_MEM;
    }
#endif

    if (xTaskCreate(channel_read_task, "ch_read", CHANNEL_TASK_STACK_SIZE, NULL,
                    CHANNEL_TASK_PRIORITY, &read_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create channel read task");
#if CONFIG_ZCLAW_EMULATOR_LIVE_LLM
        vQueueDelete(s_llm_bridge_queue);
        s_llm_bridge_queue = NULL;
#endif
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(channel_write_task, "ch_write", CHANNEL_TASK_STACK_SIZE, NULL,
                    CHANNEL_TASK_PRIORITY, &write_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create channel write task");
        if (read_task) {
            vTaskDelete(read_task);
        }
#if CONFIG_ZCLAW_EMULATOR_LIVE_LLM
        vQueueDelete(s_llm_bridge_queue);
        s_llm_bridge_queue = NULL;
#endif
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Channel tasks started");
    return ESP_OK;
}

void channel_write(const char *text)
{
    channel_write_normalized_text(text, portMAX_DELAY);
}

esp_err_t channel_llm_bridge_exchange(const char *request_json,
                                      char *response_json,
                                      size_t response_json_size,
                                      uint32_t timeout_ms)
{
#if !CONFIG_ZCLAW_EMULATOR_LIVE_LLM
    (void)request_json;
    (void)response_json;
    (void)response_json_size;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#else
    TickType_t timeout_ticks;
    llm_bridge_response_t resp;

    if (!request_json || !response_json || response_json_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_llm_bridge_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ticks == 0) {
        timeout_ticks = pdMS_TO_TICKS(1);
    }

    xQueueReset(s_llm_bridge_queue);

    channel_io_write_bytes((const uint8_t *)LLM_BRIDGE_REQ_PREFIX,
                           strlen(LLM_BRIDGE_REQ_PREFIX), portMAX_DELAY);
    channel_io_write_bytes((const uint8_t *)request_json, strlen(request_json), portMAX_DELAY);
    channel_io_write_bytes((const uint8_t *)"\n", 1, portMAX_DELAY);

    if (xQueueReceive(s_llm_bridge_queue, &resp, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (resp.truncated) {
        ESP_LOGE(TAG, "LLM bridge payload exceeded %d bytes", LLM_RESPONSE_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }

    if (resp.payload_len >= response_json_size) {
        ESP_LOGE(TAG, "LLM bridge payload too large");
        return ESP_ERR_NO_MEM;
    }

    memcpy(response_json, s_llm_bridge_payload, resp.payload_len + 1);
    return ESP_OK;
#endif
}
