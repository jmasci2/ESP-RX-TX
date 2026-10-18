// Elecrow CrowPanel 7.0 HMI DIS08070H (ESP32-S3, 800x480 RGB LCD, EK9716BD3) remote.
// LVGL 9.6 renders the dashboard; ESP-NOW sends controls and receives telemetry.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <driver/i2c.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include <vector>

// Arduino runs setup() and loop() on loopTask; allocate a larger stack for LVGL/UI work.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#if LVGL_VERSION_MAJOR != 9 || LVGL_VERSION_MINOR != 6
#error "This transmitter sketch requires LVGL 9.6.x"
#endif

static constexpr uint8_t WIFI_CHANNEL = 6;
static constexpr uint8_t BUTTON_PCF8574_ADDRESS = 0x21; // six buttons on separate expander
static constexpr uint8_t ADS1115_ADDRESS = 0x48;
static constexpr uint8_t EVENT_ANALOG = 2;
static constexpr uint8_t EVENT_BUTTON = 3;
static constexpr uint8_t EVENT_TRACK = 5;
static constexpr uint8_t EVENT_KEEPALIVE = 6;
static constexpr uint8_t PHYSICAL_BUTTON_CODE_BASE = 65;
static constexpr uint8_t EVENT_FEEDBACK = 0x80;
static constexpr uint16_t ANALOG_DELTA = 32; // send after ~32 ADC counts of movement
static constexpr uint32_t ANALOG_SAMPLE_MS = 40;
static constexpr uint32_t ANALOG_KEEPALIVE_MS = 200;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
static constexpr uint32_t KEEPALIVE_INTERVAL_MS = 250;

// Replace with the RECEIVER's Wi-Fi station MAC, printed in its Serial Monitor.
uint8_t receiverMac[] = { 0x24, 0x6F, 0x28, 0x00, 0x00, 0x00 };

using namespace esp_panel::board;
using namespace esp_panel::drivers;
Board *displayBoard = nullptr;
lv_display_t *lvDisplay = nullptr;
lv_indev_t *touchInput = nullptr;
bool externalI2cReady = false;
uint8_t *lvBuffer = nullptr;
uint16_t lvBufferLines = 0;
lv_obj_t *linkLabel, *keyLabel, *analogLabel[4], *analogBar[4];
lv_obj_t *motorLabel, *batteryLabel, *bankLabel;
lv_obj_t *trackButtonLabels[16];
uint32_t lastFeedbackAt = 0;
bool haveFeedback = false;
uint8_t selectedBank = 0;
uint8_t lastTrackSelected = 0;
uint32_t lastUiUpdate = 0;

struct __attribute__((packed)) InputPacket {
  uint16_t magic;
  uint8_t type;
  uint8_t code;
  uint16_t value;
  uint16_t sequence;
};
struct __attribute__((packed)) FeedbackPacket {
  uint16_t magic;
  uint8_t type;
  uint16_t sequence;
  uint16_t analogPosition[4];
  int8_t sabertoothMotor1;
  int8_t sabertoothMotor2;
  int8_t syrenMotor;
  uint16_t receiverBatteryMv;
};
FeedbackPacket lastFeedback{};
static constexpr uint16_t PACKET_MAGIC = 0x4B42;
uint16_t sequenceNumber = 0;
uint16_t lastAnalog[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
uint32_t lastAnalogSentAt[4] = {0, 0, 0, 0};
bool buttonRaw[6];
bool buttonStable[6];
uint32_t buttonChangedAt[6] = {0, 0, 0, 0, 0, 0};
uint32_t lastAnalogSample = 0;
uint32_t lastKeepaliveAt = 0;

uint32_t getLvglTick() {
  return millis();
}

void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;
  if (len != sizeof(FeedbackPacket)) return;
  FeedbackPacket feedback;
  memcpy(&feedback, data, sizeof(feedback));
  if (feedback.magic != PACKET_MAGIC || feedback.type != EVENT_FEEDBACK) return;
  lastFeedback = feedback;
  haveFeedback = true;
  lastFeedbackAt = millis();

  Serial.printf("RX feedback #%u | controls A1=%u A2=%u A3=%u A4=%u | Sabertooth M1=%d M2=%d | SyRen=%d | receiver battery=%.2fV\n",
                feedback.sequence, feedback.analogPosition[0], feedback.analogPosition[1],
                feedback.analogPosition[2], feedback.analogPosition[3],
                feedback.sabertoothMotor1, feedback.sabertoothMotor2, feedback.syrenMotor,
                feedback.receiverBatteryMv / 1000.0f);
}

void onSent(const wifi_tx_info_t *txInfo, esp_now_send_status_t status) {
  (void)txInfo;
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("ESP-NOW send failed");
  }
}

bool sendInput(uint8_t type, uint8_t code, uint16_t value) {
  InputPacket packet{PACKET_MAGIC, type, code, value, sequenceNumber++};
  esp_err_t error = esp_now_send(receiverMac, reinterpret_cast<const uint8_t *>(&packet),
                                 sizeof(packet));
  if (error != ESP_OK) return false;
  Serial.printf("TX type=%u code=%u value=%u seq=%u\n",
                packet.type, packet.code, packet.value, packet.sequence);
  return true;
}

void sendKeepalive() {
  uint32_t now = millis();
  if (now - lastKeepaliveAt < KEEPALIVE_INTERVAL_MS) return;
  lastKeepaliveAt = now;
  if (!sendInput(EVENT_KEEPALIVE, 0, 0)) {
    Serial.println("Queueing ESP-NOW keepalive failed");
  }
}

bool probeAds1115() {
  uint8_t configRegister = 0x01;
  uint8_t configBytes[2]{};
  return i2c_master_write_read_device(I2C_NUM_0, ADS1115_ADDRESS, &configRegister, 1,
                                      configBytes, sizeof(configBytes), pdMS_TO_TICKS(100)) == ESP_OK;
}

bool readAds1115(uint8_t channel, int16_t &sample) {
  if (!externalI2cReady || channel > 3) return false;

  // ADS1115 one-shot, AINx-to-GND, +/-4.096 V, 128 samples/s, comparator off.
  uint16_t config = 0x8000 | (static_cast<uint16_t>(4 + channel) << 12) |
                    0x0200 | 0x0100 | 0x0080 | 0x0003;
  uint8_t configWrite[] = {0x01, static_cast<uint8_t>(config >> 8), static_cast<uint8_t>(config)};
  if (i2c_master_write_to_device(I2C_NUM_0, ADS1115_ADDRESS, configWrite,
                                 sizeof(configWrite), pdMS_TO_TICKS(100)) != ESP_OK) return false;
  delay(9); // 128 SPS conversion time is 7.8 ms

  uint8_t conversionRegister = 0x00;
  uint8_t conversion[2]{};
  if (i2c_master_write_read_device(I2C_NUM_0, ADS1115_ADDRESS, &conversionRegister, 1,
                                  conversion, sizeof(conversion), pdMS_TO_TICKS(100)) != ESP_OK) return false;
  sample = static_cast<int16_t>((static_cast<uint16_t>(conversion[0]) << 8) | conversion[1]);
  return true;
}

bool writeExpander(uint8_t value) {
  return externalI2cReady &&
         i2c_master_write_to_device(I2C_NUM_0, BUTTON_PCF8574_ADDRESS, &value, 1,
                                    pdMS_TO_TICKS(100)) == ESP_OK;
}

uint8_t readExpander() {
  uint8_t value = 0xFF;
  if (!externalI2cReady ||
      i2c_master_read_from_device(I2C_NUM_0, BUTTON_PCF8574_ADDRESS, &value, 1,
                                  pdMS_TO_TICKS(100)) != ESP_OK) return 0xFF;
  return value;
}

void pollAnalogInputs() {
  uint32_t now = millis();
  if (now - lastAnalogSample < ANALOG_SAMPLE_MS) return;
  lastAnalogSample = now;
  for (uint8_t i = 0; i < 4; ++i) {
    int16_t raw;
    if (!readAds1115(i, raw)) continue;
    uint16_t value = static_cast<uint16_t>(constrain(map(raw, 0, 26500, 0, 4095), 0, 4095));
    if (lastAnalog[i] == 0xFFFF ||
        abs((int)value - (int)lastAnalog[i]) >= ANALOG_DELTA ||
        now - lastAnalogSentAt[i] >= ANALOG_KEEPALIVE_MS) {
      lastAnalog[i] = value;
      lastAnalogSentAt[i] = now;
      if (!sendInput(EVENT_ANALOG, i, value)) {
        Serial.println("Queueing analog ESP-NOW packet failed");
      }
    }
  }
}

void pollButtons() {
  if (!externalI2cReady) return;
  uint32_t now = millis();
  if (!writeExpander(0xFF)) return;
  uint8_t pins = readExpander();
  for (uint8_t i = 0; i < 6; ++i) {
    bool reading = (pins & (1U << i)) == 0; // active-low, PCF8574 pull-up
    if (reading != buttonRaw[i]) {
      buttonRaw[i] = reading;
      buttonChangedAt[i] = now;
    }
    if (buttonStable[i] != buttonRaw[i] && now - buttonChangedAt[i] >= BUTTON_DEBOUNCE_MS) {
      buttonStable[i] = buttonRaw[i];
      if (!sendInput(EVENT_BUTTON, PHYSICAL_BUTTON_CODE_BASE + i,
                     buttonStable[i] ? 1 : 0)) {
        Serial.println("Queueing button ESP-NOW packet failed");
      }
    }
  }
}

void displayFlush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
  auto lcd = displayBoard->getLCD();
  // drawBitmap takes width/height, while LVGL areas use inclusive x2/y2 bounds.
  const int32_t width = area->x2 - area->x1 + 1;
  const int32_t height = area->y2 - area->y1 + 1;
  lcd->drawBitmap(area->x1, area->y1, width, height, pixels);
  lv_display_flush_ready(display);
}

void readTouch(lv_indev_t *input, lv_indev_data_t *data) {
  (void)input;
  auto touch = displayBoard->getTouch();
  if (!touch) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  touch->readRawData(-1, -1, 0);
  std::vector<TouchPoint> points;
  touch->getPoints(points);
  if (points.empty()) {
    data->state = LV_INDEV_STATE_RELEASED;
  } else {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = points[0].x;
    data->point.y = points[0].y;
  }
}

void updateTrackBankLabels() {
  uint8_t firstTrack = selectedBank * 16 + 1;
  lv_label_set_text_fmt(bankLabel, "BANK %u   TRACKS %u-%u", selectedBank + 1,
                        firstTrack, firstTrack + 15);
  for (uint8_t slot = 0; slot < 16; ++slot) {
    lv_label_set_text_fmt(trackButtonLabels[slot], "%u", firstTrack + slot);
  }
}

void trackButtonEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  uint8_t slot = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  uint16_t trackNumber = selectedBank * 16 + slot + 1;
  lastTrackSelected = static_cast<uint8_t>(trackNumber);
  if (!sendInput(EVENT_TRACK, 0, trackNumber)) {
    Serial.println("Could not queue track number for ESP-NOW");
  }
}

void bankButtonEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  bool forward = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)) != 0;
  selectedBank = forward ? (selectedBank + 1) % 4 : (selectedBank + 3) % 4;
  updateTrackBankLabels();
}

void createDashboard() {
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
  bankLabel = lv_label_create(root);
  lv_obj_set_style_text_color(bankLabel, lv_color_hex(0x50D890), 0);
  lv_obj_set_style_text_font(bankLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_pos(bankLabel, 465, 64);

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

  for (uint8_t row = 0; row < 4; ++row) {
    for (uint8_t col = 0; col < 4; ++col) {
      uint8_t slot = row * 4 + col;
      lv_obj_t *button = lv_button_create(root);
      lv_obj_set_size(button, 72, 49);
      lv_obj_set_pos(button, 465 + col * 79, 108 + row * 56);
      lv_obj_set_style_bg_color(button, lv_color_hex(0x24445A), LV_PART_MAIN);
      lv_obj_set_style_bg_color(button, lv_color_hex(0x35749A), LV_PART_MAIN | LV_STATE_PRESSED);
      lv_obj_add_event_cb(button, trackButtonEvent, LV_EVENT_CLICKED,
                          reinterpret_cast<void *>(static_cast<uintptr_t>(slot)));
      trackButtonLabels[slot] = lv_label_create(button);
      lv_obj_set_style_text_font(trackButtonLabels[slot], &lv_font_montserrat_22, 0);
      lv_obj_center(trackButtonLabels[slot]);
    }
  }
  lv_obj_t *backButton = lv_button_create(root);
  lv_obj_set_size(backButton, 76, 54);
  lv_obj_set_pos(backButton, 375, 168);
  lv_obj_add_event_cb(backButton, bankButtonEvent, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *backLabel = lv_label_create(backButton);
  lv_label_set_text(backLabel, "BACK");
  lv_obj_center(backLabel);
  lv_obj_t *forwardButton = lv_button_create(root);
  lv_obj_set_size(forwardButton, 76, 54);
  lv_obj_set_pos(forwardButton, 375, 235);
  lv_obj_add_event_cb(forwardButton, bankButtonEvent, LV_EVENT_CLICKED, reinterpret_cast<void *>(1));
  lv_obj_t *forwardLabel = lv_label_create(forwardButton);
  lv_label_set_text(forwardLabel, "NEXT");
  lv_obj_center(forwardLabel);
  updateTrackBankLabels();
}

void updateDashboard() {
  uint32_t now = millis();
  if (now - lastUiUpdate < 200) return;
  lastUiUpdate = now;
  bool connected = haveFeedback && (now - lastFeedbackAt < 1500);
  lv_label_set_text_fmt(linkLabel, connected ? "LINK OK  %ums" : "LINK LOST",
                        connected ? now - lastFeedbackAt : 0);
  lv_obj_set_style_text_color(linkLabel, connected ? lv_color_hex(0x50D890) : lv_color_hex(0xFF5050), 0);
  if (lastTrackSelected) lv_label_set_text_fmt(keyLabel, "Last track sent: %u", lastTrackSelected);
  else lv_label_set_text(keyLabel, "Last track sent: --");
  for (uint8_t i = 0; i < 4; ++i) {
    uint16_t value = lastAnalog[i] == 0xFFFF ? 0 : lastAnalog[i];
    lv_label_set_text_fmt(analogLabel[i], "A%u  %4u", i + 1, value);
    lv_bar_set_value(analogBar[i], value, LV_ANIM_OFF);
  }
  if (haveFeedback) {
    lv_label_set_text_fmt(motorLabel,
      "RECEIVER OUTPUTS\n\nSabertooth M1: %d\nSabertooth M2: %d\nSyRen: %d\n\nReceiver controls\nA1 %u   A2 %u\nA3 %u   A4 %u",
      lastFeedback.sabertoothMotor1, lastFeedback.sabertoothMotor2,
      lastFeedback.syrenMotor, lastFeedback.analogPosition[0], lastFeedback.analogPosition[1],
      lastFeedback.analogPosition[2], lastFeedback.analogPosition[3]);
    lv_label_set_text_fmt(batteryLabel, "RECEIVER BATTERY\n\n%.2f V",
      lastFeedback.receiverBatteryMv / 1000.0f);
  } else {
    lv_label_set_text(motorLabel, "Waiting for receiver feedback...");
    lv_label_set_text(batteryLabel, "RECEIVER BATTERY\n\n--");
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  const bool hasPsram = psramFound();
  Serial.printf("PSRAM found: %s, total: %u bytes, free: %u bytes\n",
                hasPsram ? "yes" : "no",
                static_cast<unsigned>(hasPsram ? ESP.getPsramSize() : 0),
                static_cast<unsigned>(hasPsram ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0));
  if (!hasPsram) {
    Serial.println("FATAL: CrowPanel RGB display needs OPI PSRAM. Select Tools > PSRAM > OPI PSRAM, then reboot.");
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
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    while (true) delay(1000);
  }
  Serial.println("STARTUP: board begin complete");
  Serial.flush();
  // Reuse the legacy I2C host already initialized by the CrowPanel profile.
  // Do not initialize Wire or the newer driver_ng I2C host in this sketch.
  externalI2cReady = true;
  lv_init();
  lv_tick_set_cb(getLvglTick);
  Serial.println("STARTUP: LVGL initialized");
  lvBufferLines = 40;
  size_t lvBufferBytes = 800 * lvBufferLines * sizeof(uint16_t);
  lvBuffer = static_cast<uint8_t *>(heap_caps_malloc(lvBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!lvBuffer) {
    lvBufferLines = 20;
    lvBufferBytes = 800 * lvBufferLines * sizeof(uint16_t);
    lvBuffer = static_cast<uint8_t *>(heap_caps_malloc(lvBufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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
  touchInput = lv_indev_create();
  lv_indev_set_type(touchInput, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touchInput, readTouch);
  Serial.println("STARTUP: creating dashboard");
  Serial.flush();
  createDashboard();
  Serial.println("STARTUP: dashboard created");
  Serial.flush();
  analogReadResolution(12);
  if (!probeAds1115()) {
    Serial.println("ADS1115 not detected at 0x48; connect analog controls to ADS1115 A0-A3");
  }
  if (!writeExpander(0xFF)) {
    Serial.println("Button PCF8574 not detected at 0x21");
  }
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.print("Remote STA MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) delay(100);
  }
  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, receiverMac, 6);
  peer.channel = WIFI_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Could not add receiver peer; check receiver MAC");
    while (true) delay(100);
  }
  const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_peer_info_t feedbackPeer{};
  memcpy(feedbackPeer.peer_addr, broadcastMac, 6);
  feedbackPeer.channel = WIFI_CHANNEL;
  feedbackPeer.ifidx = WIFI_IF_STA;
  feedbackPeer.encrypt = false;
  if (esp_now_add_peer(&feedbackPeer) != ESP_OK) {
    Serial.println("Could not add ESP-NOW feedback broadcast peer");
    while (true) delay(100);
  }
  esp_now_register_recv_cb(onReceive);
  Serial.println("Ready. Sending touch pad, ADS1115 analog, and PCF8574 button events.");
}

void loop() {
  pollAnalogInputs();
  pollButtons();
  sendKeepalive();
  updateDashboard();
  lv_timer_handler();
  delay(2);
}
