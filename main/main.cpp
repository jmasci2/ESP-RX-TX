#include "app_state.hpp"
#include "buttons.hpp"
#include "display.hpp"
#include "esp_now_link.hpp"

#include "lvgl.h"

static void setup() {
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
