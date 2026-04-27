/*
 * W.A.S.P. — Unified Worker / Drone Firmware — Stage 9
 * Board: Seeed XIAO ESP32-C5 (on Xiao Expansion dev board)
 *
 * One firmware, two modes — auto-detected at boot:
 *
 *   WORKER — SD card found → logs to SD, GPS used if present
 *   DRONE  — no SD card   → logs to 25-slot RAM circular buffer
 *
 * Both modes sync to the Nest via HTTP upload every SYNC_EVERY cycles.
 * If the Nest is not in range, Drone data is retained in the buffer
 * (oldest slot overwritten first when full). Worker renames uploaded
 * files to .done as before.
 *
 * Compatible nest: stage8_unified/nest  (stage7_nest_display also works)
 *
 * ── Wiring (XIAO Expansion Board) ─────────────────────────────────────────────────
 *   GPS TX  →  D7 (GPIO12)   UART1 RX
 *   GPS RX  →  D6 (GPIO11)   UART1 TX  (optional)
 *   SD CS   →  D2 (GPIO25)   SPI CS
 *   SD SCK  →  D8 (GPIO8)    SPI SCK
 *   SD MISO →  D9 (GPIO9)    SPI MISO
 *   SD MOSI →  D10 (GPIO10)  SPI MOSI
 *
 * ── Arduino IDE settings ──────────────────────────────────────────────────────
 *   Tools > Board              > XIAO_ESP32C5
 *   Tools > USB CDC On Boot    > Enabled
 *   Tools > Partition Scheme   > Huge APP (3MB No OTA/1MB SPIFFS)
 *   Tools > PSRAM              > Disabled
 *
 * ── Libraries ────────────────────────────────────────────────────────────────
 *   NimBLE-Arduino by h2zero   (Tools > Manage Libraries)
 *   TinyGPS++ by Mikal Hart     (Tools > Manage Libraries)
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <TinyGPS++.h>
#include <SD.h>
#include <SPI.h>
#include <vector>
#include <sys/time.h>

// ── ESP-NOW ───────────────────────────────────────────────────────────────────
#define ESPNOW_CHANNEL 1
static const uint8_t NEST_MAC[6] = {0xA4, 0xF0, 0x0F, 0x5D, 0x96, 0xD4};

#define WASP_PKT_SUMMARY   0x01
#define WASP_PKT_HEARTBEAT 0x02
#define WASP_FIRMWARE_VER  8

typedef struct __attribute__((packed)) {
  uint8_t  type;
  uint8_t  workerMac[6];
  uint8_t  nodeType;       // 0x00 = worker, 0x01 = drone
  uint16_t swarmId;        // djb2 hash of swarm name (0 until swarm config added)
  uint8_t  loyaltyLevel;   // 0=wild, 255=fully loyal (drone mechanic)
  uint8_t  gangId;         // WDGWars gang affiliation (0=ungrouped)
  uint8_t  firmwareVer;    // WASP_FIRMWARE_VER
  uint8_t  battLevel;      // 0-100%, 255=unknown
  uint16_t playerLevel;    // rank / promotion mechanic
  uint8_t  boostLevel;     // active boost / buff tier
  uint8_t  reserved[7];    // zero-filled, future game fields
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

// ── Nest AP ───────────────────────────────────────────────────────────────────
#define NEST_AP_SSID      "WASP-Nest"
#define NEST_AP_PASS      "waspswarm"
#define NEST_IP           "192.168.4.1"
#define NEST_UPLOAD_URL   "http://192.168.4.1/upload"  // drone HTTP uploads (small CSV)
#define NEST_UPLOAD_PORT  8080                          // worker raw TCP uploads
#define SYNC_EVERY        25

// ── GPS ───────────────────────────────────────────────────────────────────────
#define GPS_BAUD       9600
#define GPS_RX_PIN     12
#define GPS_TX_PIN     11
#define GPS_DETECT_MS  2000

// ── SD ────────────────────────────────────────────────────────────────────────
#define SD_CS    25
#define SD_SCK    8
#define SD_MISO   9
#define SD_MOSI  10

// ── Scan timing ───────────────────────────────────────────────────────────────
#define GPS_FEED_MS      500
#define WIFI_MS_PER_CHAN  80
#define BLE_SCAN_MS     3000
#define CYCLE_DELAY_MS  2000

// ── Heartbeat ─────────────────────────────────────────────────────────────────
#define HEARTBEAT_INTERVAL_MS 5000

// ── Log file size cap ─────────────────────────────────────────────────────────
// The C5's TCP send path stalls on payloads above ~9.5 KB (observed ceiling
// during live testing). Cap is set well below that threshold so that even a
// file that gets one final row written after the rotation check triggers stays
// safely under the reliable limit. Files already on the SD that exceed this
// cap are renamed .toobig and skipped; they can be recovered via chunked
// upload (Stage 10) or by reading the SD card directly on a PC.
#define MAX_LOG_BYTES 8192

// ── RAM buffer (drone mode) ───────────────────────────────────────────────────
#define CYCLE_SLOTS        25
#define MAX_WIFI_PER_SLOT  40
#define MAX_BLE_PER_SLOT   20

struct wifi_entry_t {
  uint8_t bssid[6];
  char    ssid[33];
  uint8_t auth;
  uint8_t channel;
  int8_t  rssi;
};

struct ble_entry_t {
  uint8_t  addr[6];
  char     name[21];
  int8_t   rssi;
  uint16_t mfgrId;
  bool     hasMfgr;
};

struct WiFiScanResult { int total, g2, g5; int8_t bestRssi; };

struct cycle_slot_t {
  bool         used;
  bool         uploaded;
  uint32_t     capturedMs;
  uint8_t      wifiCount;
  uint8_t      bleCount;
  wifi_entry_t wifi[MAX_WIFI_PER_SLOT];
  ble_entry_t  ble[MAX_BLE_PER_SLOT];
};

// Allocated only in drone mode — ~57 KB in worker mode that would otherwise
// be permanently reserved even though the buffers are never used.
static cycle_slot_t* cycleBuffer     = nullptr;
static uint8_t       writeHead       = 0;

static wifi_entry_t* pendingWifi      = nullptr;
static uint8_t       pendingWifiCount = 0;
static ble_entry_t*  pendingBle       = nullptr;
static uint8_t       pendingBleCount  = 0;

// ── Runtime state ─────────────────────────────────────────────────────────────
static bool     sdOk            = false;
static bool     gpsOk           = false;
static bool     droneMode       = false;
static bool     clockSet        = false;
static File     logFile;
static String   logPath;
static uint32_t linesSinceFlush = 0;
static uint32_t lastFlushMs     = 0;
static uint32_t cycleCount      = 0;
static uint32_t lastHeartbeatMs = 0;

HardwareSerial gpsSerial(1);
TinyGPSPlus    gps;

// ── GPS helpers ───────────────────────────────────────────────────────────────

static void feedGPS(unsigned long ms) {
  unsigned long deadline = millis() + ms;
  while (millis() < deadline) {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    delay(1);
  }
}

static bool detectGPS() {
  Serial.print(" Detecting GPS ");
  unsigned long start = millis();
  while (millis() - start < GPS_DETECT_MS) {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    Serial.print(".");
    delay(100);
  }
  Serial.println();
  return gps.charsProcessed() > 0;
}

static void printGPSStatus() {
  if (!gpsOk) { Serial.println("[WORKER] GPS  not present"); return; }
  if (gps.location.isValid()) {
    Serial.printf("[WORKER] GPS  %.6f, %.6f | alt %.1fm | sats %d | hdop %.2f\n",
                  gps.location.lat(), gps.location.lng(),
                  gps.altitude.isValid()   ? gps.altitude.meters()       : 0.0,
                  gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                  gps.hdop.isValid()       ? gps.hdop.hdop()             : 99.99);
  } else {
    Serial.printf("[WORKER] GPS  NO FIX (sats seen: %d)\n",
                  gps.satellites.isValid() ? (int)gps.satellites.value() : 0);
  }
}

static String gpsTimestamp() {
  if (!gps.date.isValid() || !gps.time.isValid()) return "";
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           gps.date.year(), gps.date.month(), gps.date.day(),
           gps.time.hour(), gps.time.minute(), gps.time.second());
  return String(buf);
}

static String nowTimestamp() {
  time_t now = time(nullptr);
  if (now < 1000000000UL) return "1970-01-01 00:00:00";
  struct tm* t = gmtime(&now);
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);
  return String(buf);
}

static void setClockFromGPS() {
  if (clockSet || !gps.date.isValid() || !gps.time.isValid()) return;
  if (gps.date.year() < 2020) return;  // sanity check
  struct tm t = {};
  t.tm_year = gps.date.year() - 1900;
  t.tm_mon  = gps.date.month() - 1;
  t.tm_mday = gps.date.day();
  t.tm_hour = gps.time.hour();
  t.tm_min  = gps.time.minute();
  t.tm_sec  = gps.time.second();
  time_t epoch = mktime(&t);
  struct timeval tv = { epoch, 0 };
  settimeofday(&tv, nullptr);
  clockSet = true;
  Serial.printf("[GPS] Clock set: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                gps.date.year(), gps.date.month(), gps.date.day(),
                gps.time.hour(), gps.time.minute(), gps.time.second());
}

// ── SD / file helpers (worker mode) ──────────────────────────────────────────

static void flushLog() {
  if (logFile) logFile.flush();
  linesSinceFlush = 0;
  lastFlushMs     = millis();
}

static void maybeFlush() {
  if (++linesSinceFlush >= 25 || (millis() - lastFlushMs) >= 2000) flushLog();
  // Rotate to a new log file once we cross the size cap. The current row has
  // already been written to the open file; the next row will land in the new
  // one. Files stay self-contained CSVs because openLogFile() rewrites the
  // WigleWifi-1.6 header for each new file.
  if (logFile && (int)logFile.size() >= MAX_LOG_BYTES) {
    Serial.printf("[SD] Rotating log: %s reached %u B (cap %d)\n",
                  logPath.c_str(), (unsigned)logFile.size(), MAX_LOG_BYTES);
    logFile.close();
    openLogFile();
  }
}

static String newLogPath() {
  if (!SD.exists("/logs")) SD.mkdir("/logs");
  for (int i = 0; i < 25; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "/logs/WASP_%lu_%08lX.csv",
             (unsigned long)millis(), (unsigned long)esp_random());
    if (!SD.exists(buf)) return String(buf);
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "/logs/WASP_%lu.csv", (unsigned long)millis());
  return String(buf);
}

static bool openLogFile() {
  logPath = newLogPath();
  logFile = SD.open(logPath, FILE_WRITE);
  if (!logFile) { Serial.printf("[SD] Failed to open %s\n", logPath.c_str()); return false; }
  logFile.println("WigleWifi-1.6,appRelease=1,model=WASP-WarDriver_v1,release=1,device=WASP-Worker,display=,board=XIAO-ESP32C5,brand=Seeed,star=Sol,body=3,subBody=0");
  logFile.println("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type");
  logFile.flush();
  Serial.printf("[SD] Log: %s\n", logPath.c_str());
  return true;
}

static int channelToFreq(int ch) {
  if (ch >= 1 && ch <= 13) return 2412 + (ch - 1) * 5;
  if (ch == 14) return 2484;
  if (ch >= 36)  return 5000 + ch * 5;
  return 0;
}

static const char* wigleAuth(wifi_auth_mode_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN:            return "[ESS]";
    case WIFI_AUTH_WEP:             return "[WEP]";
    case WIFI_AUTH_WPA_PSK:         return "[WPA][ESS]";
    case WIFI_AUTH_WPA2_PSK:        return "[WPA2][ESS]";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "[WPA][WPA2][ESS]";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "[WPA2-EAP][ESS]";
    case WIFI_AUTH_WPA3_PSK:        return "[WPA3][ESS]";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "[WPA2][WPA3][ESS]";
    default:                        return "[ESS]";
  }
}

static void logWiFiRow(const String& mac, const String& ssid, wifi_auth_mode_t auth,
                       int channel, int rssi,
                       double lat, double lon, double altM, double accuracy) {
  String ts = gpsTimestamp();
  if (ts.isEmpty()) ts = "1970-01-01 00:00:00";
  String safe = ssid; safe.replace("\"", "\"\"");
  char line[256];
  snprintf(line, sizeof(line), "%s,\"%s\",%s,%s,%d,%d,%d,%.6f,%.6f,%.0f,%.1f,,,WIFI",
           mac.c_str(), safe.c_str(), wigleAuth(auth),
           ts.c_str(), channel, channelToFreq(channel), rssi, lat, lon, altM, accuracy);
  logFile.println(line);
  maybeFlush();
}

static void logBLERow(const String& mac, const String& name, int rssi,
                      double lat, double lon, double altM, double accuracy,
                      bool hasMfgr, uint16_t mfgrId) {
  String ts = gpsTimestamp();
  if (ts.isEmpty()) ts = "1970-01-01 00:00:00";
  String safe = name; safe.replace("\"", "\"\"");
  char mfgrField[8] = "";
  if (hasMfgr) snprintf(mfgrField, sizeof(mfgrField), "%u", mfgrId);
  char line[256];
  snprintf(line, sizeof(line), "%s,\"%s\",[BLE],%s,0,,%d,%.6f,%.6f,%.0f,%.1f,,%s,BLE",
           mac.c_str(), safe.c_str(), ts.c_str(), rssi, lat, lon, altM, accuracy, mfgrField);
  logFile.println(line);
  maybeFlush();
}

// ── RAM buffer helpers (drone mode) ───────────────────────────────────────────

// Used by commitCycle() before its definition below — explicit forward decl
// rather than relying on Arduino IDE's implicit top-level prototype generator.
static int countUnuploaded();

static void clearPending() {
  pendingWifiCount = 0;
  pendingBleCount  = 0;
}

static void commitCycle() {
  cycle_slot_t& slot = cycleBuffer[writeHead];
  slot.used       = true;
  slot.uploaded   = false;
  slot.capturedMs = millis();
  slot.wifiCount  = pendingWifiCount;
  slot.bleCount   = pendingBleCount;
  memcpy(slot.wifi, pendingWifi, pendingWifiCount * sizeof(wifi_entry_t));
  memcpy(slot.ble,  pendingBle,  pendingBleCount  * sizeof(ble_entry_t));

  uint8_t slotIdx = writeHead;
  writeHead = (writeHead + 1) % CYCLE_SLOTS;

  int pending = countUnuploaded();
  Serial.printf("[BUF] Slot %d committed — WiFi:%d BLE:%d  |  %d/%d slots pending\n",
                slotIdx, pendingWifiCount, pendingBleCount, pending, CYCLE_SLOTS);
  clearPending();
}

static int countUnuploaded() {
  if (!cycleBuffer) return 0;  // worker mode never allocates this buffer
  int n = 0;
  for (int i = 0; i < CYCLE_SLOTS; i++)
    if (cycleBuffer[i].used && !cycleBuffer[i].uploaded) n++;
  return n;
}

static String buildCSV(int idx) {
  const cycle_slot_t& s = cycleBuffer[idx];
  String csv;
  csv.reserve(2048);
  csv += "WigleWifi-1.6,appRelease=1,model=WASP-WarDriver_v1,release=1,device=WASP-Drone,display=,board=XIAO-ESP32C5,brand=Seeed,star=Sol,body=3,subBody=0\n";
  csv += "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n";

  for (int i = 0; i < s.wifiCount; i++) {
    const wifi_entry_t& w = s.wifi[i];
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
             w.bssid[0], w.bssid[1], w.bssid[2], w.bssid[3], w.bssid[4], w.bssid[5]);
    String ssid = String(w.ssid);
    ssid.replace("\"", "\"\"");
    char line[200];
    snprintf(line, sizeof(line), "%s,\"%s\",%s,%s,%d,%d,%d,0.000000,0.000000,0,999.9,,,WIFI",
             bssid, ssid.c_str(), wigleAuth((wifi_auth_mode_t)w.auth),
             nowTimestamp().c_str(), w.channel, channelToFreq(w.channel), w.rssi);
    csv += line; csv += '\n';
  }

  for (int i = 0; i < s.bleCount; i++) {
    const ble_entry_t& b = s.ble[i];
    char addr[18];
    snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
             b.addr[0], b.addr[1], b.addr[2], b.addr[3], b.addr[4], b.addr[5]);
    String name = String(b.name);
    name.replace("\"", "\"\"");
    char mfgrField[8] = "";
    if (b.hasMfgr) snprintf(mfgrField, sizeof(mfgrField), "%u", b.mfgrId);
    char line[200];
    snprintf(line, sizeof(line), "%s,\"%s\",[BLE],%s,0,,%d,0.000000,0.000000,0,999.9,,%s,BLE",
             addr, name.c_str(), nowTimestamp().c_str(), b.rssi, mfgrField);
    csv += line; csv += '\n';
  }
  return csv;
}

// ── ESP-NOW ───────────────────────────────────────────────────────────────────

static void sendHeartbeat() {
  heartbeat_t pkt = {};
  pkt.type        = WASP_PKT_HEARTBEAT;
  pkt.nodeType    = droneMode ? 1 : 0;
  pkt.firmwareVer = WASP_FIRMWARE_VER;
  WiFi.macAddress(pkt.workerMac);
  esp_now_send(NEST_MAC, (uint8_t*)&pkt, sizeof(pkt));
  lastHeartbeatMs = millis();
}

static void maybeHeartbeat() {
  if (millis() - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) sendHeartbeat();
}

static void onSendResult(const wifi_tx_info_t*, esp_now_send_status_t status) {
  Serial.printf("[WORKER] ESP-NOW TX %s\n",
                status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAILED");
}

static void initEspNow() {
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) { Serial.println("[WORKER] ESP-NOW init FAILED"); return; }
  esp_now_register_send_cb(onSendResult);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, NEST_MAC, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

static void sendSummary(int wifiTotal, int wifi2g, int wifi5g,
                        int bleCount, int8_t bestRssi) {
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

// ── File sync (worker mode) ───────────────────────────────────────────────────

static bool hasPendingFiles() {
  File dir = SD.open("/logs");
  if (!dir) return false;
  bool found = false;
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;
    bool isDir = entry.isDirectory();
    String name = String(entry.name());
    size_t sz = entry.size();
    entry.close();
    if (!isDir && name.endsWith(".csv") && sz > 0) { found = true; break; }
  }
  dir.close();
  return found;
}

static bool connectToNest() {
  sendHeartbeat();           // last signal before ESP-NOW goes dark
  delay(50);
  esp_now_deinit();
  WiFi.mode(WIFI_STA);
  WiFi.begin(NEST_AP_SSID, NEST_AP_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000) {
    if (gpsOk) while (gpsSerial.available()) gps.encode(gpsSerial.read());
    delay(200); Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

static void disconnectFromNest() {
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  initEspNow();
  delay(50);
  sendHeartbeat();           // announce return immediately after ESP-NOW is back
}

static void syncFiles() {
  Serial.println("\n[SYNC] Starting...");
  if (logFile) { logFile.flush(); logFile.close(); }

  int uploaded = 0, failed = 0;

  // SD+WiFi DMA coexistence on the ESP32-C5 is unreliable mid-transfer:
  // f.read() hangs after the first chunk once the WiFi stack is active.
  // Fix: read the whole file into RAM while WiFi is OFF, then connect and
  // stream from the buffer. OOM files are deferred and retried after 25
  // scan cycles once the heap has settled.
  for (int pass = 0; pass < 200; pass++) {

    // ── Find next pending .csv (WiFi off, SD safe) ──────────────────────────
    String name, path;
    int sz = 0;
    {
      File dir = SD.open("/logs");
      if (!dir) break;
      bool found = false;
      while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        String n = String(entry.name());
        bool   d = entry.isDirectory();
        int    s = (int)entry.size();
        entry.close();
        if (!d && n.endsWith(".csv") && s > 0) { name = n; sz = s; found = true; break; }
      }
      dir.close();
      if (!found) break;
      path = name.startsWith("/") ? name : "/logs/" + name;
    }

    // ── Set aside files that exceed the C5's reliable upload size ───────────
    // These would either OOM on the malloc below or stall partway through TCP
    // send (the 4380-byte symptom). Renaming to .toobig stops the sync loop
    // from tripping over them every cycle; data is preserved on the SD card
    // for offline recovery.
    if (sz > MAX_LOG_BYTES) {
      Serial.printf("[SYNC]  %-32s  %5d B  exceeds cap (%d B) — set aside as .toobig\n",
                    name.c_str(), sz, MAX_LOG_BYTES);
      SD.rename(path.c_str(), (path + ".toobig").c_str());
      continue;
    }

    Serial.printf("[SYNC]  %-32s  %5d B  ...", name.c_str(), sz);

    // ── Pre-buffer entire file from SD while WiFi is off ─────────────────────
    uint8_t* fileBuf = (uint8_t*)malloc(sz);
    if (!fileBuf) {
      Serial.printf("[OOM %dB] — deferred\n", sz);
      SD.rename(path.c_str(), (path + ".defer").c_str());
      failed++;
      continue;
    }
    {
      File f = SD.open(path);
      if (!f) {
        Serial.println("[SD open failed] — deferred");
        free(fileBuf);
        SD.rename(path.c_str(), (path + ".defer").c_str());
        failed++;
        continue;
      }
      int got = f.read(fileBuf, sz);
      f.close();
      if (got != sz) {
        Serial.printf("[SD short read %d/%d] — deferred\n", got, sz);
        free(fileBuf);
        SD.rename(path.c_str(), (path + ".defer").c_str());
        failed++;
        continue;
      }
    }

    // ── Connect and stream from RAM — no SD access during WiFi ───────────────
    bool nestOk  = connectToNest();
    bool ok      = false;
    int  sent    = 0;
    bool tcpFail = false;
    if (nestOk) {
      String myMac = WiFi.macAddress(); myMac.replace(":", "");
      Serial.printf(" [%s]", WiFi.localIP().toString().c_str());
      WiFiClient tcp;
      Serial.print(" tcp..");
      if (tcp.connect(NEST_IP, NEST_UPLOAD_PORT)) {
        tcp.setNoDelay(true);
        tcp.setTimeout(5000);
        Serial.print("OK hdr..");
        tcp.printf("UPLOAD %s %s %d\n", myMac.c_str(), name.c_str(), sz);
        tcp.flush();
        Serial.print("rdy..");
        String ready = tcp.readStringUntil('\n'); ready.trim();
        if (ready == "READY") {
          Serial.print("OK data..");
          const uint8_t* ptr = fileBuf;
          int rem = sz;
          uint32_t wDeadline = millis() + 30000;
          while (rem > 0 && tcp.connected() && millis() < wDeadline) {
            int toSend = min(rem, 1460);  // one TCP segment (MTU) — each write maps to one IP packet
            int w = tcp.write(ptr, toSend);
            if (w > 0) { ptr += w; sent += w; rem -= w; }
            else if (w < 0) break;
            else delay(1);
          }
          tcp.flush();
          Serial.printf("%dB resp..", sent);
          String resp = tcp.readStringUntil('\n'); resp.trim();
          Serial.printf("[%s]", resp.c_str());
          ok = (resp == "OK") && (sent == sz);
        } else {
          Serial.printf("[nest READY? got: %s]", ready.c_str());
        }
      } else {
        Serial.print("FAIL");
        tcpFail = true;
      }
      tcp.stop();
    }
    free(fileBuf);
    disconnectFromNest();

    if (ok) {
      SD.rename(path.c_str(), (path + ".done").c_str());
      Serial.printf(" OK\n"); uploaded++;
    } else if (!nestOk || tcpFail) {
      Serial.println("\n[SYNC] Nest TCP not reachable — stopping");
      break;
    } else {
      Serial.printf(" FAILED (%d/%d B) — deferred\n", sent, sz);
      SD.rename(path.c_str(), (path + ".defer").c_str());
      failed++;
    }
    delay(500);
  }

  // ── Restore deferred files for retry next sync ───────────────────────────
  {
    File dir = SD.open("/logs");
    if (dir) {
      while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        String n = String(entry.name());
        bool   d = entry.isDirectory();
        entry.close();
        if (!d && n.endsWith(".csv.defer")) {
          String op = "/logs/" + n;
          SD.rename(op.c_str(), op.substring(0, op.length() - 6).c_str());
        }
      }
      dir.close();
    }
  }

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  SD.begin(SD_CS, SPI);

  Serial.printf("[SYNC] Done — %d uploaded, %d failed\n", uploaded, failed);
  if (sdOk) openLogFile();
}

// ── Buffer sync (drone mode) ──────────────────────────────────────────────────

static void syncBuffer() {
  int pending = countUnuploaded();
  if (pending == 0) { Serial.println("[SYNC] Buffer empty — nothing to send"); return; }

  Serial.printf("\n[SYNC] Connecting to Nest AP (%d slot(s) pending)...\n", pending);

  if (!connectToNest()) {
    Serial.println("[SYNC] Nest not reachable — buffer retained");
    disconnectFromNest();
    return;
  }

  Serial.printf("[SYNC] Connected  IP: %s\n", WiFi.localIP().toString().c_str());
  String myMac = WiFi.macAddress(); myMac.replace(":", "");
  int uploaded = 0, failed = 0;

  for (int i = 0; i < CYCLE_SLOTS; i++) {
    if (!cycleBuffer[i].used || cycleBuffer[i].uploaded) continue;
    char fileName[40];
    snprintf(fileName, sizeof(fileName), "DRONE_%lu_%02d.csv",
             (unsigned long)cycleBuffer[i].capturedMs, i);

    String csv = buildCSV(i);
    Serial.printf("[SYNC]  slot %2d  %-28s  %5d B  ...", i, fileName, csv.length());

    HTTPClient http;
    http.begin(String(NEST_UPLOAD_URL) + "?worker=" + myMac + "&file=" + fileName);
    http.addHeader("Content-Type", "text/csv");
    int code = http.POST(csv);
    http.end();

    if (code == 200) { cycleBuffer[i].uploaded = true; Serial.println(" OK"); uploaded++; }
    else             { Serial.printf(" FAILED (HTTP %d)\n", code); failed++; }
  }
  Serial.printf("[SYNC] Done — %d uploaded, %d failed\n", uploaded, failed);

  disconnectFromNest();
}

// ── WiFi scan ─────────────────────────────────────────────────────────────────

static const char* authTypeStr(wifi_auth_mode_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENT";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "UNKNOWN";
  }
}

WiFiScanResult runWiFiScan() {
  Serial.println("\n[WORKER] Starting WiFi scan (2.4 GHz + 5 GHz)...");
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);
  int n = WiFi.scanNetworks(false, true, false, WIFI_MS_PER_CHAN);

  WiFiScanResult r = {0, 0, 0, -127};
  if (n == WIFI_SCAN_FAILED || n < 0) { Serial.println("[WORKER] WiFi scan failed"); return r; }
  if (n == 0) { Serial.println("[WORKER] No networks found"); WiFi.scanDelete(); return r; }

  bool   hasFix   = gpsOk && gps.location.isValid();
  double lat      = hasFix ? gps.location.lat()                           : 0.0;
  double lon      = hasFix ? gps.location.lng()                           : 0.0;
  double alt      = (hasFix && gps.altitude.isValid()) ? gps.altitude.meters() : 0.0;
  double accuracy = (hasFix && gps.hdop.isValid())     ? gps.hdop.hdop() * 5.0  : 999.9;

  Serial.println("  #    Band  Ch   RSSI  Security        BSSID              SSID");
  Serial.println("  ---  ----  --   ----  --------------  -----------------  ----");

  for (int i = 0; i < n; i++) {
    int  ch    = WiFi.channel(i);
    bool is_5g = (ch > 14);
    is_5g ? r.g5++ : r.g2++;
    int rssi = WiFi.RSSI(i);
    if (rssi > r.bestRssi) r.bestRssi = (int8_t)rssi;

    String ssid = WiFi.SSID(i);
    Serial.printf("  %3d  %s  %2d  %4d  %-14s  %s  %s\n",
                  i + 1, is_5g ? "5GHz" : "2.4G", ch, rssi,
                  authTypeStr(WiFi.encryptionType(i)),
                  WiFi.BSSIDstr(i).c_str(),
                  ssid.isEmpty() ? "[hidden]" : ssid.c_str());

    if (!droneMode) {
      if (hasFix && sdOk && logFile)
        logWiFiRow(WiFi.BSSIDstr(i), ssid, WiFi.encryptionType(i),
                   ch, rssi, lat, lon, alt, accuracy);
    } else {
      if (pendingWifiCount < MAX_WIFI_PER_SLOT) {
        wifi_entry_t& e = pendingWifi[pendingWifiCount++];
        WiFi.BSSID(i, e.bssid);
        strncpy(e.ssid, ssid.c_str(), 32); e.ssid[32] = '\0';
        e.auth    = (uint8_t)WiFi.encryptionType(i);
        e.channel = (uint8_t)ch;
        e.rssi    = (int8_t)rssi;
      }
    }
  }

  r.total = n;
  Serial.printf("[WORKER] WiFi: %d network(s) — %d x 2.4GHz, %d x 5GHz\n", n, r.g2, r.g5);
  if (!droneMode && !hasFix) Serial.println("         (no GPS fix — not logged to SD)");
  WiFi.scanDelete();
  return r;
}

// ── BLE scan ──────────────────────────────────────────────────────────────────

struct BLEEntry { String addr; String name; int rssi; uint16_t mfgrId; bool hasMfgr; };
static std::vector<BLEEntry> bleResults;

class BLEScanCallbacks : public NimBLEScanCallbacks {
  void onDiscovered(const NimBLEAdvertisedDevice* d) override {
    String   name    = d->getName().empty() ? "" : String(d->getName().c_str());
    uint16_t mfgrId  = 0;
    bool     hasMfgr = false;
    if (d->haveManufacturerData()) {
      std::string mfg = d->getManufacturerData();
      if (mfg.size() >= 2) { mfgrId = (uint8_t)mfg[0] | ((uint8_t)mfg[1] << 8); hasMfgr = true; }
    }
    bleResults.push_back({ String(d->getAddress().toString().c_str()), name, d->getRSSI(), mfgrId, hasMfgr });
    Serial.printf("  RSSI %4d  %-22s  %s\n",
                  d->getRSSI(), d->getAddress().toString().c_str(),
                  name.isEmpty() ? "[unnamed]" : name.c_str());
  }
};

static NimBLEScan* pBLEScan = nullptr;

int runBLEScan() {
  Serial.printf("\n[WORKER] Starting BLE scan (%d ms)...\n", BLE_SCAN_MS);
  Serial.println("  RSSI  Address                 Name");
  Serial.println("  ----  -------                 ----");

  bleResults.clear();
  pBLEScan->clearResults();
  pBLEScan->start(BLE_SCAN_MS, false, false);

  while (pBLEScan->isScanning()) {
    if (gpsOk) while (gpsSerial.available()) gps.encode(gpsSerial.read());
    maybeHeartbeat();
    delay(10);
  }

  if (!droneMode) {
    if (gpsOk && gps.location.isValid() && sdOk && logFile) {
      double lat      = gps.location.lat();
      double lon      = gps.location.lng();
      double alt      = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
      double accuracy = gps.hdop.isValid()     ? gps.hdop.hdop() * 5.0  : 999.9;
      for (auto& e : bleResults)
        logBLERow(e.addr, e.name, e.rssi, lat, lon, alt, accuracy, e.hasMfgr, e.mfgrId);
    }
  } else {
    for (auto& e : bleResults) {
      if (pendingBleCount >= MAX_BLE_PER_SLOT) break;
      ble_entry_t& b = pendingBle[pendingBleCount++];
      sscanf(e.addr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
             &b.addr[0], &b.addr[1], &b.addr[2],
             &b.addr[3], &b.addr[4], &b.addr[5]);
      strncpy(b.name, e.name.c_str(), 20); b.name[20] = '\0';
      b.rssi    = (int8_t)e.rssi;
      b.mfgrId  = e.mfgrId;
      b.hasMfgr = e.hasMfgr;
    }
  }

  Serial.printf("[WORKER] BLE: %d device(s)\n", (int)bleResults.size());
  return (int)bleResults.size();
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n========================================");
  Serial.println(" W.A.S.P. — Stage 9  Unified Firmware");
  Serial.println("========================================");

  gpsSerial.setRxBufferSize(512);
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  Serial.printf(" MAC: %s\n", WiFi.macAddress().c_str());

  initEspNow();
  Serial.println(" ESP-NOW OK");

  NimBLEDevice::init("");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(new BLEScanCallbacks(), false);
  pBLEScan->setActiveScan(true);
  pBLEScan->setDuplicateFilter(true);
  pBLEScan->setMaxResults(0);

  // ── Hardware detection ───────────────────────────────────────────────────────
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, SPI);
  if (sdOk && !SD.exists("/logs")) SD.mkdir("/logs");

  gpsOk = detectGPS();

  droneMode = !sdOk;

  if (droneMode) {
    cycleBuffer = (cycle_slot_t*)calloc(CYCLE_SLOTS, sizeof(cycle_slot_t));
    pendingWifi = (wifi_entry_t*)malloc(MAX_WIFI_PER_SLOT * sizeof(wifi_entry_t));
    pendingBle  = (ble_entry_t*)malloc(MAX_BLE_PER_SLOT  * sizeof(ble_entry_t));
    if (!cycleBuffer || !pendingWifi || !pendingBle) {
      // Fail loud, fail fast: a silent NULL deref 30 s later inside a scan
      // would be harder to diagnose than a clean reboot here.
      Serial.printf("\n[FATAL] Drone-mode buffer alloc failed (free heap=%u). Rebooting in 2s...\n",
                    (unsigned)ESP.getFreeHeap());
      delay(2000);
      ESP.restart();
    }
  }

  // ── Mode banner ──────────────────────────────────────────────────────────────
  Serial.println("\n ┌─────────────────────────────────────┐");
  if (droneMode) {
    Serial.println(" │  Mode  :  DRONE                     │");
    Serial.println(" │  Store :  RAM circular buffer        │");
    Serial.printf(" │  Slots :  %2d x (WiFi:%d + BLE:%d)     │\n",
                  CYCLE_SLOTS, MAX_WIFI_PER_SLOT, MAX_BLE_PER_SLOT);
    Serial.printf(" │  RAM   :  ~%d KB reserved             │\n",
                  (int)(CYCLE_SLOTS * sizeof(cycle_slot_t) / 1024));
  } else {
    Serial.println(" │  Mode  :  WORKER                    │");
    Serial.printf(" │  SD    :  %s                         │\n", sdOk  ? "OK  " : "FAIL");
    Serial.printf(" │  GPS   :  %s                    │\n", gpsOk ? "detected" : "not found");
    if (sdOk) openLogFile();
  }
  Serial.printf(" │  Sync  :  every %d cycles             │\n", SYNC_EVERY);
  Serial.println(" └─────────────────────────────────────┘\n");

  // Upload any files left from the previous session before starting scan cycles.
  // syncFiles() re-opens a fresh log file at the end, so no double-open.
  if (!droneMode && sdOk && hasPendingFiles()) {
    Serial.println(" Pending files found — syncing to Nest before first cycle...");
    syncFiles();
  }

  feedGPS(500);
  Serial.println(" Setup complete\n");
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void loop() {
  Serial.printf("\n======================================== [cycle %u]\n", cycleCount + 1);
  maybeHeartbeat();

  if (gpsOk) { feedGPS(GPS_FEED_MS); setClockFromGPS(); }
  printGPSStatus();

  if (droneMode) clearPending();

  WiFiScanResult wifi = runWiFiScan();
  int            ble  = runBLEScan();

  if (!droneMode && sdOk && logFile) flushLog();
  if (droneMode)                     commitCycle();

  sendSummary(wifi.total, wifi.g2, wifi.g5, ble, wifi.bestRssi);
  cycleCount++;

  if (cycleCount % SYNC_EVERY == 0) {
    if (droneMode) syncBuffer();
    else           syncFiles();
  }

  Serial.println("\n[WORKER] Waiting before next cycle...");
  delay(CYCLE_DELAY_MS);
}
