// ESP32 receiver: receive keypad/controller events over ESP-NOW and print to Serial.
// Tested API target: Arduino-ESP32 3.x (classic esp_now.h API).
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <SoftwareSerial.h>

static constexpr uint8_t WIFI_CHANNEL = 6;
static constexpr uint16_t PACKET_MAGIC = 0x4B42;
static constexpr int MP3_RX_PIN = 16; // ESP32 RX2 <- MP3 Trigger TX
static constexpr int MP3_TX_PIN = 17; // ESP32 TX2 -> MP3 Trigger RX
static constexpr uint32_t MP3_BAUD = 38400;
static constexpr int SABER_TX_PIN = 4; // ESP32 UART1 TX -> Sabertooth S1
static constexpr uint32_t SABER_BAUD = 9600;
static constexpr int SYREN_TX_PIN = 22; // ESP32 software UART TX -> SyRen S1
static constexpr uint32_t SYREN_BAUD = 9600;
static constexpr uint8_t BATTERY_SENSE_PIN = 32;
// Divider: battery+ -- 100k -- GPIO32 -- 15k -- GND (ratio 7.667, max ~25V).
static constexpr float BATTERY_DIVIDER_RATIO = (100.0f + 15.0f) / 15.0f;
static constexpr uint32_t LINK_TIMEOUT_MS = 750;
static constexpr int AXIS_DEADBAND = 160;
static constexpr uint8_t EVENT_FEEDBACK = 0x80;
static constexpr uint32_t FEEDBACK_INTERVAL_MS = 500;
static constexpr uint8_t EVENT_ANALOG = 2;
static constexpr uint8_t EVENT_BUTTON = 3;
static constexpr uint8_t EVENT_TRACK = 5;
static constexpr uint8_t EVENT_KEEPALIVE = 6;
static constexpr uint8_t PHYSICAL_BUTTON_CODE_BASE = 65;
HardwareSerial mp3Serial(2);
HardwareSerial sabertoothSerial(1);
EspSoftwareSerial::UART syrenSerial;
uint16_t analogValues[4] = {2048, 2048, 2048, 2048};
bool axisSeen[2] = {false, false};
volatile uint32_t lastInputAt = 0;
volatile int pendingSyrenPower = 0;
volatile bool syrenUpdatePending = false;
int lastMotor1 = 999;
int lastMotor2 = 999;
int lastSyrenPower = 999;
bool failsafeStopped = false;
uint16_t feedbackSequence = 0;
uint32_t lastFeedbackAt = 0;
uint32_t lastDebugSummaryAt = 0;
volatile uint32_t receivedPacketCount = 0;
volatile uint32_t rejectedPacketCount = 0;
volatile uint16_t lastReceivedSequence = 0;

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

// Simplified Serial byte encoding, equivalent to Dimension Engineering's
// SabertoothSimplified Arduino library.
void setSabertoothMotor(uint8_t motor, int power) {
  power = constrain(power, -127, 127);
  uint8_t magnitude = abs(power) >> 1;
  uint8_t command;
  if (motor == 1) {
    command = power < 0 ? 63 - magnitude : 64 + magnitude;
  } else {
    command = power < 0 ? 191 - magnitude : 192 + magnitude;
  }
  sabertoothSerial.write(command);
}

int axisToPower(uint16_t raw) {
  int centered = (int)raw - 2048;
  if (abs(centered) <= AXIS_DEADBAND) return 0;
  int adjusted = centered > 0 ? centered - AXIS_DEADBAND : centered + AXIS_DEADBAND;
  return constrain(adjusted * 127 / (2048 - AXIS_DEADBAND), -127, 127);
}

bool sendTrackToAudioBoard(uint16_t trackNumber) {
  if (trackNumber < 1 || trackNumber > 64) {
    Serial.printf("Ignoring invalid audio track number %u\n", trackNumber);
    return false;
  }
  mp3Serial.write('t');
  mp3Serial.write(static_cast<uint8_t>(trackNumber));
  Serial.printf("Playing track %u on audio board\n", trackNumber);
  return true;
}

uint8_t syrenCommandForPower(int power) {
  power = constrain(power, -127, 127);
  // SyRen simplified serial: 0 full reverse, 128 stop, 255 full forward.
  return static_cast<uint8_t>(power < 0 ? 127 + power : 128 + power);
}

void updateDrive() {
  if (!axisSeen[0] || !axisSeen[1]) return;
  int throttle = axisToPower(analogValues[0]); // analog input 1: forward/reverse
  int steering = axisToPower(analogValues[1]); // analog input 2: left/right
  int motor1 = throttle - steering;
  int motor2 = throttle + steering;
  int peak = max(abs(motor1), abs(motor2));
  if (peak > 127) {
    motor1 = motor1 * 127 / peak;
    motor2 = motor2 * 127 / peak;
  }
  if (motor1 != lastMotor1 || motor2 != lastMotor2) {
    setSabertoothMotor(1, motor1);
    setSabertoothMotor(2, motor2);
    lastMotor1 = motor1;
    lastMotor2 = motor2;
    failsafeStopped = false;
    Serial.printf("Sabertooth M1=%d M2=%d\n", motor1, motor2);
  }
}

void stopMotors(const char *reason) {
  setSabertoothMotor(1, 0);
  setSabertoothMotor(2, 0);
  lastMotor1 = 0;
  lastMotor2 = 0;
  syrenSerial.write(syrenCommandForPower(0));
  lastSyrenPower = 0;
  failsafeStopped = true;
  Serial.printf("FAILSAFE: motors stopped (%s)\n", reason);
}

void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(InputPacket)) {
    ++rejectedPacketCount;
    Serial.printf("ESP-NOW RX rejected: length=%d expected=%u\n", len,
                  static_cast<unsigned>(sizeof(InputPacket)));
    return;
  }
  InputPacket packet;
  memcpy(&packet, data, sizeof(packet));
  if (packet.magic != PACKET_MAGIC) {
    ++rejectedPacketCount;
    Serial.printf("ESP-NOW RX rejected: bad magic 0x%04X\n", packet.magic);
    return;
  }
  ++receivedPacketCount;
  lastReceivedSequence = packet.sequence;
  lastInputAt = millis();
  failsafeStopped = false;

  if (packet.type == EVENT_KEEPALIVE) return;
  if (packet.type == EVENT_ANALOG) {
    if (packet.code < 4) {
      analogValues[packet.code] = packet.value;
      if (packet.code < 2) {
        axisSeen[packet.code] = true;
        updateDrive();
      }
      if (packet.code == 2) {
        pendingSyrenPower = axisToPower(packet.value); // analog input 3: SyRen speed/direction
        syrenUpdatePending = true;
      }
    }
    return;
  }
  if (packet.type == EVENT_BUTTON) {
    if (packet.code >= PHYSICAL_BUTTON_CODE_BASE &&
        packet.code < PHYSICAL_BUTTON_CODE_BASE + 6) {
      Serial.printf("BUTTON %u %s  seq=%u\n",
                    packet.code - PHYSICAL_BUTTON_CODE_BASE + 1,
                    packet.value ? "PRESSED" : "RELEASED", packet.sequence);
    } else {
      Serial.printf("Ignoring invalid button value %u\n", packet.code);
    }
    return;
  }
  if (packet.type == EVENT_TRACK) {
    uint16_t trackNumber = packet.value;
    sendTrackToAudioBoard(trackNumber);
    return;
  }
  Serial.printf("ESP-NOW RX unknown event type=%u code=%u value=%u seq=%u\n",
                packet.type, packet.code, packet.value, packet.sequence);
}

void onSent(const wifi_tx_info_t *txInfo, esp_now_send_status_t status) {
  (void)txInfo;
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("ESP-NOW TX failed: receiver feedback broadcast was not delivered");
  }
}

void sendFeedback() {
  if (lastInputAt == 0 || millis() - lastFeedbackAt < FEEDBACK_INTERVAL_MS) return;
  lastFeedbackAt = millis();
  uint32_t receiverPinMv = analogReadMilliVolts(BATTERY_SENSE_PIN);
  FeedbackPacket feedback{};
  feedback.magic = PACKET_MAGIC;
  feedback.type = EVENT_FEEDBACK;
  feedback.sequence = feedbackSequence++;
  for (uint8_t i = 0; i < 4; ++i) feedback.analogPosition[i] = analogValues[i];
  feedback.sabertoothMotor1 = static_cast<int8_t>(lastMotor1 == 999 ? 0 : lastMotor1);
  feedback.sabertoothMotor2 = static_cast<int8_t>(lastMotor2 == 999 ? 0 : lastMotor2);
  feedback.syrenMotor = static_cast<int8_t>(lastSyrenPower == 999 ? 0 : lastSyrenPower);
  feedback.receiverBatteryMv = static_cast<uint16_t>(receiverPinMv * BATTERY_DIVIDER_RATIO);

  const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  if (esp_now_send(broadcastMac, reinterpret_cast<const uint8_t *>(&feedback),
                   sizeof(feedback)) != ESP_OK) {
    Serial.println("ESP-NOW TX enqueue failed: feedback broadcast");
  }
}

void printDebugSummary() {
  uint32_t now = millis();
  if (now - lastDebugSummaryAt < 1000) return;
  lastDebugSummaryAt = now;
  uint32_t age = lastInputAt == 0 ? UINT32_MAX : now - lastInputAt;
  Serial.printf("STATUS rx=%lu rejected=%lu seq=%u age=%s%lu ms A1=%u A2=%u A3=%u A4=%u motors=%d,%d syren=%d\n",
                static_cast<unsigned long>(receivedPacketCount),
                static_cast<unsigned long>(rejectedPacketCount),
                lastReceivedSequence, age == UINT32_MAX ? ">" : "",
                static_cast<unsigned long>(age == UINT32_MAX ? 999999 : age),
                analogValues[0], analogValues[1], analogValues[2], analogValues[3],
                lastMotor1 == 999 ? 0 : lastMotor1,
                lastMotor2 == 999 ? 0 : lastMotor2,
                lastSyrenPower == 999 ? 0 : lastSyrenPower);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  
  Serial.println("\n=== ESP-NOW vehicle receiver boot ===");
    uint8_t mac[6];
  esp_err_t err = esp_base_mac_addr_get(mac);
  switch (err) {
    case ESP_OK:
      Serial.printf(MACSTR "\n", MAC2STR(mac));  // both macros also in esp_mac.h
      break;
    case ESP_ERR_INVALID_MAC:
      Serial.println("base MAC address has not been set");
      break;
    default:
      Serial.printf("other error: 0x%x\n", err);
      break;
  }

  analogReadResolution(12);
  pinMode(BATTERY_SENSE_PIN, INPUT);
  mp3Serial.begin(MP3_BAUD, SERIAL_8N1, MP3_RX_PIN, MP3_TX_PIN);
  sabertoothSerial.begin(SABER_BAUD, SERIAL_8N1, -1, SABER_TX_PIN);
  syrenSerial.begin(SYREN_BAUD, EspSoftwareSerial::SWSERIAL_8N1, -1, SYREN_TX_PIN);
  Serial.printf("UART setup: MP3 RX=%d TX=%d @%lu; Sabertooth TX=%d @%lu; SyRen TX=%d @%lu\n",
                MP3_RX_PIN, MP3_TX_PIN, static_cast<unsigned long>(MP3_BAUD),
                SABER_TX_PIN, static_cast<unsigned long>(SABER_BAUD),
                SYREN_TX_PIN, static_cast<unsigned long>(SYREN_BAUD));
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();


  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.print("Receiver STA MAC (copy into remote.ino): ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) delay(100);
  }
  esp_now_register_send_cb(onSent);
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
  Serial.printf("Listening on ESP-NOW channel %u; MP3 Trigger UART %lu baud; Sabertooth UART %lu baud; SyRen Simplified Serial %lu baud\n",
                WIFI_CHANNEL, static_cast<unsigned long>(MP3_BAUD),
                static_cast<unsigned long>(SABER_BAUD),
                static_cast<unsigned long>(SYREN_BAUD));
  Serial.println("Startup complete; waiting for transmitter packets.");
  stopMotors("startup safety stop");
}

void loop() {
  if (syrenUpdatePending) {
    int power = pendingSyrenPower;
    syrenUpdatePending = false;
    if (power != lastSyrenPower) {
      syrenSerial.write(syrenCommandForPower(power));
      lastSyrenPower = power;
      Serial.printf("SyRen power=%d\n", power);
    }
  }
  if (lastInputAt != 0 && !failsafeStopped && millis() - lastInputAt > LINK_TIMEOUT_MS) {
    stopMotors("transmitter packet timeout");
  }
  sendFeedback();
  printDebugSummary();
  delay(100);
}
