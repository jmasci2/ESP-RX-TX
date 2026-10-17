#include "display.hpp"

#include <cassert>

#include "app_state.hpp"
#include "buttons.hpp"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "touchscreen.hpp"

#include <esp_display_panel.hpp>
#include <lvgl.h>

#if LVGL_VERSION_MAJOR != 9 || LVGL_VERSION_MINOR != 6
#error "This transmitter sketch requires LVGL 9.6.x"
#endif

using esp_panel::board::Board;

static Board *displayBoard = nullptr;
static lv_display_t *lvDisplay = nullptr;
static uint8_t *lvBuffer = nullptr;
static lv_obj_t *linkLabel = nullptr;
static lv_obj_t *keyLabel = nullptr;
static lv_obj_t *analogLabel[4]{};
static lv_obj_t *analogBar[4]{};
static lv_obj_t *motorLabel = nullptr;
static lv_obj_t *batteryLabel = nullptr;

static uint32_t getLvglTick() {
  return millis();
}

static void displayFlush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
  auto lcd = displayBoard->getLCD();
  const int32_t width = area->x2 - area->x1 + 1;
  const int32_t height = area->y2 - area->y1 + 1;
  lcd->drawBitmap(area->x1, area->y1, width, height, pixels);
  lv_display_flush_ready(display);
}

static void createDashboard() {
  lv_obj_t *root = lv_screen_active();
  lv_obj_set_style_bg_color(root, lv_color_hex(0x101820), 0);

  lv_obj_t *title = lv_label_create(root);
  lv_label_set_text(title, "ESP-NOW VEHICLE REMOTE");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 16);

  linkLabel = lv_label_create(root);
  lv_obj_set_style_text_font(linkLabel, &lv_font_montserrat_20, 0);
  lv_obj_align(linkLabel, LV_ALIGN_TOP_RIGHT, -24, 22);

  keyLabel = lv_label_create(root);
  lv_obj_set_style_text_font(keyLabel, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(keyLabel, lv_color_hex(0x50D890), 0);
  lv_label_set_text(keyLabel, "Last track: --");
  lv_obj_align(keyLabel, LV_ALIGN_TOP_LEFT, 24, 62);

  for (uint8_t i = 0; i < 4; ++i) {
    int y = 112 + i * 53;
    analogLabel[i] = lv_label_create(root);
    lv_obj_set_style_text_color(analogLabel[i], lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(analogLabel[i], &lv_font_montserrat_20, 0);
    lv_obj_set_pos(analogLabel[i], 24, y);
    lv_bar_set_range((analogBar[i] = lv_bar_create(root)), 0, 4095);
    lv_obj_set_size(analogBar[i], 230, 24);
    lv_obj_set_pos(analogBar[i], 120, y + 3);
  }

  motorLabel = lv_label_create(root);
  lv_obj_set_style_text_color(motorLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(motorLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_pos(motorLabel, 24, 340);

  batteryLabel = lv_label_create(root);
  lv_obj_set_style_text_color(batteryLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(batteryLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_pos(batteryLabel, 350, 340);

  createTrackButtons(root);
}

void initializeDisplayBoard() {
  const bool hasPsram = esp_psram_is_initialized();
  Serial.printf("PSRAM found: %s, total: %u bytes, free: %u bytes\n",
                hasPsram ? "yes" : "no",
                static_cast<unsigned>(hasPsram ? esp_psram_get_size() : 0),
                static_cast<unsigned>(hasPsram ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0));
  if (!hasPsram) {
    Serial.println("FATAL: CrowPanel RGB display needs OPI PSRAM. Enable it in sdkconfig.defaults, then reboot.");
    while (true) delay(1000);
  }

  displayBoard = new Board();
  Serial.println("STARTUP: board init");
  Serial.flush();
  displayBoard->init();
  Serial.println("STARTUP: board begin");
  Serial.flush();
  if (!displayBoard->begin()) {
    Serial.printf("Display initialization failed; internal free heap: %u bytes, largest PSRAM block: %u bytes\n",
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    while (true) delay(1000);
  }
  Serial.println("STARTUP: board begin complete");
  Serial.flush();
}

void initializeDisplayUi() {
  lv_init();
  lv_tick_set_cb(getLvglTick);
  Serial.println("STARTUP: LVGL initialized");

  constexpr uint16_t bufferLines = 40;
  size_t lvBufferBytes = 800 * bufferLines * sizeof(uint16_t);
  lvBuffer = static_cast<uint8_t *>(
      heap_caps_malloc(lvBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!lvBuffer) {
    lvBufferBytes = 800 * 20 * sizeof(uint16_t);
    lvBuffer = static_cast<uint8_t *>(
        heap_caps_malloc(lvBufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  Serial.printf("STARTUP: LVGL draw buffer %u bytes at %p\n",
                static_cast<unsigned>(lvBufferBytes), lvBuffer);
  Serial.flush();
  assert(lvBuffer != nullptr);

  lvDisplay = lv_display_create(800, 480);
  lv_display_set_color_format(lvDisplay, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(lvDisplay, displayFlush);
  lv_display_set_buffers(lvDisplay, lvBuffer, nullptr, lvBufferBytes,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  initializeTouchscreen(displayBoard);

  Serial.println("STARTUP: creating dashboard");
  Serial.flush();
  createDashboard();
  Serial.println("STARTUP: dashboard created");
  Serial.flush();
}

void updateDisplay() {
  uint32_t now = millis();
  if (now - appState.lastUiUpdate < 200) return;
  appState.lastUiUpdate = now;
  bool connected = appState.haveFeedback && (now - appState.lastFeedbackAt < 1500);
  lv_label_set_text_fmt(linkLabel, connected ? "LINK OK  %ums" : "LINK LOST",
                        static_cast<unsigned int>(connected ? now - appState.lastFeedbackAt : 0));
  lv_obj_set_style_text_color(linkLabel,
                              connected ? lv_color_hex(0x50D890) : lv_color_hex(0xFF5050), 0);
  if (appState.lastTrackSelected) {
    lv_label_set_text_fmt(keyLabel, "Last track sent: %u", appState.lastTrackSelected);
  } else {
    lv_label_set_text(keyLabel, "Last track sent: --");
  }

  for (uint8_t i = 0; i < 4; ++i) {
    uint16_t value = appState.lastAnalog[i] == 0xFFFF ? 0 : appState.lastAnalog[i];
    lv_label_set_text_fmt(analogLabel[i], "A%u  %4u", i + 1, value);
    lv_bar_set_value(analogBar[i], value, LV_ANIM_OFF);
  }

  if (appState.haveFeedback) {
    lv_label_set_text_fmt(
        motorLabel,
        "RECEIVER OUTPUTS\n\nSabertooth M1: %d\nSabertooth M2: %d\nSyRen: %d\n\nReceiver controls\nA1 %u   A2 %u\nA3 %u   A4 %u",
        appState.lastFeedback.sabertoothMotor1, appState.lastFeedback.sabertoothMotor2,
        appState.lastFeedback.syrenMotor, appState.lastFeedback.analogPosition[0],
        appState.lastFeedback.analogPosition[1], appState.lastFeedback.analogPosition[2],
        appState.lastFeedback.analogPosition[3]);
    lv_label_set_text_fmt(batteryLabel, "BATTERY\n\nTransmitter: %.2f V\nReceiver: %.2f V",
                          appState.transmitterBatteryMv / 1000.0f,
                          appState.lastFeedback.receiverBatteryMv / 1000.0f);
  } else {
    lv_label_set_text(motorLabel, "Waiting for receiver feedback...");
    lv_label_set_text_fmt(batteryLabel, "BATTERY\n\nTransmitter: %.2f V\nReceiver: --",
                          appState.transmitterBatteryMv / 1000.0f);
  }
}
