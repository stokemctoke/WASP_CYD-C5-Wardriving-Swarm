/*
 * NEST - Stage 9: WiGLE + WDGWars Upload
 * Board: CYD (JC2432W328C) — standard ESP32
 *
 * Extends Stage 7 with:
 *   - wasp.cfg parser — reads credentials from SD at boot (no recompile)
 *   - Home WiFi STA connection
 *   - WiGLE and WDGWars CSV upload via REST API
 *
 * Place wasp.cfg on the Nest SD card. See wasp.cfg.example in the repo root.
 *
 * ── TFT_eSPI setup (required before first flash) ─────────────────────────────
 * Install "TFT_eSPI" by Bodmer via Library Manager.
 * Edit Arduino/libraries/TFT_eSPI/User_Setup.h — comment out existing driver
 * and pins, then add:
 *
 *   #define ILI9341_DRIVER
 *   #define TFT_MISO  12
 *   #define TFT_MOSI  13
 *   #define TFT_SCLK  14
 *   #define TFT_CS    15
 *   #define TFT_DC     2
 *   #define TFT_RST    4
 *   #define LOAD_GLCD
 *   #define LOAD_FONT2
 *   #define LOAD_FONT4
 *   #define SPI_FREQUENCY       40000000
 *   #define SPI_READ_FREQUENCY  20000000
 *
 * ── CYD wiring ───────────────────────────────────────────────────────────────
 *   Display backlight : GPIO21  (driven HIGH)
 *   Display SPI       : HSPI — configured in User_Setup.h above
 *   SD card SPI       : VSPI — CS=5  SCK=18  MISO=19  MOSI=23
 */

#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>

// ── Pins ──────────────────────────────────────────────────────────────────────
#define TFT_BACKLIGHT  21
#define SD_CS           5
#define SD_SCK         18
#define SD_MISO        19
#define SD_MOSI        23

// ── Network ───────────────────────────────────────────────────────────────────
#define ESPNOW_CHANNEL  1

// ── Display layout (240 × 320 portrait) ──────────────────────────────────────
// HEADER(28) + STATUS(16) + 5 × ROW(52) + FOOTER(16) = 320
#define HEADER_H  28
#define STATUS_H  16
#define ROW_H     52
#define MAX_ROWS   5
#define FOOTER_H  16

// ── Colors (RGB565) ───────────────────────────────────────────────────────────
#define CLR_BG        TFT_BLACK
#define CLR_HDR_BG    0xC600    // wasp amber
#define CLR_HDR_FG    TFT_BLACK
#define CLR_LABEL     TFT_LIGHTGREY
#define CLR_ACTIVE    TFT_GREEN
#define CLR_STALE     TFT_YELLOW
#define CLR_OFFLINE   0x4208    // dark grey
#define CLR_GPS_OK    TFT_CYAN
#define CLR_GPS_NO    0xFBE0    // orange
#define CLR_DIVIDER   0x2104
#define CLR_FTR_BG    0x1082    // near-black

// ── Packet types ─────────────────────────────────────────────────────────────
#define WASP_PKT_SUMMARY   0x01
#define WASP_PKT_HEARTBEAT 0x02
#define WASP_FIRMWARE_VER  9

// ── Extended packet header (appended to both packet types) ───────────────────
// All fields zero-filled by senders that do not yet populate them.
// Nest never rejects a packet solely because extended fields are zero.
//
//  swarmId     — djb2 hash of swarm name; nest filters ESP-NOW on this
//  loyaltyLevel— 0=wild, 255=fully loyal (drone game mechanic)
//  gangId      — WDGWars gang affiliation (0=ungrouped)
//  firmwareVer — sender firmware stage; for cross-swarm compat checks
//  battLevel   — 0–100 %, 255=unknown
//  playerLevel — rank / promotion mechanic
//  boostLevel  — active boost / buff tier
//  reserved    — zero-filled; reserved for future game fields
//
// heartbeat_t  : 8 + 16 = 24 bytes
// scan_summary_t: 36 + 16 = 52 bytes

typedef struct __attribute__((packed)) {
  uint8_t  type;
  uint8_t  workerMac[6];
  uint8_t  nodeType;       // 0x00 = worker, 0x01 = drone
  uint16_t swarmId;
  uint8_t  loyaltyLevel;
  uint8_t  gangId;
  uint8_t  firmwareVer;
  uint8_t  battLevel;
  uint16_t playerLevel;
  uint8_t  boostLevel;
  uint8_t  reserved[7];
} heartbeat_t;             // 24 bytes

typedef struct __attribute__((packed)) {
  uint8_t  type;
  uint8_t  workerMac[6];
  uint8_t  gpsFix;
  float    lat, lon, altM;
  uint8_t  sats;
  float    hdop;
  uint16_t wifiTotal;
  uint8_t  wifi2g, wifi5g;
  uint16_t bleCount;
  int8_t   bestRssi;
  uint32_t cycleCount;
  uint16_t swarmId;
  uint8_t  loyaltyLevel;
  uint8_t  gangId;
  uint8_t  firmwareVer;
  uint8_t  battLevel;
  uint16_t playerLevel;
  uint8_t  boostLevel;
  uint8_t  reserved[7];
} scan_summary_t;          // 52 bytes

// ── Config ────────────────────────────────────────────────────────────────────

struct wasp_config_t {
  char homeSsid[64];
  char homePsk[64];
  char apSsid[32];
  char apPsk[32];
  char wigleBasicToken[128];
  char wdgwarsApiKey[72];   // 64 hex chars + null
};

static wasp_config_t cfg = {};   // zero-init; defaults applied in loadConfig()

static bool loadConfig() {
  // Defaults — overridden by wasp.cfg if present
  strlcpy(cfg.apSsid, "WASP-Nest", sizeof(cfg.apSsid));
  strlcpy(cfg.apPsk,  "waspswarm", sizeof(cfg.apPsk));

  File f = SD.open("/wasp.cfg");
  if (!f) {
    Serial.println("[CFG] /wasp.cfg not found — using defaults");
    return false;
  }

  int loaded = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) continue;

    int eq = line.indexOf('=');
    if (eq < 1) continue;

    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    key.trim();
    val.trim();

    if      (key == "homeSsid")        { val.toCharArray(cfg.homeSsid,        sizeof(cfg.homeSsid));        loaded++; }
    else if (key == "homePsk")         { val.toCharArray(cfg.homePsk,         sizeof(cfg.homePsk));         loaded++; }
    else if (key == "apSsid")          { val.toCharArray(cfg.apSsid,          sizeof(cfg.apSsid));          loaded++; }
    else if (key == "apPsk")           { val.toCharArray(cfg.apPsk,           sizeof(cfg.apPsk));           loaded++; }
    else if (key == "wigleBasicToken") { val.toCharArray(cfg.wigleBasicToken, sizeof(cfg.wigleBasicToken)); loaded++; }
    else if (key == "wdgwarsApiKey")   { val.toCharArray(cfg.wdgwarsApiKey,   sizeof(cfg.wdgwarsApiKey));   loaded++; }
    else {
      Serial.printf("[CFG] Unknown key ignored: %s\n", key.c_str());
    }
  }

  f.close();
  Serial.printf("[CFG] Loaded %d key(s) from /wasp.cfg\n", loaded);
  Serial.printf("[CFG]   AP      : %s\n", cfg.apSsid);
  Serial.printf("[CFG]   Home    : %s\n", cfg.homeSsid[0] ? cfg.homeSsid : "(not set)");
  Serial.printf("[CFG]   WiGLE   : %s\n", cfg.wigleBasicToken[0] ? "token set" : "(not set)");
  Serial.printf("[CFG]   WDGWars : %s\n", cfg.wdgwarsApiKey[0]   ? "key set"   : "(not set)");
  return loaded > 0;
}

// ── Worker registry ───────────────────────────────────────────────────────────
#define MAX_WORKERS        8
#define WORKER_TIMEOUT_MS  30000   // grey on display after 30 s
#define WORKER_DISPLAY_MS  90000   // hide from display after 90 s (18 missed heartbeats)
#define WORKER_REMOVE_MS  300000   // free registry slot after 5 min

struct worker_entry_t {
  uint8_t  mac[6];
  uint32_t lastSeenMs;
  uint32_t lastSummaryMs;
  int8_t   rssi;
  uint8_t  gpsFix;
  uint16_t wifiTotal;
  uint8_t  wifi2g, wifi5g;
  uint16_t bleCount;
  uint32_t cycleCount;
  uint8_t  nodeType;   // 0 = worker, 1 = drone
};
static worker_entry_t workers[MAX_WORKERS] = {};

static bool sdOk = false;
static char lastSyncStr[48] = "none";

TFT_eSPI  tft = TFT_eSPI();
SPIClass  sdSpi(VSPI);
WebServer   server(80);
WiFiServer  rawServer(8080);

// ── Worker registry helpers ───────────────────────────────────────────────────

static int findOrAddWorker(const uint8_t* mac) {
  for (int i = 0; i < MAX_WORKERS; i++)
    if (memcmp(workers[i].mac, mac, 6) == 0) return i;
  for (int i = 0; i < MAX_WORKERS; i++)
    if (workers[i].lastSeenMs == 0) { memcpy(workers[i].mac, mac, 6); return i; }
  return -1;
}

static int countActiveWorkers() {
  int n = 0;
  uint32_t now = millis();
  for (int i = 0; i < MAX_WORKERS; i++)
    if (workers[i].lastSeenMs > 0 && (now - workers[i].lastSeenMs) < WORKER_TIMEOUT_MS) n++;
  return n;
}

// ── WiFi events ───────────────────────────────────────────────────────────────

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

  if (data[0] == WASP_PKT_HEARTBEAT && len >= 7) {
    const heartbeat_t* pkt = (const heartbeat_t*)data;
    uint8_t nodeType = (len >= 8)                    ? pkt->nodeType : 0;
    // extended fields present only in stage9+ firmware
    bool ext         = (len >= (int)sizeof(heartbeat_t));

    int idx = findOrAddWorker(pkt->workerMac);
    uint32_t ago = 0;
    if (idx >= 0) {
      ago = (workers[idx].lastSeenMs > 0) ? (millis() - workers[idx].lastSeenMs) / 1000 : 0;
      workers[idx].lastSeenMs = millis();
      workers[idx].rssi       = (int8_t)rssi;
      workers[idx].nodeType   = nodeType;
    }
    int inRange = countActiveWorkers();
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             pkt->workerMac[0], pkt->workerMac[1], pkt->workerMac[2],
             pkt->workerMac[3], pkt->workerMac[4], pkt->workerMac[5]);
    const char* nodeEmoji = nodeType ? "🛸" : "🐝";
    char agoBuf[12];
    if (ago == 0) snprintf(agoBuf, sizeof(agoBuf), "first");
    else          snprintf(agoBuf, sizeof(agoBuf), "%lus ago", (unsigned long)ago);
    Serial.printf("🍯 [NEST]: %s💓 Detected | 🧥 %s | 📶 %d dBm | (%s)  %d in range\n",
                  nodeEmoji, mac, rssi, agoBuf, inRange);
    return;
  }

  if (data[0] != WASP_PKT_SUMMARY || len < 36) return;  // 36 = v1 minimum (stage8)

  const scan_summary_t* pkt = (const scan_summary_t*)data;
  int idx = findOrAddWorker(pkt->workerMac);
  if (idx >= 0) {
    workers[idx].lastSeenMs    = millis();
    workers[idx].lastSummaryMs = millis();
    workers[idx].rssi          = (int8_t)rssi;
    workers[idx].gpsFix        = pkt->gpsFix;
    workers[idx].wifiTotal     = pkt->wifiTotal;
    workers[idx].wifi2g        = pkt->wifi2g;
    workers[idx].wifi5g        = pkt->wifi5g;
    workers[idx].bleCount      = pkt->bleCount;
    workers[idx].cycleCount    = pkt->cycleCount;
  }
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

// ── Raw TCP upload handler (port 8080) ────────────────────────────────────────
// Header: "UPLOAD <MAC_NO_COLONS> <FILENAME> <SIZE_BYTES>\n"
// Response: "OK\n" or "ERR <reason>\n"

static void handleRawUpload() {
  WiFiClient client = rawServer.accept();
  if (!client) return;

  client.setTimeout(5000);
  String hdr = client.readStringUntil('\n');
  hdr.trim();

  int s1 = hdr.indexOf(' ');
  int s2 = s1 > 0 ? hdr.indexOf(' ', s1 + 1) : -1;
  int s3 = s2 > 0 ? hdr.indexOf(' ', s2 + 1) : -1;
  if (s3 < 0 || !hdr.startsWith("UPLOAD")) {
    client.println("ERR bad header");
    client.stop();
    return;
  }

  String workerMac = hdr.substring(s1 + 1, s2);
  String fileName  = hdr.substring(s2 + 1, s3);
  int    fileSize  = hdr.substring(s3 + 1).toInt();

  if (!sdOk || fileSize <= 0 || fileName.isEmpty() || workerMac.isEmpty()) {
    client.println("ERR bad params");
    client.stop();
    return;
  }

  String dir  = "/logs/" + workerMac;
  String path = dir + "/" + fileName;
  if (!SD.exists("/logs")) SD.mkdir("/logs");
  if (!SD.exists(dir))     SD.mkdir(dir);

  File f = SD.open(path.c_str(), FILE_WRITE);
  if (!f) { client.println("ERR sd open failed"); client.stop(); return; }

  uint8_t buf[256];
  int remaining = fileSize;
  while (remaining > 0 && client.connected()) {
    int toRead = min(remaining, (int)sizeof(buf));
    int n      = client.readBytes(buf, toRead);
    if (n <= 0) break;
    f.write(buf, n);
    remaining -= n;
  }

  size_t saved = f.size();
  f.close();
  client.println("OK");
  client.stop();

  snprintf(lastSyncStr, sizeof(lastSyncStr), "%s  %dB", workerMac.c_str(), (int)saved);
  Serial.printf("[NEST] Saved %s (%d bytes)\n", path.c_str(), (int)saved);
}

// ── HTTP upload handler (drone small CSV uploads) ─────────────────────────────

static void handleUpload() {
  if (!sdOk) { server.send(503, "text/plain", "SD not ready"); return; }

  String workerMac = server.arg("worker");
  String fileName  = server.arg("file");

  for (unsigned int i = 0; i < fileName.length(); i++) {
    char c = fileName[i];
    if (!isAlphaNumeric(c) && c != '_' && c != '-' && c != '.') {
      server.send(400, "text/plain", "Bad filename"); return;
    }
  }
  if (fileName.isEmpty() || !fileName.endsWith(".csv")) {
    server.send(400, "text/plain", "Filename must end in .csv"); return;
  }

  String dir  = "/logs/" + workerMac;
  String path = dir + "/" + fileName;

  if (!SD.exists("/logs")) SD.mkdir("/logs");
  if (!SD.exists(dir))     SD.mkdir(dir);

  String body = server.arg("plain");
  File f = SD.open(path.c_str(), FILE_WRITE);
  if (!f) { server.send(500, "text/plain", "SD write failed"); return; }
  f.print(body);
  f.close();

  snprintf(lastSyncStr, sizeof(lastSyncStr), "%s  %dB", workerMac.c_str(), body.length());
  Serial.printf("[NEST] Saved %s (%d bytes)\n", path.c_str(), body.length());
  server.send(200, "text/plain", "OK");
}

// ── Display helpers ───────────────────────────────────────────────────────────

static uint16_t rssiColor(int8_t r) {
  if (r > -50) return TFT_GREEN;
  if (r > -70) return TFT_YELLOW;
  if (r > -85) return 0xFBE0;
  return TFT_RED;
}

static void drawBootMsg(const char* msg) {
  tft.setTextFont(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(CLR_LABEL, CLR_BG);
  tft.fillRect(0, HEADER_H + STATUS_H, 240, ROW_H, CLR_BG);
  tft.drawString(msg, 120, HEADER_H + STATUS_H + ROW_H / 2);
}

// ── Display drawing ───────────────────────────────────────────────────────────

static void drawHeader() {
  tft.fillRect(0, 0, 240, HEADER_H, CLR_HDR_BG);
  tft.setTextColor(CLR_HDR_FG, CLR_HDR_BG);
  tft.setTextDatum(ML_DATUM);
  tft.setTextFont(4);
  tft.drawString("W.A.S.P. NEST", 6, HEADER_H / 2);
  tft.setTextFont(2);
  tft.setTextDatum(MR_DATUM);
  char buf[12];
  snprintf(buf, sizeof(buf), "ch:%d", ESPNOW_CHANNEL);
  tft.drawString(buf, 234, HEADER_H / 2);
}

static void drawWorkerRow(int row, const worker_entry_t& w) {
  int y = HEADER_H + STATUS_H + row * ROW_H;
  uint32_t age = millis() - w.lastSeenMs;
  bool active  = age < WORKER_TIMEOUT_MS;
  bool stale   = age >= 10000 && active;

  bool     isDrone   = (w.nodeType == 1);
  uint16_t baseColor = isDrone ? TFT_CYAN : CLR_ACTIVE;
  uint16_t nameColor = active ? (stale ? CLR_STALE : baseColor) : CLR_OFFLINE;

  tft.fillRect(0, y, 240, ROW_H, CLR_BG);
  tft.drawFastHLine(0, y + ROW_H - 1, 240, CLR_DIVIDER);

  tft.fillCircle(8, y + 14, 5, nameColor);

  tft.setTextFont(1);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(nameColor, CLR_BG);
  tft.drawString(isDrone ? "D" : "W", 16, y + 10);

  char mac[18];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           w.mac[0], w.mac[1], w.mac[2], w.mac[3], w.mac[4], w.mac[5]);
  tft.setTextFont(2);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(nameColor, CLR_BG);
  tft.drawString(mac, 26, y + 14);

  char rssiStr[10];
  snprintf(rssiStr, sizeof(rssiStr), "%d dBm", w.rssi);
  tft.setTextColor(rssiColor(w.rssi), CLR_BG);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(rssiStr, 234, y + 14);

  tft.setTextDatum(ML_DATUM);
  if (w.lastSummaryMs == 0) {
    tft.setTextColor(CLR_LABEL, CLR_BG);
    tft.drawString("awaiting scan data...", 26, y + 38);
    return;
  }

  if (w.gpsFix) {
    tft.setTextColor(CLR_GPS_OK, CLR_BG);
    tft.drawString("FIX", 26, y + 38);
  } else {
    tft.setTextColor(CLR_GPS_NO, CLR_BG);
    tft.drawString("NO FIX", 26, y + 38);
  }

  char scanLine[40];
  snprintf(scanLine, sizeof(scanLine), "W:%d(%d+%d) B:%d #%u",
           w.wifiTotal, w.wifi2g, w.wifi5g, w.bleCount, (unsigned)w.cycleCount);
  tft.setTextColor(CLR_LABEL, CLR_BG);
  tft.drawString(scanLine, w.gpsFix ? 64 : 94, y + 38);
}

static void refreshDisplay() {
  int active = countActiveWorkers();
  tft.fillRect(0, HEADER_H, 240, STATUS_H, CLR_BG);
  tft.setTextFont(2);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(CLR_LABEL, CLR_BG);
  char buf[32];
  snprintf(buf, sizeof(buf), "Workers in range: %d", active);
  tft.drawString(buf, 6, HEADER_H + STATUS_H / 2);

  uint32_t now = millis();
  int row = 0;
  for (int i = 0; i < MAX_WORKERS && row < MAX_ROWS; i++) {
    if (workers[i].lastSeenMs > 0 && (now - workers[i].lastSeenMs) < WORKER_DISPLAY_MS)
      drawWorkerRow(row++, workers[i]);
  }
  for (; row < MAX_ROWS; row++) {
    int y = HEADER_H + STATUS_H + row * ROW_H;
    tft.fillRect(0, y, 240, ROW_H, CLR_BG);
  }

  int fy = 320 - FOOTER_H;
  tft.fillRect(0, fy, 240, FOOTER_H, CLR_FTR_BG);
  tft.setTextFont(1);
  tft.setTextColor(CLR_LABEL, CLR_FTR_BG);
  tft.setTextDatum(ML_DATUM);
  char fbuf[52];
  snprintf(fbuf, sizeof(fbuf), "Last sync: %s", lastSyncStr);
  tft.drawString(fbuf, 6, fy + FOOTER_H / 2);
}

// ── Registry cleanup ──────────────────────────────────────────────────────────

static void cleanRegistry() {
  uint32_t now = millis();
  for (int i = 0; i < MAX_WORKERS; i++) {
    if (workers[i].lastSeenMs == 0) continue;
    if ((now - workers[i].lastSeenMs) < WORKER_REMOVE_MS) continue;
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             workers[i].mac[0], workers[i].mac[1], workers[i].mac[2],
             workers[i].mac[3], workers[i].mac[4], workers[i].mac[5]);
    Serial.printf("[NEST] Worker %s expired — slot freed\n", mac);
    memset(&workers[i], 0, sizeof(workers[i]));
  }
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n========================================");
  Serial.println(" W.A.S.P. Nest — Stage 9");
  Serial.println(" ESP-NOW + WiFi AP + File Sync + Display + Upload");
  Serial.println("========================================");

  pinMode(TFT_BACKLIGHT, OUTPUT);
  digitalWrite(TFT_BACKLIGHT, HIGH);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(CLR_BG);
  drawHeader();

  // SD — must come before loadConfig()
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  drawBootMsg("SD...");
  if (SD.begin(SD_CS, sdSpi)) {
    sdOk = true;
    if (!SD.exists("/logs")) SD.mkdir("/logs");
    Serial.println(" SD OK");
  } else {
    Serial.println(" SD FAIL — uploads will be rejected");
  }

  // Config — read from SD before any network setup
  drawBootMsg("Reading config...");
  loadConfig();

  // WiFi — AP uses credentials from config
  drawBootMsg("WiFi...");
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  String staMac = WiFi.macAddress();
  WiFi.softAP(cfg.apSsid, cfg.apPsk, ESPNOW_CHANNEL);
  delay(100);

  Serial.printf(" STA MAC: %s  (ESP-NOW peer address for workers)\n", staMac.c_str());
  Serial.printf(" AP  MAC: %s\n", WiFi.softAPmacAddress().c_str());
  Serial.printf(" AP: %-12s  IP: %s  ch: %d\n",
                cfg.apSsid, WiFi.softAPIP().toString().c_str(), ESPNOW_CHANNEL);

  // ESP-NOW
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    Serial.println(" ERROR: esp_now_init() failed");
  } else {
    esp_now_register_recv_cb(onDataRecv);
    Serial.printf(" ESP-NOW ready on channel %d\n", ESPNOW_CHANNEL);
  }

  // HTTP servers
  server.on("/upload", HTTP_POST, handleUpload);
  server.begin();
  rawServer.begin();
  Serial.println(" HTTP server started (port 80)");
  Serial.println(" Raw upload server started (port 8080)");
  Serial.println(" Ready — waiting for workers\n");

  tft.fillRect(0, HEADER_H, 240, 320 - HEADER_H, CLR_BG);
  refreshDisplay();
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void loop() {
  server.handleClient();
  handleRawUpload();

  static uint32_t lastRefresh = 0;
  static uint32_t lastClean   = 0;
  uint32_t now = millis();

  if (now - lastRefresh >= 1000) {
    refreshDisplay();
    lastRefresh = now;
  }
  if (now - lastClean >= 30000) {
    cleanRegistry();
    lastClean = now;
  }
}
