/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t esp_io_expander_i2c_write(i2c_port_num_t port, uint16_t address, const uint8_t *data,
                                    size_t size, int timeout_ms);
esp_err_t esp_io_expander_i2c_read(i2c_port_num_t port, uint16_t address, uint8_t *data,
                                   size_t size, int timeout_ms);
esp_err_t esp_io_expander_i2c_write_read(i2c_port_num_t port, uint16_t address,
                                         const uint8_t *write_data, size_t write_size,
                                         uint8_t *read_data, size_t read_size, int timeout_ms);
