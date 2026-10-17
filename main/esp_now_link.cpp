#include "esp_now_link.hpp"

#include <cassert>
#include <cstring>

#include "app_state.hpp"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static constexpr uint32_t KEEPALIVE_INTERVAL_MS = 250;

// Replace with the receiver's Wi-Fi station MAC, printed in its Serial Monitor.
static uint8_t receiverMac[] = {0xb4, 0xbf, 0xe9, 0x1c, 0x6a, 0x7c};

static void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;
  if (len != sizeof(FeedbackPacket)) return;

  FeedbackPacket feedback;
  std::memcpy(&feedback, data, sizeof(feedback));
  if (feedback.magic != PACKET_MAGIC || feedback.type != EVENT_FEEDBACK) return;

  appState.lastFeedback = feedback;
  appState.haveFeedback = true;
  appState.lastFeedbackAt = millis();
  Serial.printf(
      "RX feedback #%u | controls A1=%u A2=%u A3=%u A4=%u | Sabertooth M1=%d M2=%d | SyRen=%d | receiver battery=%.2fV | transmitter battery=%.2fV\n",
      feedback.sequence, feedback.analogPosition[0], feedback.analogPosition[1],
      feedback.analogPosition[2], feedback.analogPosition[3],
      feedback.sabertoothMotor1, feedback.sabertoothMotor2, feedback.syrenMotor,
      feedback.receiverBatteryMv / 1000.0f, feedback.transmitterBatteryMv / 1000.0f);
}

static void onSent(const esp_now_send_info_t *txInfo, esp_now_send_status_t status) {
  (void)txInfo;
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("ESP-NOW send failed");
  }
}

bool sendInput(uint8_t type, uint8_t code, uint16_t value) {
  InputPacket packet{PACKET_MAGIC, type, code, value, appState.sequenceNumber++};
  esp_err_t error = esp_now_send(receiverMac, reinterpret_cast<const uint8_t *>(&packet),
                                 sizeof(packet));
  if (error != ESP_OK) return false;

  Serial.printf("TX type=%u code=%u value=%u seq=%u\n",
                packet.type, packet.code, packet.value, packet.sequence);
  return true;
}

void sendKeepalive() {
  uint32_t now = millis();
  if (now - appState.lastKeepaliveAt < KEEPALIVE_INTERVAL_MS) return;
  appState.lastKeepaliveAt = now;
  if (!sendInput(EVENT_KEEPALIVE, 0, 0)) {
    Serial.println("Queueing ESP-NOW keepalive failed");
  }
}

void initializeEspNow() {
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  wifi_init_config_t wifiConfig = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifiConfig));
  esp_netif_t *stationNetif = esp_netif_create_default_wifi_sta();
  assert(stationNetif != nullptr);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

  uint8_t stationMac[6]{};
  ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, stationMac));
  Serial.printf("Remote STA MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                stationMac[0], stationMac[1], stationMac[2],
                stationMac[3], stationMac[4], stationMac[5]);

  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb(onSent));

  esp_now_peer_info_t peer{};
  std::memcpy(peer.peer_addr, receiverMac, sizeof(receiverMac));
  peer.channel = WIFI_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  ESP_ERROR_CHECK(esp_now_add_peer(&peer));

  const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_peer_info_t feedbackPeer{};
  std::memcpy(feedbackPeer.peer_addr, broadcastMac, sizeof(broadcastMac));
  feedbackPeer.channel = WIFI_CHANNEL;
  feedbackPeer.ifidx = WIFI_IF_STA;
  feedbackPeer.encrypt = false;
  ESP_ERROR_CHECK(esp_now_add_peer(&feedbackPeer));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(onReceive));
}
