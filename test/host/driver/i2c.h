#ifndef DRIVER_I2C_H
#define DRIVER_I2C_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef int i2c_port_t;
typedef void *i2c_cmd_handle_t;

typedef struct {
    int mode;
    int sda_io_num;
    int scl_io_num;
    int sda_pullup_en;
    int scl_pullup_en;
    struct {
        int clk_speed;
    } master;
} i2c_config_t;

#define I2C_NUM_0 0
#define I2C_MODE_MASTER 1
#define I2C_MASTER_WRITE 0
#define GPIO_PULLUP_ENABLE 1

esp_err_t i2c_param_config(i2c_port_t i2c_num, const i2c_config_t *i2c_conf);
esp_err_t i2c_driver_install(i2c_port_t i2c_num, int mode, int slv_rx_buf_len, int slv_tx_buf_len, int intr_alloc_flags);
esp_err_t i2c_driver_delete(i2c_port_t i2c_num);
i2c_cmd_handle_t i2c_cmd_link_create(void);
void i2c_cmd_link_delete(i2c_cmd_handle_t cmd_handle);
esp_err_t i2c_master_start(i2c_cmd_handle_t cmd_handle);
esp_err_t i2c_master_write_byte(i2c_cmd_handle_t cmd_handle, uint8_t data, bool ack_en);
esp_err_t i2c_master_stop(i2c_cmd_handle_t cmd_handle);
esp_err_t i2c_master_cmd_begin(i2c_port_t i2c_num, i2c_cmd_handle_t cmd_handle, uint32_t ticks_to_wait);
esp_err_t i2c_master_write_to_device(i2c_port_t i2c_num,
                                     uint8_t device_address,
                                     const uint8_t *write_buffer,
                                     size_t write_size,
                                     uint32_t ticks_to_wait);
esp_err_t i2c_master_read_from_device(i2c_port_t i2c_num,
                                      uint8_t device_address,
                                      uint8_t *read_buffer,
                                      size_t read_size,
                                      uint32_t ticks_to_wait);
esp_err_t i2c_master_write_read_device(i2c_port_t i2c_num,
                                       uint8_t device_address,
                                       const uint8_t *write_buffer,
                                       size_t write_size,
                                       uint8_t *read_buffer,
                                       size_t read_size,
                                       uint32_t ticks_to_wait);

void i2c_test_reset(void);
void i2c_test_set_cmd_begin_result(esp_err_t result);
void i2c_test_set_write_to_device_result(esp_err_t result);
void i2c_test_set_read_from_device_result(esp_err_t result);
void i2c_test_set_write_read_device_result(esp_err_t result);
void i2c_test_set_read_data(const uint8_t *data, size_t len);
int i2c_test_get_param_config_calls(void);
int i2c_test_get_driver_install_calls(void);
int i2c_test_get_driver_delete_calls(void);
uint8_t i2c_test_get_last_address(void);
size_t i2c_test_get_last_write_length(void);
size_t i2c_test_get_last_read_length(void);
uint8_t i2c_test_get_last_write_byte(size_t index);

#endif  // DRIVER_I2C_H
