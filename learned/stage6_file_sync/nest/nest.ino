/*
 * NEST - Stage 6: ESP-NOW Receiver + WiFi AP + File Sync Server
 * Board: CYD (JC2432W328C) — standard ESP32
 *
 * Builds on Stage 5. The Nest now runs a WiFi AP ("WASP-Nest") on the
 * same channel as ESP-NOW so both can operate simultaneously. Workers
 * connect to the AP and POST their CSV logs; files are saved to the
 * Nest's SD card under /logs/<worker_mac>/. ESP-NOW summary reception
 * continues uninterrupted.
 *
 * Board:   ESP32 Dev Module
 * No extra libraries needed (WebServer and SD are built-in).
 *
 * CYD SD wiring (JC2432W328C built-in slot — verify against your board):
 *   CS   → GPIO5   (VSPI CS)
 *   SCK  → GPIO18  (VSPI CLK)
 *   MISO → GPIO19  (VSPI MISO)
 *   MOSI → GPIO23  (VSPI MOSI)
 */

#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SD.h>
#include <SPI.h>

#define ESPNOW_CHANNEL 1

#define AP_SSID "WASP-Nest"
#define AP_PASS "waspswarm"

// CYD built-in SD card (JC2432W328C VSPI bus)
#define SD_CS    5
#define SD_SCK  18
#define SD_MISO 19
#define SD_MOSI 23

#define WASP_PKT_SUMMARY   0x01
#define WASP_PKT_HEARTBEAT 0x02

typedef struct __attribute__((packed)) {
  uint8_t type;
  uint8_t workerMac[6];
} heartbeat_t;

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

#define MAX_WORKERS       8
#define WORKER_TIMEOUT_MS 30000

struct worker_entry_t { uint8_t mac[6]; uint32_t lastSeenMs; };
static worker_entry_t workers[MAX_WORKERS] = {};

static void updateWorkerSeen(const uint8_t* mac) {
  for (int i = 0; i < MAX_WORKERS; i++) {
    if (memcmp(workers[i].mac, mac, 6) == 0) { workers[i].lastSeenMs = millis(); return; }
  }
  for (int i = 0; i < MAX_WORKERS; i++) {
    if (workers[i].lastSeenMs == 0) { memcpy(workers[i].mac, mac, 6); workers[i].lastSeenMs = millis(); return; }
  }
}

static int countActiveWorkers() {
  int count = 0;
  uint32_t now = millis();
  for (int i = 0; i < MAX_WORKERS; i++) {
    if (workers[i].lastSeenMs > 0 && (now - workers[i].lastSeenMs) < WORKER_TIMEOUT_MS) count++;
  }
  return count;
}

static bool sdOk = false;
WebServer   server(80);

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             info.wifi_ap_staconnected.mac[0], info.wifi_ap_staconnected.mac[1],
             info.wifi_ap_staconnected.mac[2], info.wifi_ap_staconnected.mac[3],
             info.wifi_ap_staconnected.mac[4], info.wifi_ap_staconnected.mac[5]);
    Serial.printf("[NEST] Worker connected: %s\n", mac);
  } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    Serial.println("[NEST] Worker disconnected");
  }
}

// ── ESP-NOW receive ───────────────────────────────────────────────────────────

void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (!info || !data || len < 1) return;

  int rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
  char mac[18];

  if (data[0] == WASP_PKT_HEARTBEAT && len >= (int)sizeof(heartbeat_t)) {
    const heartbeat_t* pkt = (const heartbeat_t*)data;
    updateWorkerSeen(pkt->workerMac);
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             pkt->workerMac[0], pkt->workerMac[1], pkt->workerMac[2],
             pkt->workerMac[3], pkt->workerMac[4], pkt->workerMac[5]);
    Serial.printf("🍯 [NEST]: 🐝💓 Detected | 🧥 %s | 📶 %d dBm  (%d in range)\n",
                  mac, rssi, countActiveWorkers());
    return;
  }

  if (data[0] != WASP_PKT_SUMMARY || len < (int)sizeof(scan_summary_t)) return;

  const scan_summary_t* pkt = (const scan_summary_t*)data;
  updateWorkerSeen(pkt->workerMac);
  int inRange = countActiveWorkers();
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           pkt->workerMac[0], pkt->workerMac[1], pkt->workerMac[2],
           pkt->workerMac[3], pkt->workerMac[4], pkt->workerMac[5]);

  Serial.printf("\n🐝 [NEST] Worker %s | cycle %u | RSSI link %d dBm | (%d in range)\n",
                mac, (uint32_t)pkt->cycleCount, rssi, inRange);

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

// ── HTTP upload handler ───────────────────────────────────────────────────────

static void handleUpload() {
  if (!sdOk) {
    server.send(503, "text/plain", "SD not ready");
    return;
  }

  String workerMac = server.arg("worker");
  String fileName  = server.arg("file");

  // Only allow safe characters in filename
  for (unsigned int i = 0; i < fileName.length(); i++) {
    char c = fileName[i];
    if (!isAlphaNumeric(c) && c != '_' && c != '-' && c != '.') {
      server.send(400, "text/plain", "Bad filename");
      return;
    }
  }
  if (fileName.isEmpty() || !fileName.endsWith(".csv")) {
    server.send(400, "text/plain", "Filename must end in .csv");
    return;
  }

  String dir  = "/logs/" + workerMac;
  String path = dir + "/" + fileName;

  if (!SD.exists("/logs"))  SD.mkdir("/logs");
  if (!SD.exists(dir))      SD.mkdir(dir);

  String body = server.arg("plain");

  File f = SD.open(path.c_str(), FILE_WRITE);
  if (!f) {
    server.send(500, "text/plain", "SD write failed");
    return;
  }
  f.print(body);
  f.close();

  Serial.printf("[NEST] Saved %s (%d bytes)\n", path.c_str(), body.length());
  server.send(200, "text/plain", "OK");
}

// ── Setup / Loop ──────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n========================================");
  Serial.println(" W.A.S.P. Nest — Stage 6");
  Serial.println(" ESP-NOW + WiFi AP + File Sync Server");
  Serial.println("========================================");

  // SD
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, SPI)) {
    sdOk = true;
    if (!SD.exists("/logs")) SD.mkdir("/logs");
    Serial.println(" SD OK");
  } else {
    Serial.println(" SD FAIL — uploads will be rejected");
  }

  // AP_STA mode: STA interface keeps its original MAC for ESP-NOW,
  // AP interface runs alongside it on the same channel.
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  String staMac = WiFi.macAddress();
  WiFi.softAP(AP_SSID, AP_PASS, ESPNOW_CHANNEL);
  delay(100);

  Serial.printf(" STA MAC: %s  (ESP-NOW peer address for workers)\n", staMac.c_str());
  Serial.printf(" AP  MAC: %s\n", WiFi.softAPmacAddress().c_str());
  Serial.printf(" AP: %-12s  IP: %s  ch: %d\n",
                AP_SSID, WiFi.softAPIP().toString().c_str(), ESPNOW_CHANNEL);

  // ESP-NOW uses the STA interface MAC — must match NEST_MAC in worker code
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    Serial.println(" ERROR: esp_now_init() failed — summaries disabled");
  } else {
    esp_now_register_recv_cb(onDataRecv);
    Serial.printf(" ESP-NOW ready on channel %d\n", ESPNOW_CHANNEL);
  }

  // HTTP server
  server.on("/upload", HTTP_POST, handleUpload);
  server.begin();
  Serial.println(" HTTP server started");
  Serial.println(" Ready — waiting for workers\n");
}

void loop() {
  server.handleClient();
  delay(1);
}
