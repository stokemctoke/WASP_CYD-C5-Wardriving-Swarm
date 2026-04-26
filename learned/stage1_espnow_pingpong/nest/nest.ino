/*
 * NEST - Stage 1: ESP-NOW Ping-Pong
 * Board: CYD (JC2432W328C) — standard ESP32
 *
 * Goal: prove ESP-NOW works between a standard ESP32 and an ESP32-C5.
 *
 * What this sketch does:
 *   1. Prints its own MAC address so you can note it down.
 *   2. Broadcasts a PING packet every 2 seconds.
 *   3. Listens for PONG replies and prints the sender MAC + RSSI.
 *
 * Expected serial output when a worker bee is nearby:
 *   [NEST] MAC: AA:BB:CC:DD:EE:FF
 *   [NEST] ESP-NOW ready — sending pings on channel 1
 *   [NEST] TX PING #0 ... sent
 *   [NEST] RX PONG #0 from 11:22:33:44:55:66 | RSSI -45 dBm
 *   [NEST] TX PING #1 ... sent
 *   [NEST] RX PONG #1 from 11:22:33:44:55:66 | RSSI -44 dBm
 *
 * Board package required: arduino-esp32 v3.x
 * No extra libraries needed.
 */

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>

// Both devices must use the same channel.
// Channel 1 is a safe default — not used by ESP-NOW internally.
#define ESPNOW_CHANNEL 1
#define PING_INTERVAL_MS 2000

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Shared message layout for both PING and PONG.
// __attribute__((packed)) ensures no padding bytes are inserted,
// so both ends read the struct the same way regardless of architecture.
typedef struct __attribute__((packed)) {
  char     magic[4];  // "PING" or "PONG"
  uint32_t counter;   // increments with each ping; pong echoes it back
} msg_t;

static uint32_t g_ping_counter = 0;
static uint32_t g_last_ping_ms = 0;

// Called by the ESP-NOW stack whenever a packet arrives.
// Runs in a WiFi task context — keep it short, no heavy work here.
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!info || !data || len < (int)sizeof(msg_t)) return;

  const msg_t *msg = (const msg_t *)data;

  if (memcmp(msg->magic, "PONG", 4) != 0) return;

  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
           info->src_addr[0], info->src_addr[1], info->src_addr[2],
           info->src_addr[3], info->src_addr[4], info->src_addr[5]);

  Serial.printf("[NEST] RX PONG #%lu from %s | RSSI %d dBm\n",
                (unsigned long)msg->counter,
                mac_str,
                info->rx_ctrl ? info->rx_ctrl->rssi : 0);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  delay(500);

  // STA mode is required for ESP-NOW even when not connecting to an AP.
  // It brings the WiFi radio up so ESP-NOW has a stack to sit on top of.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();  // make sure we're not trying to join any saved AP

  Serial.printf("\n[NEST] MAC: %s\n", WiFi.macAddress().c_str());

  // Lock both devices to the same channel before init.
  // Without this, each device picks a default and they may not match.
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[NEST] ERROR: esp_now_init() failed — halting");
    while (true) delay(1000);
  }

  esp_now_register_recv_cb(onDataRecv);

  // Register the broadcast address as a peer so we can send to it.
  // encrypt=false keeps it simple for this test.
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  Serial.printf("[NEST] ESP-NOW ready — sending pings on channel %d\n", ESPNOW_CHANNEL);
}

void loop() {
  uint32_t now = millis();

  if (now - g_last_ping_ms >= PING_INTERVAL_MS) {
    g_last_ping_ms = now;

    msg_t msg;
    memcpy(msg.magic, "PING", 4);
    msg.counter = g_ping_counter++;

    esp_err_t result = esp_now_send(BROADCAST_MAC, (uint8_t *)&msg, sizeof(msg));
    Serial.printf("[NEST] TX PING #%lu ... %s\n",
                  (unsigned long)msg.counter,
                  (result == ESP_OK) ? "sent" : "FAILED");
  }
}
