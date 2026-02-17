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

    while (1) {
        int len = channel_io_read_byte(&byte, portMAX_DELAY);
        if (len > 0) {
            // Echo character back
            channel_io_write_bytes(&byte, 1, portMAX_DELAY);

            if (byte == '\r' || byte == '\n') {
                if (line_pos > 0) {
                    line_buf[line_pos] = '\0';

                    // Send newline for terminal
                    channel_io_write_bytes((const uint8_t *)"\r\n", 2, portMAX_DELAY);

                    // Push to input queue
                    channel_msg_t msg;
                    strncpy(msg.text, line_buf, CHANNEL_RX_BUF_SIZE - 1);
                    msg.text[CHANNEL_RX_BUF_SIZE - 1] = '\0';

                    if (xQueueSend(s_input_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
                        ESP_LOGW(TAG, "Input queue full, dropping message");
                    }

                    line_pos = 0;
                }
            } else if (byte == 0x7F || byte == 0x08) {
                // Backspace
                if (line_pos > 0) {
                    line_pos--;
                    channel_io_write_bytes((const uint8_t *)"\b \b", 3, portMAX_DELAY);
                }
            } else if (line_pos < CHANNEL_RX_BUF_SIZE - 1) {
                line_buf[line_pos++] = byte;
            }
        }
    }
}

// Write task: watch output queue, print responses
static void channel_write_task(void *arg)
{
    channel_msg_t msg;

    while (1) {
        if (xQueueReceive(s_output_queue, &msg, portMAX_DELAY) == pdTRUE) {
            // Print response with newlines
            const char *text = msg.text;
            size_t len = strlen(text);
            channel_io_write_bytes((const uint8_t *)text, len, portMAX_DELAY);
            channel_io_write_bytes((const uint8_t *)"\r\n\r\n", 4, portMAX_DELAY);
        }
    }
}

void channel_start(QueueHandle_t input_queue, QueueHandle_t output_queue)
{
    s_input_queue = input_queue;
    s_output_queue = output_queue;

    TaskHandle_t read_task = NULL;
    if (xTaskCreate(channel_read_task, "ch_read", CHANNEL_TASK_STACK_SIZE, NULL,
                    CHANNEL_TASK_PRIORITY, &read_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create channel read task");
        return;
    }

    if (xTaskCreate(channel_write_task, "ch_write", CHANNEL_TASK_STACK_SIZE, NULL,
                    CHANNEL_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create channel write task");
        vTaskDelete(read_task);
        return;
    }

    ESP_LOGI(TAG, "Channel tasks started");
}

void channel_write(const char *text)
{
    size_t len = strlen(text);
    channel_io_write_bytes((const uint8_t *)text, len, portMAX_DELAY);
}
