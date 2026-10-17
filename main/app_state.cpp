#include "app_state.hpp"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

RuntimeState appState{};
SerialPort Serial;

uint32_t millis() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

void delay(uint32_t milliseconds) {
  vTaskDelay(pdMS_TO_TICKS(milliseconds));
}
