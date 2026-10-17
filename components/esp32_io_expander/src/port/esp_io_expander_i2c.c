/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_io_expander_i2c.h"

static esp_err_t create_device(i2c_port_num_t port, uint16_t address,
                               i2c_master_dev_handle_t *device)
{
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t error = i2c_master_get_bus_handle(port, &bus);
    if (error != ESP_OK) {
        return error;
    }

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(bus, &config, device);
}

static esp_err_t finish_transfer(i2c_master_dev_handle_t device, esp_err_t transfer_error)
{
    esp_err_t cleanup_error = i2c_master_bus_rm_device(device);
    return transfer_error == ESP_OK ? cleanup_error : transfer_error;
}

esp_err_t esp_io_expander_i2c_write(i2c_port_num_t port, uint16_t address, const uint8_t *data,
                                    size_t size, int timeout_ms)
{
    i2c_master_dev_handle_t device = NULL;
    esp_err_t error = create_device(port, address, &device);
    if (error != ESP_OK) {
        return error;
    }
    return finish_transfer(device, i2c_master_transmit(device, data, size, timeout_ms));
}

esp_err_t esp_io_expander_i2c_read(i2c_port_num_t port, uint16_t address, uint8_t *data,
                                   size_t size, int timeout_ms)
{
    i2c_master_dev_handle_t device = NULL;
    esp_err_t error = create_device(port, address, &device);
    if (error != ESP_OK) {
        return error;
    }
    return finish_transfer(device, i2c_master_receive(device, data, size, timeout_ms));
}

esp_err_t esp_io_expander_i2c_write_read(i2c_port_num_t port, uint16_t address,
                                         const uint8_t *write_data, size_t write_size,
                                         uint8_t *read_data, size_t read_size, int timeout_ms)
{
    i2c_master_dev_handle_t device = NULL;
    esp_err_t error = create_device(port, address, &device);
    if (error != ESP_OK) {
        return error;
    }
    return finish_transfer(device, i2c_master_transmit_receive(
                                       device, write_data, write_size, read_data, read_size, timeout_ms));
}
