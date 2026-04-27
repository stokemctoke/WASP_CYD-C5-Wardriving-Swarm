/*
 * NEST - Stage 13: LED event config from wasp.cfg
 * Board: CYD (JC2432W328C) — standard ESP32
 *
 * Extends Stage 11 with:
 *   - Connects to home WiFi (STA) to upload collected CSVs to WiGLE and WDGWars
 *   - Upload triggered automatically every 5 min when home credentials are set
 *   - AP + ESP-NOW temporarily suspended during upload, restored immediately after
 *   - LED: cyan solid = uploading, green 2× = success, red 3× = fail
 *   - Display: "H" indicator (cyan=OK, amber=fail, grey=unconfigured) in status bar
 *   - Footer cycles between last sync and last WiGLE/WDGWars result
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

#include "nest_types.h"
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// ── Nest LED event descriptor ─────────────────────────────────────────────────
// Colour is 24-bit hex; R/G/B channels are binary (non-zero = on) since the
// CYD onboard LED has no PWM. flashes=0 = solid on (for upload-in-progress).
struct LedEvent {
  uint32_t colour;
  int      flashes;
  int      onMs;
  int      offMs;
};

// ── Pins ──────────────────────────────────────────────────────────────────────
#define TFT_BACKLIGHT  21
#define SD_CS           5
#define SD_SCK         18
#define SD_MISO        19
#define SD_MOSI        23

// ── CYD onboard RGB LED (active LOW) ─────────────────────────────────────────
// GPIO 4 is shared with TFT_RST in User_Setup.h. The reset pulse fires once
// inside tft.init(); after that GPIO 4 is safe to drive as a plain output.
#define NEST_LED_R  4
#define NEST_LED_G  16
#define NEST_LED_B  17

// ── Network ───────────────────────────────────────────────────────────────────
#define ESPNOW_CHANNEL          1
#define HOME_UPLOAD_INTERVAL_MS 300000  // attempt home upload every 5 minutes
#define HOME_CONNECT_TIMEOUT_MS  15000  // give up connecting to home WiFi after 15 s

// ── Display layout (240 × 320 portrait) ──────────────────────────────────────
// HEADER(28) + STATUS(16) + 5 × ROW(52) + FOOTER(16) = 320
#define HEADER_H  28
#define STATUS_H  16
#define ROW_H     52
#define MAX_ROWS   5
#define FOOTER_H  16

// ── Colors (RGB565) — stokemctoke.com palette ─────────────────────────────────
#define CLR_BG        0x0002    // #000111 near-black
#define CLR_HDR_BG    0xFD00    // #FAA307 amber
#define CLR_HDR_FG    0xF7BF    // #F4F6F8 near-white
#define CLR_LABEL     0x9D15    // #9CA3AF slate grey
#define CLR_ACTIVE    0xFFE0    // #FFFF00 yellow  — active worker
#define CLR_STALE     0xFD00    // #FAA307 amber   — stale worker
#define CLR_OFFLINE   0x2945    // #2B2B2B dark grey
#define CLR_GPS_OK    0x07FF    // #00FFFF cyan
#define CLR_GPS_NO    0x9D15    // #9CA3AF grey    — GPS absent
#define CLR_DIVIDER   0x2945    // #2B2B2B dark grey
#define CLR_FTR_BG    0x2945    // #2B2B2B dark grey

// ── Packet types ─────────────────────────────────────────────────────────────
#define WASP_PKT_SUMMARY   0x01
#define WASP_PKT_HEARTBEAT 0x02
#define WASP_FIRMWARE_VER  10

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

// ── Nest LED event defaults (overridden by wasp.cfg at boot) ─────────────────
static LedEvent evNestBoot       = { 0xFFFFFF, 3,  50,  50 }; // white   — startup / config loaded
static LedEvent evNestHeartbeat  = { 0xFF69B4, 2,  80,  80 }; // pink    — heartbeat / idle
static LedEvent evNestChunk      = { 0x0050FF, 1,  80,   0 }; // blue    — receiving chunk from worker
static LedEvent evNestUploadAct  = { 0xFF00B4, 0,   0,   0 }; // magenta — solid while uploading
static LedEvent evNestUploadOK   = { 0x64FF00, 2, 200, 200 }; // lime    — upload success
static LedEvent evNestUploadFail = { 0xFF0000, 3, 200, 200 }; // red     — upload failed

static bool loadConfig() {
  // Defaults — overridden by wasp.cfg if present
  strlcpy(cfg.apSsid, "WASP-Nest", sizeof(cfg.apSsid));
  strlcpy(cfg.apPsk,  "waspswarm", sizeof(cfg.apPsk));

  // If /reset.cfg exists, skip wasp.cfg entirely and boot on compiled defaults.
  if (SD.exists("/reset.cfg")) {
    Serial.println("[CFG] /reset.cfg found — using compiled defaults, wasp.cfg ignored");
    return false;
  }

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
    else if (key == "nestLedBoot")        parseNestLedEvent(val, evNestBoot);
    else if (key == "nestLedHeartbeat")   parseNestLedEvent(val, evNestHeartbeat);
    else if (key == "nestLedChunk")       parseNestLedEvent(val, evNestChunk);
    else if (key == "nestLedUploadAct")   parseNestLedEvent(val, evNestUploadAct);
    else if (key == "nestLedUploadOK")    parseNestLedEvent(val, evNestUploadOK);
    else if (key == "nestLedUploadFail")  parseNestLedEvent(val, evNestUploadFail);
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

static worker_entry_t workers[MAX_WORKERS] = {};

static bool sdOk = false;
static char lastSyncStr[48] = "none";

static portMUX_TYPE gLock = portMUX_INITIALIZER_UNLOCKED;

// Flags set by callbacks (WiFi task / upload task) and consumed by loop()
// to avoid delay() inside callback context.
static volatile bool ledHeartbeatFlag = false;
static volatile bool ledSyncFlag      = false;

// ── Home upload state ─────────────────────────────────────────────────────────
static char     lastWigleStr[32]     = "never";
static char     lastWdgStr[32]       = "never";
static uint32_t lastUploadAttemptMs  = 0;
static bool     uploadRunning        = false;  // guard against re-entry from loop()
static uint8_t  homeStatus           = 0;      // 0=unconfigured 1=last OK 2=last fail

// ── CYD RGB LED helpers ───────────────────────────────────────────────────────

static void nestLedOff() {
  digitalWrite(NEST_LED_R, HIGH);
  digitalWrite(NEST_LED_G, HIGH);
  digitalWrite(NEST_LED_B, HIGH);
}

static void nestLedSet(bool r, bool g, bool b) {
  digitalWrite(NEST_LED_R, r ? LOW : HIGH);
  digitalWrite(NEST_LED_G, g ? LOW : HIGH);
  digitalWrite(NEST_LED_B, b ? LOW : HIGH);
}

static void nestLedFlash(bool r, bool g, bool b, int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    nestLedSet(r, g, b);
    delay(onMs);
    nestLedOff();
    if (i < times - 1) delay(offMs);
  }
}

static void nestLedFlashEvent(const LedEvent& ev) {
  bool r = ((ev.colour >> 16) & 0xFF) > 0;
  bool g = ((ev.colour >>  8) & 0xFF) > 0;
  bool b = ( ev.colour        & 0xFF) > 0;
  if (ev.flashes == 0) { nestLedSet(r, g, b); return; }  // solid
  nestLedFlash(r, g, b, ev.flashes, ev.onMs, ev.offMs);
}

static bool parseNestLedEvent(const String& val, LedEvent& ev) {
  int c1 = val.indexOf(',');
  int c2 = val.indexOf(',', c1 + 1);
  int c3 = val.indexOf(',', c2 + 1);
  if (c1 < 0 || c2 < 0 || c3 < 0) return false;
  ev.colour  = (uint32_t)strtoul(val.substring(0, c1).c_str(), nullptr, 16);
  ev.flashes = val.substring(c1 + 1, c2).toInt();
  ev.onMs    = val.substring(c2 + 1, c3).toInt();
  ev.offMs   = val.substring(c3 + 1).toInt();
  return true;
}

TFT_eSPI  tft = TFT_eSPI();
SPIClass  sdSpi(VSPI);
WebServer   server(80);
WiFiServer  rawServer(8080);

// ── Worker registry helpers ───────────────────────────────────────────────────

static int findWorker(const uint8_t* mac) {
  for (int i = 0; i < MAX_WORKERS; i++)
    if (memcmp(workers[i].mac, mac, 6) == 0) return i;
  return -1;
}

static int findOrAddWorker(const uint8_t* mac) {
  int idx = findWorker(mac);
  if (idx >= 0) return idx;
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

// ── Home WiFi upload ──────────────────────────────────────────────────────────

static bool hasFilesToUpload() {
  File root = SD.open("/logs");
  if (!root) return false;
  bool found = false;
  while (!found) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) { entry.close(); continue; }
    String sub = "/logs/" + String(entry.name());
    entry.close();
    File dir = SD.open(sub.c_str());
    if (!dir) continue;
    while (true) {
      File f = dir.openNextFile();
      if (!f) break;
      String n = String(f.name());
      bool ok = n.endsWith(".csv") && !f.isDirectory() && f.size() > 0;
      f.close();
      if (ok) { found = true; break; }
    }
    dir.close();
  }
  root.close();
  return found;
}

static void restoreNestAP() {
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  WiFi.softAP(cfg.apSsid, cfg.apPsk, ESPNOW_CHANNEL);
  delay(100);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() == ESP_OK) esp_now_register_recv_cb(onDataRecv);
  delay(50);
  Serial.println("[HOME] AP + ESP-NOW restored");
}

// Streams a file from SD as a multipart/form-data POST over HTTPS — no heap alloc for
// the file content. Uses a 512-byte stack buffer; safe for files of any size.
// The standard ESP32 (CYD, Xtensa) has no SD/WiFi DMA coexistence issue, so reading
// from SD while the WiFi socket is open is safe here (unlike the ESP32-C5 worker).
// Returns the HTTP status code, or -1 on connection/socket failure.
static int streamMultipartPost(const char* host, const char* urlPath,
                                const char* authHeader, const char* authValue,
                                const String& filePath, const String& fileName) {
  File f = SD.open(filePath.c_str());
  if (!f) return -1;
  int sz = (int)f.size();
  if (sz <= 0) { f.close(); return -1; }
  f.close();

  const char* boundary = "WASPupload0123456789";
  String pre  = String("--") + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"file\"; filename=\"" + fileName + "\"\r\n"
                "Content-Type: text/csv\r\n\r\n";
  String post = String("\r\n--") + boundary + "--\r\n";
  int total   = (int)pre.length() + sz + (int)post.length();

  WiFiClientSecure wc; wc.setInsecure();
  wc.setTimeout(60);  // 60-second socket timeout for large files
  if (!wc.connect(host, 443)) {
    Serial.printf("[UPLOAD] TCP connect to %s failed\n", host);
    return -1;
  }

  // HTTP request headers
  wc.printf("POST %s HTTP/1.1\r\n", urlPath);
  wc.printf("Host: %s\r\n", host);
  wc.printf("%s: %s\r\n", authHeader, authValue);
  wc.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
  wc.printf("Content-Length: %d\r\n", total);
  wc.printf("Accept: application/json\r\n");
  wc.printf("Connection: close\r\n\r\n");

  // Multipart header
  wc.print(pre);

  // Stream file body from SD in 512-byte chunks — no large malloc needed
  File sf = SD.open(filePath.c_str());
  if (!sf) { wc.stop(); return -1; }
  uint8_t buf[512];
  size_t  n;
  uint32_t deadline = millis() + 120000UL;  // 2-minute body send deadline
  while ((n = sf.read(buf, sizeof(buf))) > 0 && millis() < deadline) {
    if (wc.write(buf, n) == 0) break;
  }
  sf.close();

  // Multipart footer
  wc.print(post);

  // Read HTTP status line — e.g. "HTTP/1.1 200 OK"
  String statusLine = wc.readStringUntil('\n');
  statusLine.trim();
  wc.stop();

  int code = 0;
  if (statusLine.startsWith("HTTP/")) {
    int sp = statusLine.indexOf(' ');
    if (sp > 0) code = statusLine.substring(sp + 1, sp + 4).toInt();
  }
  return code;
}

static bool uploadFileToWigle(const String& path, const String& fileName) {
  if (!cfg.wigleBasicToken[0]) return false;
  Serial.printf("[WIGLE] Uploading %s...\n", fileName.c_str());
  String auth = String("Basic ") + cfg.wigleBasicToken;
  int code = streamMultipartPost("api.wigle.net", "/api/v2/file/upload",
                                  "Authorization", auth.c_str(), path, fileName);
  Serial.printf("[WIGLE] %s  HTTP %d\n", fileName.c_str(), code);
  return (code == 200);
}

// WDGWars upload — POST multipart/form-data to /api/upload-csv, WigleWifi-1.6 CSV format.
// Header: X-API-Key (64-char hex). Field: "file". Confirmed from wdgwars.pl/help/#api-docs.
static bool uploadFileToWdgwars(const String& path, const String& fileName) {
  if (!cfg.wdgwarsApiKey[0]) return false;
  Serial.printf("[WDG] Uploading %s...\n", fileName.c_str());
  int code = streamMultipartPost("wdgwars.pl", "/api/upload-csv",
                                  "X-API-Key", cfg.wdgwarsApiKey, path, fileName);
  Serial.printf("[WDG] %s  HTTP %d\n", fileName.c_str(), code);
  return (code == 200);
}

// Merges all pending /logs/MACADDR/*.csv into a single WigleWifi-1.6 file at outPath.
// First file's two header lines are written once; subsequent files skip them.
// Built on SD before WiFi starts to avoid SD/WiFi DMA coexistence issues.
// Returns the number of source files merged (0 = nothing to do or SD error).
static int buildMergedCsv(const String& outPath) {
  if (SD.exists(outPath.c_str())) SD.remove(outPath.c_str());
  File out = SD.open(outPath.c_str(), FILE_WRITE);
  if (!out) return 0;

  bool headerWritten = false;
  int  count = 0;
  uint8_t buf[512];

  File root = SD.open("/logs");
  if (!root) { out.close(); return 0; }
  while (true) {
    File macDir = root.openNextFile();
    if (!macDir) break;
    if (!macDir.isDirectory()) { macDir.close(); continue; }
    String macName = "/logs/" + String(macDir.name());
    macDir.close();
    File sub = SD.open(macName.c_str());
    if (!sub) continue;
    while (true) {
      File cf = sub.openNextFile();
      if (!cf) break;
      String name = String(cf.name());
      bool isCsv = name.endsWith(".csv") && !cf.isDirectory() && cf.size() > 0;
      cf.close();
      if (!isCsv) continue;
      File src = SD.open((macName + "/" + name).c_str());
      if (!src) continue;
      if (!headerWritten) {
        size_t n;
        while ((n = src.read(buf, sizeof(buf))) > 0) out.write(buf, n);
        headerWritten = true;
      } else {
        src.readStringUntil('\n');  // skip WigleWifi-1.6 app line
        src.readStringUntil('\n');  // skip MAC,SSID,... column header
        size_t n;
        while ((n = src.read(buf, sizeof(buf))) > 0) out.write(buf, n);
      }
      src.close();
      count++;
    }
    sub.close();
  }
  root.close();
  out.close();
  return count;
}

// Renames every /logs/MACADDR/*.csv to .done after a successful upload batch.
static void markAllCsvsDone() {
  File root = SD.open("/logs");
  if (!root) return;
  while (true) {
    File macDir = root.openNextFile();
    if (!macDir) break;
    if (!macDir.isDirectory()) { macDir.close(); continue; }
    String macName = "/logs/" + String(macDir.name());
    macDir.close();
    File sub = SD.open(macName.c_str());
    if (!sub) continue;
    while (true) {
      File cf = sub.openNextFile();
      if (!cf) break;
      String name = String(cf.name());
      bool isCsv = name.endsWith(".csv") && !cf.isDirectory();
      cf.close();
      if (!isCsv) continue;
      String fp = macName + "/" + name;
      SD.rename(fp.c_str(), (fp + ".done").c_str());
    }
    sub.close();
  }
  root.close();
}

// Connects to home WiFi, merges all pending CSVs into one WigleWifi-1.6 file,
// uploads it once to WiGLE and WDGWars, marks sources done, restores AP + ESP-NOW.
static void runHomeUploads() {
  if (uploadRunning || !sdOk) return;
  if (!cfg.homeSsid[0]) return;
  if (!cfg.wigleBasicToken[0] && !cfg.wdgwarsApiKey[0]) return;
  if (!hasFilesToUpload()) { Serial.println("[HOME] No files to upload"); return; }

  uploadRunning = true;
  nestLedFlashEvent(evNestUploadAct);

  // Build merged CSV on SD before bringing up WiFi (avoids SD/WiFi DMA conflict)
  const String mergePath = "/merge_tmp.csv";
  int mergedCount = buildMergedCsv(mergePath);
  if (mergedCount == 0) {
    Serial.println("[HOME] Merge produced no files — aborting");
    nestLedOff(); nestLedFlashEvent(evNestUploadFail);
    uploadRunning = false;
    return;
  }
  { File mf = SD.open(mergePath.c_str());
    Serial.printf("[HOME] Merged %d file(s) → %s (%d B)\n",
                  mergedCount, mergePath.c_str(), mf ? (int)mf.size() : 0);
    if (mf) mf.close(); }

  Serial.printf("[HOME] Connecting to %s ...", cfg.homeSsid);
  esp_now_deinit();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.homeSsid, cfg.homePsk);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < HOME_CONNECT_TIMEOUT_MS) {
    delay(200); Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HOME] Connect failed — restoring AP");
    nestLedOff(); nestLedFlashEvent(evNestUploadFail);
    homeStatus = 2;
    SD.remove(mergePath.c_str());
    WiFi.disconnect(true); delay(200);
    restoreNestAP();
    uploadRunning = false;
    return;
  }
  Serial.printf("[HOME] Connected  IP: %s\n", WiFi.localIP().toString().c_str());

  bool wOk = cfg.wigleBasicToken[0] ? uploadFileToWigle(mergePath,   "WASP_merged.csv") : true;
  bool dOk = cfg.wdgwarsApiKey[0]   ? uploadFileToWdgwars(mergePath, "WASP_merged.csv") : true;

  SD.remove(mergePath.c_str());

  taskENTER_CRITICAL(&gLock);
  if (cfg.wigleBasicToken[0])
    snprintf(lastWigleStr, sizeof(lastWigleStr), wOk ? "%d files OK" : "FAIL", mergedCount);
  if (cfg.wdgwarsApiKey[0])
    snprintf(lastWdgStr,   sizeof(lastWdgStr),   dOk ? "%d files OK" : "FAIL", mergedCount);
  taskEXIT_CRITICAL(&gLock);

  Serial.printf("[HOME] Done — WiGLE %s  WDGWars %s\n",
                wOk ? "OK" : "FAIL", dOk ? "OK" : "FAIL");

  nestLedOff();
  if (wOk && dOk) {
    markAllCsvsDone();
    nestLedFlashEvent(evNestUploadOK);
    homeStatus = 1;
  } else {
    nestLedFlashEvent(evNestUploadFail);
    homeStatus = 2;
  }

  WiFi.disconnect(true); delay(200);
  restoreNestAP();
  uploadRunning = false;
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
    // Refresh registry timeout — worker is alive, just syncing over WiFi
    taskENTER_CRITICAL(&gLock);
    int idx = findWorker(info.wifi_ap_staconnected.mac);
    if (idx >= 0) workers[idx].lastSeenMs = millis();
    taskEXIT_CRITICAL(&gLock);
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
    ledHeartbeatFlag = true;  // loop() will flash green — safe from callback context
    const heartbeat_t* pkt = (const heartbeat_t*)data;
    uint8_t nodeType = (len >= 8)                    ? pkt->nodeType : 0;
    // extended fields present only in stage9+ firmware
    bool ext         = (len >= (int)sizeof(heartbeat_t));

    taskENTER_CRITICAL(&gLock);
    int idx = findOrAddWorker(pkt->workerMac);
    uint32_t ago = 0;
    if (idx >= 0) {
      ago = (workers[idx].lastSeenMs > 0) ? (millis() - workers[idx].lastSeenMs) / 1000 : 0;
      workers[idx].lastSeenMs = millis();
      workers[idx].rssi       = (int8_t)rssi;
      workers[idx].nodeType   = nodeType;
    }
    int inRange = countActiveWorkers();
    taskEXIT_CRITICAL(&gLock);
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
  taskENTER_CRITICAL(&gLock);
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
  taskEXIT_CRITICAL(&gLock);

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

// ── Upload input validation ──────────────────────────────────────────────────
// All upload paths take a worker MAC and a filename from the network and use
// them directly in SD path construction. Both must be allowlisted before use,
// or a malicious / buggy worker can write outside /logs/ via path traversal.

static bool isValidMac(const String& mac) {
  if (mac.length() != 12) return false;
  for (unsigned int i = 0; i < mac.length(); i++) {
    char c = mac[i];
    if (!((c >= '0' && c <= '9') ||
          (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

static bool isValidFilename(const String& name) {
  if (name.isEmpty() || name.length() > 64) return false;
  if (!name.endsWith(".csv")) return false;
  if (name.indexOf("..") >= 0) return false;     // explicit traversal guard
  for (unsigned int i = 0; i < name.length(); i++) {
    char c = name[i];
    if (!isAlphaNumeric(c) && c != '_' && c != '-' && c != '.') return false;
  }
  return true;
}

// ── Raw TCP upload handler (port 8080) ────────────────────────────────────────
// Header: "UPLOAD <MAC_NO_COLONS> <FILENAME> <SIZE_BYTES>\n"
// Response: "OK\n" or "ERR <reason>\n"

static void handleRawUpload() {
  WiFiClient client = rawServer.accept();
  if (!client) return;

  client.setTimeout(15000);
  String hdr = client.readStringUntil('\n');
  hdr.trim();

  // Detect header type: UPLOAD_CHUNK or UPLOAD
  bool isChunked = hdr.startsWith("UPLOAD_CHUNK ");
  bool isSingle  = !isChunked && hdr.startsWith("UPLOAD ");
  if (!isSingle && !isChunked) {
    client.println("ERR bad header");
    client.stop();
    return;
  }

  String workerMac, fileName;
  int fileSize = 0, chunkIndex = 0, totalChunks = 1;

  if (isSingle) {
    int s1 = hdr.indexOf(' ');
    int s2 = hdr.indexOf(' ', s1 + 1);
    int s3 = hdr.indexOf(' ', s2 + 1);
    if (s3 < 0) { client.println("ERR bad header"); client.stop(); return; }
    workerMac = hdr.substring(s1 + 1, s2);
    fileName  = hdr.substring(s2 + 1, s3);
    fileSize  = hdr.substring(s3 + 1).toInt();
  } else {
    // UPLOAD_CHUNK <MAC> <FILE> <CHUNK_INDEX> <TOTAL_CHUNKS> <CHUNK_SIZE>
    int s1 = hdr.indexOf(' ');
    int s2 = hdr.indexOf(' ', s1 + 1);
    int s3 = hdr.indexOf(' ', s2 + 1);
    int s4 = hdr.indexOf(' ', s3 + 1);
    int s5 = hdr.indexOf(' ', s4 + 1);
    if (s5 < 0) { client.println("ERR bad header"); client.stop(); return; }
    workerMac   = hdr.substring(s1 + 1, s2);
    fileName    = hdr.substring(s2 + 1, s3);
    chunkIndex  = hdr.substring(s3 + 1, s4).toInt();
    totalChunks = hdr.substring(s4 + 1, s5).toInt();
    fileSize    = hdr.substring(s5 + 1).toInt();
  }

  if (!sdOk || fileSize <= 0) {
    client.println("ERR bad params");
    client.stop();
    return;
  }
  if (!isValidMac(workerMac) || !isValidFilename(fileName)) {
    Serial.printf("[NEST] Rejected upload: mac='%s' file='%s' (validation failed)\n",
                  workerMac.c_str(), fileName.c_str());
    client.println("ERR bad params");
    client.stop();
    return;
  }

  String dir  = "/logs/" + workerMac;
  String path = dir + "/" + fileName;
  if (!SD.exists("/logs")) SD.mkdir("/logs");
  if (!SD.exists(dir))     SD.mkdir(dir);

  // Chunk 0 (or a single-shot upload) starts fresh — truncate any prior partial.
  // Later chunks append to the file assembled so far.
  File f;
  if (chunkIndex == 0) {
    if (SD.exists(path.c_str())) SD.remove(path.c_str());
    f = SD.open(path.c_str(), FILE_WRITE);
  } else {
    f = SD.open(path.c_str(), FILE_APPEND);
    if (!f) f = SD.open(path.c_str(), FILE_WRITE);  // create if missing
  }
  if (!f) { client.println("ERR sd open failed"); client.stop(); return; }

  client.println("READY");  // tell worker we're open and ready for data

  uint8_t  buf[4096];
  int      remaining = fileSize;
  size_t   written   = 0;
  uint32_t deadline  = millis() + 30000;
  bool     sdFail    = false;
  while (remaining > 0 && client.connected() && millis() < deadline) {
    int n = client.read(buf, min(remaining, (int)sizeof(buf)));
    if (n > 0) {
      size_t wrote = f.write(buf, n);
      written += wrote;
      if ((int)wrote < n) {
        // SD full / removed / FAT corruption — without this check we'd happily
        // drain the rest of the TCP stream into the void and tell the worker OK.
        Serial.printf("[NEST] SD write short %u/%d — aborting (card full or removed?)\n",
                      (unsigned)wrote, n);
        sdFail = true;
        break;
      }
      remaining -= n;
    } else {
      yield();  // no data yet — give WiFi stack CPU time so ACKs go out promptly
    }
  }
  f.close();

  if (sdFail) {
    SD.remove(path.c_str());
    client.println("ERR sd write failed");
    client.stop();
    return;
  }

  Serial.printf("[NEST] recv %u/%d B for %s\n", (unsigned)written, fileSize, fileName.c_str());

  if ((int)written < fileSize) {
    client.println("ERR transfer incomplete");
    client.stop();
    SD.remove(path.c_str());
    return;
  }

  client.println("OK");
  client.stop();

  // Flash per chunk received — safe to delay() here, we're in uploadTask on Core 0
  nestLedFlashEvent(evNestChunk);

  Serial.printf("[NEST] Saved %s chunk %d/%d (%u bytes)\n",
                fileName.c_str(), chunkIndex + 1, totalChunks, (unsigned)written);
  if (chunkIndex == totalChunks - 1) {
    taskENTER_CRITICAL(&gLock);
    snprintf(lastSyncStr, sizeof(lastSyncStr), "%s  %dB", workerMac.c_str(), (int)written);
    taskEXIT_CRITICAL(&gLock);
    Serial.printf("[NEST] Complete: %s\n", path.c_str());
  }
}

// ── HTTP upload handler (drone small CSV uploads) ─────────────────────────────

static void handleUpload() {
  if (!sdOk) { server.send(503, "text/plain", "SD not ready"); return; }

  String workerMac = server.arg("worker");
  String fileName  = server.arg("file");

  if (!isValidMac(workerMac))      { server.send(400, "text/plain", "Bad worker");   return; }
  if (!isValidFilename(fileName))  { server.send(400, "text/plain", "Bad filename"); return; }

  String dir  = "/logs/" + workerMac;
  String path = dir + "/" + fileName;

  if (!SD.exists("/logs")) SD.mkdir("/logs");
  if (!SD.exists(dir))     SD.mkdir(dir);

  String body = server.arg("plain");
  // Truncate prior partial upload — FILE_WRITE is append mode, which would
  // corrupt the file by concatenating onto a previous failed transfer.
  if (SD.exists(path.c_str())) SD.remove(path.c_str());
  File f = SD.open(path.c_str(), FILE_WRITE);
  if (!f) { server.send(500, "text/plain", "SD open failed"); return; }
  size_t wrote = f.print(body);
  f.close();
  if (wrote < body.length()) {
    SD.remove(path.c_str());
    Serial.printf("[NEST] SD write short %u/%u for %s\n",
                  (unsigned)wrote, (unsigned)body.length(), path.c_str());
    server.send(500, "text/plain", "SD write failed");
    return;
  }

  taskENTER_CRITICAL(&gLock);
  snprintf(lastSyncStr, sizeof(lastSyncStr), "%s  %dB", workerMac.c_str(), body.length());
  taskEXIT_CRITICAL(&gLock);
  Serial.printf("[NEST] Saved %s (%d bytes)\n", path.c_str(), body.length());
  server.send(200, "text/plain", "OK");
}

// ── Display helpers ───────────────────────────────────────────────────────────

static uint16_t rssiColor(int8_t r) {
  if (r > -50) return 0xFFE0;    // #FFFF00 yellow  — strong
  if (r > -70) return 0xFD00;    // #FAA307 amber   — ok
  if (r > -85) return 0x9D15;    // #9CA3AF grey    — weak
  return TFT_RED;                  // red             — poor
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
  // Snapshot shared state under spinlock — draw takes ~50 ms, far too long to hold
  worker_entry_t snap[MAX_WORKERS];
  char syncSnap[sizeof(lastSyncStr)];
  char wigleSnap[sizeof(lastWigleStr)];
  char wdgSnap[sizeof(lastWdgStr)];
  taskENTER_CRITICAL(&gLock);
  memcpy(snap,      workers,       sizeof(snap));
  memcpy(syncSnap,  lastSyncStr,   sizeof(syncSnap));
  memcpy(wigleSnap, lastWigleStr,  sizeof(wigleSnap));
  memcpy(wdgSnap,   lastWdgStr,    sizeof(wdgSnap));
  taskEXIT_CRITICAL(&gLock);

  uint32_t now = millis();
  int active = 0;
  for (int i = 0; i < MAX_WORKERS; i++)
    if (snap[i].lastSeenMs > 0 && (now - snap[i].lastSeenMs) < WORKER_TIMEOUT_MS) active++;

  tft.fillRect(0, HEADER_H, 240, STATUS_H, CLR_BG);
  tft.setTextFont(2);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(CLR_LABEL, CLR_BG);
  char buf[32];
  snprintf(buf, sizeof(buf), "Workers in range: %d", active);
  tft.drawString(buf, 6, HEADER_H + STATUS_H / 2);

  // Home WiFi "H" indicator — cyan=OK, amber=fail, grey=unconfigured
  if (cfg.homeSsid[0]) {
    uint16_t hCol = (homeStatus == 1) ? CLR_GPS_OK : (homeStatus == 2) ? CLR_STALE : CLR_OFFLINE;
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(hCol, CLR_BG);
    tft.drawString("H", 234, HEADER_H + STATUS_H / 2);
  }

  int row = 0;
  for (int i = 0; i < MAX_WORKERS && row < MAX_ROWS; i++) {
    if (snap[i].lastSeenMs > 0 && (now - snap[i].lastSeenMs) < WORKER_DISPLAY_MS)
      drawWorkerRow(row++, snap[i]);
  }
  for (; row < MAX_ROWS; row++) {
    int y = HEADER_H + STATUS_H + row * ROW_H;
    tft.fillRect(0, y, 240, ROW_H, CLR_BG);
  }

  int fy = 320 - FOOTER_H;
  tft.fillRect(0, fy, 240, FOOTER_H, CLR_FTR_BG);
  tft.setTextFont(1);
  tft.setTextDatum(ML_DATUM);
  char fbuf[52];
  // Cycle footer: 0-1=sync, 2=WiGLE, 3=WDGWars (each shown for 1 s)
  static uint8_t footerTick = 0;
  footerTick = (footerTick + 1) % 4;
  if (footerTick < 2) {
    tft.setTextColor(CLR_LABEL, CLR_FTR_BG);
    snprintf(fbuf, sizeof(fbuf), "Sync: %s", syncSnap);
  } else if (footerTick == 2) {
    tft.setTextColor(CLR_GPS_OK, CLR_FTR_BG);
    snprintf(fbuf, sizeof(fbuf), "WiGLE: %s", wigleSnap);
  } else {
    tft.setTextColor(CLR_STALE, CLR_FTR_BG);
    snprintf(fbuf, sizeof(fbuf), "WDG: %s", wdgSnap);
  }
  tft.drawString(fbuf, 6, fy + FOOTER_H / 2);
}

// ── Registry cleanup ──────────────────────────────────────────────────────────

static void cleanRegistry() {
  uint32_t now = millis();
  for (int i = 0; i < MAX_WORKERS; i++) {
    taskENTER_CRITICAL(&gLock);
    bool expired = workers[i].lastSeenMs > 0 &&
                   (now - workers[i].lastSeenMs) >= WORKER_REMOVE_MS;
    uint8_t macB[6];
    if (expired) { memcpy(macB, workers[i].mac, 6); memset(&workers[i], 0, sizeof(workers[i])); }
    taskEXIT_CRITICAL(&gLock);
    if (!expired) continue;
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             macB[0], macB[1], macB[2], macB[3], macB[4], macB[5]);
    Serial.printf("[NEST] Worker %s expired — slot freed\n", mac);
  }
}

// ── Upload task (Core 0) ──────────────────────────────────────────────────────
// Runs alongside the WiFi stack so TCP/SD work doesn't block the display tick.

static void uploadTask(void*) {
  for (;;) {
    server.handleClient();
    handleRawUpload();
    vTaskDelay(1);
  }
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n========================================");
  Serial.println(" W.A.S.P. Nest — Stage 13");
  Serial.println(" ESP-NOW + WiFi AP + Chunked Upload + Home Upload");
  Serial.println("========================================");

  pinMode(TFT_BACKLIGHT, OUTPUT);
  digitalWrite(TFT_BACKLIGHT, HIGH);

  tft.init();
  // GPIO 4 (NEST_LED_R) is now free — TFT_RST pulse completed inside tft.init()
  pinMode(NEST_LED_R, OUTPUT);
  pinMode(NEST_LED_G, OUTPUT);
  pinMode(NEST_LED_B, OUTPUT);
  nestLedOff();
  nestLedFlashEvent(evNestBoot);

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
  if (cfg.homeSsid[0]) {
    homeStatus = 0;  // configured but not yet attempted
    Serial.printf(" Home WiFi  : %s (credentials set)\n", cfg.homeSsid);
    Serial.printf(" WiGLE      : %s\n", cfg.wigleBasicToken[0] ? "token set" : "not set");
    Serial.printf(" WDGWars    : %s\n", cfg.wdgwarsApiKey[0]   ? "key set"   : "not set");
  } else {
    Serial.println(" Home WiFi  : not configured (add homeSsid/homePsk to wasp.cfg)");
  }

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

  xTaskCreatePinnedToCore(uploadTask, "upload", 8192, NULL, 5, NULL, 0);

  tft.fillRect(0, HEADER_H, 240, 320 - HEADER_H, CLR_BG);
  refreshDisplay();
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void loop() {
  // upload task runs on Core 0 — loop() handles display, housekeeping, and LED flags
  static uint32_t lastRefresh = 0;
  static uint32_t lastClean   = 0;
  uint32_t now = millis();

  // Consume LED flags set by callbacks — flash immediately, no stall in callback context
  if (ledHeartbeatFlag) { ledHeartbeatFlag = false; nestLedFlashEvent(evNestHeartbeat); }
  if (ledSyncFlag)      { ledSyncFlag      = false; nestLedFlashEvent(evNestChunk); }

  // Home upload — attempt every 5 min if home WiFi + credentials configured
  if (now - lastUploadAttemptMs >= HOME_UPLOAD_INTERVAL_MS) {
    lastUploadAttemptMs = now;
    runHomeUploads();  // blocks during connect + upload; AP + ESP-NOW restored before return
  }

  if (now - lastRefresh >= 1000) {
    refreshDisplay();
    lastRefresh = now;
  }
  if (now - lastClean >= 30000) {
    cleanRegistry();
    lastClean = now;
  }
}
