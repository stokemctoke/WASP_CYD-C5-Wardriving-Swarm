/*
 * WORKER - Stage 1: ESP-NOW Ping-Pong
 * Board: Seeed XIAO ESP32-C5
 *
 * Goal: prove ESP-NOW works between an ESP32-C5 and a standard ESP32 (CYD).
 *
 * What this sketch does:
 *   1. Prints its own MAC address so you can note it down.
 *   2. Listens for PING broadcasts from the hive.
 *   3. Replies with a PONG directly to the sender (unicast, not broadcast).
 *
 * Expected serial output when a hive is nearby:
 *   [WORKER] MAC: 11:22:33:44:55:66
 *   [WORKER] ESP-NOW ready — listening on channel 1
 *   [WORKER] RX PING #0 from AA:BB:CC:DD:EE:FF | RSSI -43 dBm
 *   [WORKER] TX PONG #0 ... sent
 *   [WORKER] RX PING #1 from AA:BB:CC:DD:EE:FF | RSSI -43 dBm
 *   [WORKER] TX PONG #1 ... sent
 *
 * Board package required: arduino-esp32 v3.x (required for C5 support)
 * No extra libraries needed.
 */

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>

// Must match the channel in nest.ino
#define ESPNOW_CHANNEL 1

typedef struct __attribute__((packed)) {
  char     magic[4];  // "PING" or "PONG"
  uint32_t counter;
} msg_t;

// Called by the ESP-NOW stack on every received packet.
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!info || !data || len < (int)sizeof(msg_t)) return;

  const msg_t *msg = (const msg_t *)data;

  if (memcmp(msg->magic, "PING", 4) != 0) return;

  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
           info->src_addr[0], info->src_addr[1], info->src_addr[2],
           info->src_addr[3], info->src_addr[4], info->src_addr[5]);

  Serial.printf("[WORKER] RX PING #%lu from %s | RSSI %d dBm\n",
                (unsigned long)msg->counter,
                mac_str,
                info->rx_ctrl ? info->rx_ctrl->rssi : 0);

  // We need the sender registered as a peer before we can unicast to them.
  // Add them on first contact; ignore the error if already added.
  if (!esp_now_is_peer_exist(info->src_addr)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, info->src_addr, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
  }

  // Echo the counter back so the hive can match pongs to pings.
  msg_t pong;
  memcpy(pong.magic, "PONG", 4);
  pong.counter = msg->counter;

  esp_err_t result = esp_now_send(info->src_addr, (uint8_t *)&pong, sizeof(pong));
  Serial.printf("[WORKER] TX PONG #%lu ... %s\n",
                (unsigned long)pong.counter,
                (result == ESP_OK) ? "sent" : "FAILED");
}

void setup() {
  Serial.begin(115200);
  // Native USB CDC on the C5 doesn't need to wait for a host connection.
  // A fixed delay is enough; 'while (!Serial)' would block forever if the
  // serial monitor wasn't open before boot.
  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.printf("\n[WORKER] MAC: %s\n", WiFi.macAddress().c_str());

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[WORKER] ERROR: esp_now_init() failed — halting");
    while (true) delay(1000);
  }

  esp_now_register_recv_cb(onDataRecv);

  Serial.printf("[WORKER] ESP-NOW ready — listening on channel %d\n", ESPNOW_CHANNEL);
}

void loop() {
  // All work happens in the receive callback.
  // The loop just keeps the scheduler happy.
  delay(10);
}
