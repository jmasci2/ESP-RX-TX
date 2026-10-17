/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "utils/esp_panel_utils_log.h"
#include "driver/i2c_master.h"
#include "esp_panel_host_i2c.hpp"

namespace esp_panel::drivers {

HostI2C::~HostI2C()
{
    ESP_UTILS_LOG_TRACE_ENTER_WITH_THIS();

    if (isOverState(State::BEGIN)) {
        int id = getID();
        ESP_UTILS_CHECK_ERROR_EXIT(
            i2c_del_master_bus(static_cast<i2c_master_bus_handle_t>(host_handle)),
            "Delete I2C host(%d) failed", id
        );
        host_handle = nullptr;
        ESP_UTILS_LOGD("Delete I2C host(%d)", id);

        setState(State::DEINIT);
    }

    ESP_UTILS_LOG_TRACE_EXIT_WITH_THIS();
}

bool HostI2C::begin()
{
    ESP_UTILS_LOG_TRACE_ENTER_WITH_THIS();

    if (isOverState(State::BEGIN)) {
        goto end;
    }

    {
        int id = getID();
        i2c_master_bus_handle_t handle = nullptr;
        ESP_UTILS_CHECK_ERROR_RETURN(
            i2c_new_master_bus(&config, &handle), false, "I2C master bus creation failed"
        );
        host_handle = handle;
        ESP_UTILS_LOGD("Initialize I2C host(%d)", id);
    }

    setState(State::BEGIN);

end:
    ESP_UTILS_LOG_TRACE_EXIT_WITH_THIS();

    return true;
}

bool HostI2C::calibrateConfig(const i2c_master_bus_config_t &config)
{
    if (config.i2c_port != this->config.i2c_port ||
        config.sda_io_num != this->config.sda_io_num ||
        config.scl_io_num != this->config.scl_io_num ||
        config.clk_source != this->config.clk_source ||
        config.glitch_ignore_cnt != this->config.glitch_ignore_cnt ||
        config.intr_priority != this->config.intr_priority ||
        config.trans_queue_depth != this->config.trans_queue_depth ||
        config.flags.enable_internal_pullup != this->config.flags.enable_internal_pullup ||
        config.flags.allow_pd != this->config.flags.allow_pd) {
        ESP_UTILS_LOGI(
            "I2C host config mismatch: original pins(%d,%d), clock source(%d), pullup(%d);"
            " new pins(%d,%d), clock source(%d), pullup(%d)",
            static_cast<int>(this->config.sda_io_num), static_cast<int>(this->config.scl_io_num),
            static_cast<int>(this->config.clk_source), static_cast<int>(this->config.flags.enable_internal_pullup),
            static_cast<int>(config.sda_io_num), static_cast<int>(config.scl_io_num),
            static_cast<int>(config.clk_source), static_cast<int>(config.flags.enable_internal_pullup)
        );
        ESP_UTILS_CHECK_FALSE_RETURN(false, false, "Config mismatch");
    }

    return true;
}

} // namespace esp_panel::drivers
