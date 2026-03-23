#include "driver/i2c.h"

#include <string.h>
#include <stddef.h>

#define I2C_TEST_MAX_BYTES 64
#define I2C_TEST_MAX_WRITES 8

static int s_param_config_calls = 0;
static int s_driver_install_calls = 0;
static int s_driver_delete_calls = 0;
static int s_write_to_device_calls = 0;
static esp_err_t s_cmd_begin_result = ESP_FAIL;
static esp_err_t s_write_to_device_result = ESP_OK;
static esp_err_t s_write_to_device_results[I2C_TEST_MAX_WRITES];
static int s_write_to_device_result_count = 0;
static int s_write_to_device_result_index = 0;
static esp_err_t s_read_from_device_result = ESP_OK;
static esp_err_t s_write_read_device_result = ESP_OK;
static uint8_t s_read_data[I2C_TEST_MAX_BYTES];
static size_t s_read_data_len = 0;
static uint8_t s_last_address = 0;
static uint8_t s_last_write[I2C_TEST_MAX_BYTES];
static size_t s_last_write_len = 0;
static size_t s_last_read_len = 0;
static uint8_t s_write_addresses[I2C_TEST_MAX_WRITES];
static uint8_t s_write_history[I2C_TEST_MAX_WRITES][I2C_TEST_MAX_BYTES];
static size_t s_write_history_len[I2C_TEST_MAX_WRITES];

void i2c_test_reset(void)
{
    s_param_config_calls = 0;
    s_driver_install_calls = 0;
    s_driver_delete_calls = 0;
    s_write_to_device_calls = 0;
    s_cmd_begin_result = ESP_FAIL;
    s_write_to_device_result = ESP_OK;
    memset(s_write_to_device_results, 0, sizeof(s_write_to_device_results));
    s_write_to_device_result_count = 0;
    s_write_to_device_result_index = 0;
    s_read_from_device_result = ESP_OK;
    s_write_read_device_result = ESP_OK;
    memset(s_read_data, 0, sizeof(s_read_data));
    s_read_data_len = 0;
    s_last_address = 0;
    memset(s_last_write, 0, sizeof(s_last_write));
    s_last_write_len = 0;
    s_last_read_len = 0;
    memset(s_write_addresses, 0, sizeof(s_write_addresses));
    memset(s_write_history, 0, sizeof(s_write_history));
    memset(s_write_history_len, 0, sizeof(s_write_history_len));
}

void i2c_test_set_cmd_begin_result(esp_err_t result)
{
    s_cmd_begin_result = result;
}

void i2c_test_set_write_to_device_result(esp_err_t result)
{
    s_write_to_device_result = result;
}

bool i2c_test_push_write_to_device_result(esp_err_t result)
{
    if (s_write_to_device_result_count >= I2C_TEST_MAX_WRITES) {
        return false;
    }
    s_write_to_device_results[s_write_to_device_result_count++] = result;
    return true;
}

void i2c_test_set_read_from_device_result(esp_err_t result)
{
    s_read_from_device_result = result;
}

void i2c_test_set_write_read_device_result(esp_err_t result)
{
    s_write_read_device_result = result;
}

void i2c_test_set_read_data(const uint8_t *data, size_t len)
{
    size_t copy_len = len;

    if (copy_len > sizeof(s_read_data)) {
        copy_len = sizeof(s_read_data);
    }
    memset(s_read_data, 0, sizeof(s_read_data));
    if (data && copy_len > 0) {
        memcpy(s_read_data, data, copy_len);
    }
    s_read_data_len = copy_len;
}

int i2c_test_get_param_config_calls(void)
{
    return s_param_config_calls;
}

int i2c_test_get_driver_install_calls(void)
{
    return s_driver_install_calls;
}

int i2c_test_get_driver_delete_calls(void)
{
    return s_driver_delete_calls;
}

int i2c_test_get_write_to_device_calls(void)
{
    return s_write_to_device_calls;
}

uint8_t i2c_test_get_last_address(void)
{
    return s_last_address;
}

size_t i2c_test_get_last_write_length(void)
{
    return s_last_write_len;
}

size_t i2c_test_get_last_read_length(void)
{
    return s_last_read_len;
}

uint8_t i2c_test_get_last_write_byte(size_t index)
{
    if (index >= s_last_write_len) {
        return 0;
    }
    return s_last_write[index];
}

uint8_t i2c_test_get_write_to_device_address(size_t index)
{
    if (index >= (size_t)s_write_to_device_calls || index >= I2C_TEST_MAX_WRITES) {
        return 0;
    }
    return s_write_addresses[index];
}

size_t i2c_test_get_write_to_device_length(size_t index)
{
    if (index >= (size_t)s_write_to_device_calls || index >= I2C_TEST_MAX_WRITES) {
        return 0;
    }
    return s_write_history_len[index];
}

uint8_t i2c_test_get_write_to_device_byte(size_t call_index, size_t byte_index)
{
    if (call_index >= (size_t)s_write_to_device_calls ||
        call_index >= I2C_TEST_MAX_WRITES ||
        byte_index >= s_write_history_len[call_index]) {
        return 0;
    }
    return s_write_history[call_index][byte_index];
}

esp_err_t i2c_param_config(i2c_port_t i2c_num, const i2c_config_t *i2c_conf)
{
    (void)i2c_num;
    (void)i2c_conf;
    s_param_config_calls++;
    return ESP_OK;
}

esp_err_t i2c_driver_install(i2c_port_t i2c_num, int mode, int slv_rx_buf_len, int slv_tx_buf_len, int intr_alloc_flags)
{
    (void)i2c_num;
    (void)mode;
    (void)slv_rx_buf_len;
    (void)slv_tx_buf_len;
    (void)intr_alloc_flags;
    s_driver_install_calls++;
    return ESP_OK;
}

esp_err_t i2c_driver_delete(i2c_port_t i2c_num)
{
    (void)i2c_num;
    s_driver_delete_calls++;
    return ESP_OK;
}

i2c_cmd_handle_t i2c_cmd_link_create(void)
{
    return (i2c_cmd_handle_t)1;
}

void i2c_cmd_link_delete(i2c_cmd_handle_t cmd_handle)
{
    (void)cmd_handle;
}

esp_err_t i2c_master_start(i2c_cmd_handle_t cmd_handle)
{
    (void)cmd_handle;
    return ESP_OK;
}

esp_err_t i2c_master_write_byte(i2c_cmd_handle_t cmd_handle, uint8_t data, bool ack_en)
{
    (void)cmd_handle;
    (void)data;
    (void)ack_en;
    return ESP_OK;
}

esp_err_t i2c_master_stop(i2c_cmd_handle_t cmd_handle)
{
    (void)cmd_handle;
    return ESP_OK;
}

esp_err_t i2c_master_cmd_begin(i2c_port_t i2c_num, i2c_cmd_handle_t cmd_handle, uint32_t ticks_to_wait)
{
    (void)i2c_num;
    (void)cmd_handle;
    (void)ticks_to_wait;
    return s_cmd_begin_result;
}

esp_err_t i2c_master_write_to_device(i2c_port_t i2c_num,
                                     uint8_t device_address,
                                     const uint8_t *write_buffer,
                                     size_t write_size,
                                     uint32_t ticks_to_wait)
{
    size_t copy_len = write_size;
    esp_err_t result = s_write_to_device_result;

    (void)i2c_num;
    (void)ticks_to_wait;
    s_last_address = device_address;
    s_last_write_len = write_size;
    s_last_read_len = 0;
    if (copy_len > sizeof(s_last_write)) {
        copy_len = sizeof(s_last_write);
    }
    memset(s_last_write, 0, sizeof(s_last_write));
    if (write_buffer && copy_len > 0) {
        memcpy(s_last_write, write_buffer, copy_len);
    }
    if (s_write_to_device_calls < I2C_TEST_MAX_WRITES) {
        s_write_addresses[s_write_to_device_calls] = device_address;
        s_write_history_len[s_write_to_device_calls] = copy_len;
        if (write_buffer && copy_len > 0) {
            memcpy(s_write_history[s_write_to_device_calls], write_buffer, copy_len);
        }
    }
    s_write_to_device_calls++;
    if (s_write_to_device_result_index < s_write_to_device_result_count) {
        result = s_write_to_device_results[s_write_to_device_result_index++];
    }
    return result;
}

esp_err_t i2c_master_read_from_device(i2c_port_t i2c_num,
                                      uint8_t device_address,
                                      uint8_t *read_buffer,
                                      size_t read_size,
                                      uint32_t ticks_to_wait)
{
    size_t copy_len = read_size;

    (void)i2c_num;
    (void)ticks_to_wait;
    s_last_address = device_address;
    s_last_write_len = 0;
    s_last_read_len = read_size;
    if (copy_len > s_read_data_len) {
        copy_len = s_read_data_len;
    }
    if (read_buffer && copy_len > 0) {
        memcpy(read_buffer, s_read_data, copy_len);
    }
    if (read_buffer && read_size > copy_len) {
        memset(read_buffer + copy_len, 0, read_size - copy_len);
    }
    return s_read_from_device_result;
}

esp_err_t i2c_master_write_read_device(i2c_port_t i2c_num,
                                       uint8_t device_address,
                                       const uint8_t *write_buffer,
                                       size_t write_size,
                                       uint8_t *read_buffer,
                                       size_t read_size,
                                       uint32_t ticks_to_wait)
{
    size_t copy_len = write_size;
    size_t read_copy_len = read_size;

    (void)i2c_num;
    (void)ticks_to_wait;
    s_last_address = device_address;
    s_last_write_len = write_size;
    s_last_read_len = read_size;
    if (copy_len > sizeof(s_last_write)) {
        copy_len = sizeof(s_last_write);
    }
    memset(s_last_write, 0, sizeof(s_last_write));
    if (write_buffer && copy_len > 0) {
        memcpy(s_last_write, write_buffer, copy_len);
    }
    if (read_copy_len > s_read_data_len) {
        read_copy_len = s_read_data_len;
    }
    if (read_buffer && read_copy_len > 0) {
        memcpy(read_buffer, s_read_data, read_copy_len);
    }
    if (read_buffer && read_size > read_copy_len) {
        memset(read_buffer + read_copy_len, 0, read_size - read_copy_len);
    }
    return s_write_read_device_result;
}
