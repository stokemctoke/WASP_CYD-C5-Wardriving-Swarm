/*
 * NEST - Stage 5: ESP-NOW Worker Summary Receiver
 * Board: CYD (JC2432W328C) — standard ESP32
 *
 * Listens for scan summary packets from Workers and prints them to serial.
 * No display code yet — that's Stage 7. This stage proves the streaming
 * link works before building UI on top of it.
 *
 * Expected serial output:
 *   [NEST] Worker 38:44:BE:BA:0F:30 | cycle 3 | GPS FIX 55.635780,-4.778969
 *          WiFi: 14 total (9 x 2.4G, 5 x 5G) best -50 dBm | BLE: 4
 *
 * Board:   ESP32 Dev Module
 * No extra libraries needed.
 */

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>

#define ESPNOW_CHANNEL 1

// Must match worker/worker.ino exactly
#define WASP_PKT_SUMMARY 0x01

typedef struct __attribute__((packed)) {
  uint8_t  type;
  uint8_t  workerMac[6];
  uint8_t  gpsFix;
  float    lat;
  float    lon;
  float    altM;
  uint8_t  sats;
  float    hdop;
  uint16_t wifiTotal;
  uint8_t  wifi2g;
  uint8_t  wifi5g;
  uint16_t bleCount;
  int8_t   bestRssi;
  uint32_t cycleCount;
} scan_summary_t;

void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (!info || !data || len < 1) return;
  if (data[0] != WASP_PKT_SUMMARY || len < (int)sizeof(scan_summary_t)) return;

  const scan_summary_t* pkt = (const scan_summary_t*)data;

  char mac[18];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           pkt->workerMac[0], pkt->workerMac[1], pkt->workerMac[2],
           pkt->workerMac[3], pkt->workerMac[4], pkt->workerMac[5]);

  Serial.printf("\n[NEST] Worker %s | cycle %lu | RSSI link %d dBm\n",
                mac, (unsigned long)pkt->cycleCount,
                info->rx_ctrl ? info->rx_ctrl->rssi : 0);

  if (pkt->gpsFix) {
    Serial.printf("       GPS FIX  %.6f, %.6f | alt %.1fm | sats %d | hdop %.2f\n",
                  pkt->lat, pkt->lon, pkt->altM, pkt->sats, pkt->hdop);
  } else {
    Serial.printf("       GPS NO FIX\n");
  }

  Serial.printf("       WiFi: %d total (%d x 2.4G, %d x 5G) best %d dBm\n",
                pkt->wifiTotal, pkt->wifi2g, pkt->wifi5g, pkt->bestRssi);
  Serial.printf("       BLE:  %d device(s)\n", pkt->bleCount);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n========================================");
  Serial.println(" W.A.S.P. Nest — Stage 5");
  Serial.println(" ESP-NOW Worker Summary Receiver");
  Serial.println("========================================");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  Serial.printf(" MAC: %s\n", WiFi.macAddress().c_str());

  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println(" ERROR: esp_now_init() failed — halting");
    while (true) delay(1000);
  }

  esp_now_register_recv_cb(onDataRecv);
  Serial.printf(" ESP-NOW ready — listening on channel %d\n", ESPNOW_CHANNEL);
  Serial.println(" Waiting for workers...\n");
}

void loop() {
  delay(10);
}
