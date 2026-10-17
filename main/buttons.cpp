#include "buttons.hpp"

#include <algorithm>
#include <cstdlib>

#include "app_state.hpp"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_now_link.hpp"

static constexpr uint8_t BUTTON_PCF8574_ADDRESS = 0x20;
static constexpr uint8_t ADS1115_ADDRESS = 0x48;
static constexpr uint16_t ANALOG_DELTA = 32;
static constexpr uint32_t ANALOG_SAMPLE_MS = 40;
static constexpr uint32_t ANALOG_KEEPALIVE_MS = 200;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;

static i2c_master_dev_handle_t ads1115I2cDevice = nullptr;
static i2c_master_dev_handle_t buttonExpanderI2cDevice = nullptr;
static bool externalI2cReady = false;
static lv_obj_t *bankLabel = nullptr;
static lv_obj_t *trackButtonLabels[16]{};

static long mapValue(long value, long inputMin, long inputMax, long outputMin, long outputMax) {
  return (value - inputMin) * (outputMax - outputMin) / (inputMax - inputMin) + outputMin;
}

static long clampValue(long value, long low, long high) {
  return std::min(std::max(value, low), high);
}

bool initializeButtonHardware() {
  i2c_master_bus_handle_t bus = nullptr;
  esp_err_t error = i2c_master_get_bus_handle(I2C_NUM_0, &bus);
  if (error != ESP_OK) {
    Serial.printf("Could not get CrowPanel I2C bus: %s\n", esp_err_to_name(error));
    return false;
  }

  const i2c_device_config_t deviceConfig = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = ADS1115_ADDRESS,
      .scl_speed_hz = 100000,
  };
  error = i2c_master_bus_add_device(bus, &deviceConfig, &ads1115I2cDevice);
  if (error != ESP_OK) {
    Serial.printf("Could not add ADS1115 to the I2C bus: %s\n", esp_err_to_name(error));
    return false;
  }

  i2c_device_config_t buttonConfig = deviceConfig;
  buttonConfig.device_address = BUTTON_PCF8574_ADDRESS;
  error = i2c_master_bus_add_device(bus, &buttonConfig, &buttonExpanderI2cDevice);
  if (error != ESP_OK) {
    Serial.printf("Could not add PCF8574 to the I2C bus: %s\n", esp_err_to_name(error));
    ESP_ERROR_CHECK(i2c_master_bus_rm_device(ads1115I2cDevice));
    ads1115I2cDevice = nullptr;
    return false;
  }

  externalI2cReady = true;
  return true;
}

bool probeAds1115() {
  if (!externalI2cReady || ads1115I2cDevice == nullptr) return false;
  uint8_t configRegister = 0x01;
  uint8_t configBytes[2]{};
  return i2c_master_transmit_receive(ads1115I2cDevice, &configRegister, 1,
                                     configBytes, sizeof(configBytes), 100) == ESP_OK;
}

static bool writeExpander(uint8_t value) {
  return externalI2cReady &&
         i2c_master_transmit(buttonExpanderI2cDevice, &value, 1, 100) == ESP_OK;
}

bool probeButtonExpander() {
  return writeExpander(0xFF);
}

static uint8_t readExpander() {
  uint8_t value = 0xFF;
  if (!externalI2cReady ||
      i2c_master_receive(buttonExpanderI2cDevice, &value, 1, 100) != ESP_OK) {
    return 0xFF;
  }
  return value;
}

static bool readAds1115(uint8_t channel, int16_t &sample) {
  if (!externalI2cReady || channel > 3) return false;

  uint16_t config = 0x8000 | (static_cast<uint16_t>(4 + channel) << 12) |
                    0x0200 | 0x0100 | 0x0080 | 0x0003;
  uint8_t configWrite[] = {0x01, static_cast<uint8_t>(config >> 8),
                           static_cast<uint8_t>(config)};
  if (i2c_master_transmit(ads1115I2cDevice, configWrite, sizeof(configWrite), 100) != ESP_OK) {
    return false;
  }
  delay(9);

  uint8_t conversionRegister = 0x00;
  uint8_t conversion[2]{};
  if (i2c_master_transmit_receive(ads1115I2cDevice, &conversionRegister, 1,
                                  conversion, sizeof(conversion), 100) != ESP_OK) {
    return false;
  }
  sample = static_cast<int16_t>((static_cast<uint16_t>(conversion[0]) << 8) | conversion[1]);
  return true;
}

static void pollAnalogInputs() {
  uint32_t now = millis();
  if (now - appState.lastAnalogSample < ANALOG_SAMPLE_MS) return;
  appState.lastAnalogSample = now;

  for (uint8_t i = 0; i < 4; ++i) {
    int16_t raw;
    if (!readAds1115(i, raw)) continue;
    uint16_t value = static_cast<uint16_t>(clampValue(mapValue(raw, 0, 26500, 0, 4095), 0, 4095));
    if (appState.lastAnalog[i] == 0xFFFF ||
        abs(static_cast<int>(value) - static_cast<int>(appState.lastAnalog[i])) >= ANALOG_DELTA ||
        now - appState.lastAnalogSentAt[i] >= ANALOG_KEEPALIVE_MS) {
      appState.lastAnalog[i] = value;
      appState.lastAnalogSentAt[i] = now;
      if (!sendInput(EVENT_ANALOG, i, value)) {
        Serial.println("Queueing analog ESP-NOW packet failed");
      }
    }
  }
}

static void pollPhysicalButtons() {
  if (!externalI2cReady) return;
  uint32_t now = millis();
  if (!writeExpander(0xFF)) return;
  uint8_t pins = readExpander();

  for (uint8_t i = 0; i < 6; ++i) {
    bool reading = (pins & (1U << i)) == 0;
    if (reading != appState.buttonRaw[i]) {
      appState.buttonRaw[i] = reading;
      appState.buttonChangedAt[i] = now;
    }
    if (appState.buttonStable[i] != appState.buttonRaw[i] &&
        now - appState.buttonChangedAt[i] >= BUTTON_DEBOUNCE_MS) {
      appState.buttonStable[i] = appState.buttonRaw[i];
      if (!sendInput(EVENT_BUTTON, PHYSICAL_BUTTON_CODE_BASE + i,
                     appState.buttonStable[i] ? 1 : 0)) {
        Serial.println("Queueing button ESP-NOW packet failed");
      }
    }
  }
}

void pollInputButtonsAndAnalog() {
  pollAnalogInputs();
  pollPhysicalButtons();
}

static void updateTrackBankLabels() {
  uint8_t firstTrack = appState.selectedBank * 16 + 1;
  lv_label_set_text_fmt(bankLabel, "BANK %u   TRACKS %u-%u", appState.selectedBank + 1,
                        firstTrack, firstTrack + 15);
  for (uint8_t slot = 0; slot < 16; ++slot) {
    lv_label_set_text_fmt(trackButtonLabels[slot], "%u", firstTrack + slot);
  }
}

static void trackButtonEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  uint8_t slot = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  uint16_t trackNumber = appState.selectedBank * 16 + slot + 1;
  appState.lastTrackSelected = static_cast<uint8_t>(trackNumber);
  if (!sendInput(EVENT_TRACK, 0, trackNumber)) {
    Serial.println("Could not queue track number for ESP-NOW");
  }
}

static void bankButtonEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  bool forward = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)) != 0;
  appState.selectedBank = forward ? (appState.selectedBank + 1) % 4
                                  : (appState.selectedBank + 3) % 4;
  updateTrackBankLabels();
}

void createTrackButtons(lv_obj_t *parent) {
  bankLabel = lv_label_create(parent);
  lv_obj_set_style_text_color(bankLabel, lv_color_hex(0x50D890), 0);
  lv_obj_set_style_text_font(bankLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_pos(bankLabel, 465, 64);

  for (uint8_t row = 0; row < 4; ++row) {
    for (uint8_t col = 0; col < 4; ++col) {
      uint8_t slot = row * 4 + col;
      lv_obj_t *button = lv_button_create(parent);
      lv_obj_set_size(button, 72, 49);
      lv_obj_set_pos(button, 465 + col * 79, 108 + row * 56);
      lv_obj_set_style_bg_color(button, lv_color_hex(0x24445A), LV_PART_MAIN);
      lv_obj_set_style_bg_color(
          button, lv_color_hex(0x35749A),
          static_cast<lv_style_selector_t>(LV_PART_MAIN) |
              static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
      lv_obj_add_event_cb(button, trackButtonEvent, LV_EVENT_CLICKED,
                          reinterpret_cast<void *>(static_cast<uintptr_t>(slot)));
      trackButtonLabels[slot] = lv_label_create(button);
      lv_obj_set_style_text_font(trackButtonLabels[slot], &lv_font_montserrat_22, 0);
      lv_obj_center(trackButtonLabels[slot]);
    }
  }

  lv_obj_t *backButton = lv_button_create(parent);
  lv_obj_set_size(backButton, 76, 54);
  lv_obj_set_pos(backButton, 375, 168);
  lv_obj_add_event_cb(backButton, bankButtonEvent, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *backLabel = lv_label_create(backButton);
  lv_label_set_text(backLabel, "BACK");
  lv_obj_center(backLabel);

  lv_obj_t *forwardButton = lv_button_create(parent);
  lv_obj_set_size(forwardButton, 76, 54);
  lv_obj_set_pos(forwardButton, 375, 235);
  lv_obj_add_event_cb(forwardButton, bankButtonEvent, LV_EVENT_CLICKED,
                      reinterpret_cast<void *>(1));
  lv_obj_t *forwardLabel = lv_label_create(forwardButton);
  lv_label_set_text(forwardLabel, "NEXT");
  lv_obj_center(forwardLabel);

  updateTrackBankLabels();
}
