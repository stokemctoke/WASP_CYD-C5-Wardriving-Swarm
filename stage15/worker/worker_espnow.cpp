#include "worker_espnow.h"
#include "worker_gps.h"
#include "worker_led.h"
#include "worker_config.h"
#include <WiFi.h>
#include <esp_wifi.h>

uint32_t lastHeartbeatMs = 0;

void onSendResult(const wifi_tx_info_t*, esp_now_send_status_t status) {
  Serial.printf("[WORKER] ESP-NOW TX %s\n",
                status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAILED");
}

void initEspNow() {
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) { Serial.println("[WORKER] ESP-NOW init FAILED"); return; }
  esp_now_register_send_cb(onSendResult);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, NEST_MAC, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void sendHeartbeat() {
  heartbeat_t pkt = {};
  pkt.type        = WASP_PKT_HEARTBEAT;
  pkt.nodeType    = 0;
  pkt.firmwareVer = WASP_FIRMWARE_VER;
  WiFi.macAddress(pkt.workerMac);
  esp_now_send(NEST_MAC, (uint8_t*)&pkt, sizeof(pkt));
  lastHeartbeatMs = millis();
  ledHeartbeat();
}

void maybeHeartbeat() {
  extern bool droneMode;
  if (millis() - lastHeartbeatMs >= heartbeatIntervalMs) sendHeartbeat();
}

void sendSummary(int wifiTotal, int wifi2g, int wifi5g,
                 int bleCount, int8_t bestRssi) {
  extern bool droneMode;
  extern uint32_t cycleCount;
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  scan_summary_t pkt = {};
  pkt.type        = WASP_PKT_SUMMARY;
  pkt.firmwareVer = WASP_FIRMWARE_VER;
  pkt.gpsFix     = (gpsOk && gps.location.isValid()) ? 1 : 0;
  pkt.lat        = pkt.gpsFix ? (float)gps.location.lat() : 0.0f;
  pkt.lon        = pkt.gpsFix ? (float)gps.location.lng() : 0.0f;
  pkt.altM       = (pkt.gpsFix && gps.altitude.isValid()) ? (float)gps.altitude.meters() : 0.0f;
  pkt.sats       = (gpsOk && gps.satellites.isValid()) ? (uint8_t)gps.satellites.value() : 0;
  pkt.hdop       = (gpsOk && gps.hdop.isValid())       ? gps.hdop.hdop() : 99.99f;
  pkt.wifiTotal  = (uint16_t)wifiTotal;
  pkt.wifi2g     = (uint8_t)wifi2g;
  pkt.wifi5g     = (uint8_t)wifi5g;
  pkt.bleCount   = (uint16_t)bleCount;
  pkt.bestRssi   = bestRssi;
  pkt.cycleCount = cycleCount;
  uint8_t mac[6];
  WiFi.macAddress(mac);
  memcpy(pkt.workerMac, mac, 6);
  esp_now_send(NEST_MAC, (uint8_t*)&pkt, sizeof(pkt));
}
