#include "app_state.hpp"
#include "buttons.hpp"
#include "display.hpp"
#include "esp_now_link.hpp"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_now.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

static constexpr uint32_t BATTERY_REPORT_MS = 1000;
static constexpr adc_channel_t BATTERY_ADC_CHANNEL = ADC_CHANNEL_9;
static constexpr float BATTERY_DIVIDER_RATIO = 2.0f;

static adc_oneshot_unit_handle_t batteryAdc = nullptr;
static adc_cali_handle_t batteryAdcCalibration = nullptr;

static void initializeBatteryAdc() {
  adc_oneshot_unit_init_cfg_t unitConfig{};
  unitConfig.unit_id = ADC_UNIT_1;
  unitConfig.ulp_mode = ADC_ULP_MODE_DISABLE;
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&unitConfig, &batteryAdc));

  adc_oneshot_chan_cfg_t channelConfig{};
  channelConfig.atten = ADC_ATTEN_DB_12;
  channelConfig.bitwidth = ADC_BITWIDTH_DEFAULT;
  ESP_ERROR_CHECK(adc_oneshot_config_channel(batteryAdc, BATTERY_ADC_CHANNEL, &channelConfig));

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_curve_fitting_config_t calibrationConfig{};
  calibrationConfig.unit_id = ADC_UNIT_1;
  calibrationConfig.chan = BATTERY_ADC_CHANNEL;
  calibrationConfig.atten = ADC_ATTEN_DB_12;
  calibrationConfig.bitwidth = ADC_BITWIDTH_DEFAULT;
  ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&calibrationConfig, &batteryAdcCalibration));
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
  adc_cali_line_fitting_config_t calibrationConfig{};
  calibrationConfig.unit_id = ADC_UNIT_1;
  calibrationConfig.atten = ADC_ATTEN_DB_12;
  calibrationConfig.bitwidth = ADC_BITWIDTH_DEFAULT;
  ESP_ERROR_CHECK(adc_cali_create_scheme_line_fitting(&calibrationConfig, &batteryAdcCalibration));
#else
#error "The ESP-IDF target must provide an ADC calibration scheme"
#endif
}

static uint32_t readBatteryPinMillivolts() {
  int raw = 0;
  int millivolts = 0;
  ESP_ERROR_CHECK(adc_oneshot_read(batteryAdc, BATTERY_ADC_CHANNEL, &raw));
  ESP_ERROR_CHECK(adc_cali_raw_to_voltage(batteryAdcCalibration, raw, &millivolts));
  return static_cast<uint32_t>(millivolts);
}

static void reportBattery() {
  uint32_t now = millis();
  if (now - appState.lastBatteryReport < BATTERY_REPORT_MS) return;
  appState.lastBatteryReport = now;
  uint32_t pinMillivolts = readBatteryPinMillivolts();
  appState.transmitterBatteryMv =
      static_cast<uint16_t>(pinMillivolts * BATTERY_DIVIDER_RATIO);
  if (!sendInput(EVENT_BATTERY, 0, appState.transmitterBatteryMv)) {
    Serial.println("Queueing transmitter battery packet failed");
  }
}

static void setup() {
  initializeBatteryAdc();
  delay(300);
  initializeDisplayBoard();
  initializeButtonHardware();
  initializeDisplayUi();

  if (!probeAds1115()) {
    Serial.println("ADS1115 not detected at 0x48; connect analog controls to ADS1115 A0-A3");
  }
  if (!probeButtonExpander()) {
    Serial.println("Button PCF8574 not detected at 0x21");
  }

  initializeEspNow();
  Serial.println("Ready. Sending touch pad, ADS1115 analog, and PCF8574 button events.");
}

static void loop() {
  pollInputButtonsAndAnalog();
  reportBattery();
  sendKeepalive();
  updateDisplay();
  lv_timer_handler();
  delay(2);
}

extern "C" void app_main(void) {
  setup();
  while (true) {
    loop();
  }
}
