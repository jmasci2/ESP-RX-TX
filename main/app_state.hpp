#pragma once

#include <cstdint>
#include <cstdio>

static constexpr uint8_t WIFI_CHANNEL = 6;
static constexpr uint8_t EVENT_ANALOG = 2;
static constexpr uint8_t EVENT_BUTTON = 3;
static constexpr uint8_t EVENT_BATTERY = 4;
static constexpr uint8_t EVENT_TRACK = 5;
static constexpr uint8_t EVENT_KEEPALIVE = 6;
static constexpr uint8_t EVENT_FEEDBACK = 0x80;
static constexpr uint8_t PHYSICAL_BUTTON_CODE_BASE = 65;
static constexpr uint16_t PACKET_MAGIC = 0x4B42;

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
  uint16_t transmitterBatteryMv;
};

struct RuntimeState {
  FeedbackPacket lastFeedback{};
  uint32_t lastFeedbackAt = 0;
  uint16_t transmitterBatteryMv = 0;
  bool haveFeedback = false;
  uint8_t selectedBank = 0;
  uint8_t lastTrackSelected = 0;
  uint16_t sequenceNumber = 0;
  uint16_t lastAnalog[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
  uint32_t lastAnalogSentAt[4] = {};
  bool buttonRaw[6] = {};
  bool buttonStable[6] = {};
  uint32_t buttonChangedAt[6] = {};
  uint32_t lastAnalogSample = 0;
  uint32_t lastBatteryReport = 0;
  uint32_t lastKeepaliveAt = 0;
  uint32_t lastUiUpdate = 0;
};

extern RuntimeState appState;

class SerialPort {
public:
  void begin(uint32_t) {}

  template <typename... Args>
  void printf(const char *format, Args... args) {
    std::printf(format, args...);
  }

  void print(const char *text) { std::printf("%s", text); }
  void println(const char *text) { std::printf("%s\n", text); }
  void flush() { std::fflush(stdout); }
};

extern SerialPort Serial;

uint32_t millis();
void delay(uint32_t milliseconds);
