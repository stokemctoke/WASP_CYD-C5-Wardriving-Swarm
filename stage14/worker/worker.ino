/*
 * W.A.S.P. — Unified Worker / Drone Firmware — Stage 13
 * Board: Seeed XIAO ESP32-C5 (on Xiao Expansion dev board)
 *
 * One firmware, two modes — auto-detected at boot:
 *
 *   WORKER — SD card found → logs to SD, GPS used if present
 *   DRONE  — no SD card   → logs to 25-slot RAM circular buffer
 *
 * Both modes sync to the Nest via HTTP upload every syncEvery cycles.
 * If the Nest is not in range, Drone data is retained in the buffer
 * (oldest slot overwritten first when full). Worker renames uploaded
 * files to .done as before.
 *
 * ── Wiring (XIAO Expansion Board) ─────────────────────────────────────────────────
 *   GPS TX  →  D7 (GPIO12)   UART1 RX
 *   GPS RX  →  D6 (GPIO11)   UART1 TX  (optional)
 *   SD CS   →  D2 (GPIO25)   SPI CS
 *   SD SCK  →  D8 (GPIO8)    SPI SCK
 *   SD MISO →  D9 (GPIO9)    SPI MISO
 *   SD MOSI →  D10 (GPIO10)  SPI MOSI
 *   RGB LED →  D0 (GPIO3)    WS2812B data (ws2812 mode) / R-channel (rgb4pin mode)
 *   LED G   →  D4 (GPIO23)   green — rgb4pin mode only (was OLED SDA, free since Stage 11)
 *   LED B   →  D5 (GPIO24)   blue  — rgb4pin mode only (was OLED SCL, free since Stage 11)
 *
 * ── Arduino IDE settings ──────────────────────────────────────────────────────
 *   Tools > Board              > XIAO_ESP32C5
 *   Tools > USB CDC On Boot    > Enabled
 *   Tools > Partition Scheme   > Huge APP (3MB No OTA/1MB SPIFFS)
 *   Tools > PSRAM              > Disabled
 *
 * ── Libraries ────────────────────────────────────────────────────────────────
 *   NimBLE-Arduino by h2zero    (Tools > Manage Libraries)
 *   TinyGPS++ by Mikal Hart      (Tools > Manage Libraries)
 *   Adafruit NeoPixel by Adafruit (Tools > Manage Libraries)
 *
 * ── LED flash vocabulary ──────────────────────────────────────────────────────
 *   White  3×  fast    Boot / power-on confirm
 *   Amber  slow pulse  GPS acquiring (setup)
 *   Cyan   2×          GPS fix acquired
 *   Yellow 1×          Scan cycle start
 *   Blue   fast blink  Connecting to Nest AP
 *   Green  2×          Sync success
 *   Red    3×  fast    Sync fail (nest unreachable)
 *   Orange 4×  fast    Chunked upload failed (file → .defer)
 *   Red    1×  slow    Low heap warning
 *
 * ── Worker config (worker.cfg on worker SD) ───────────────────────────────────
 *   ledEnabled=true      (true/false or 1/0)
 *   ledBrightness=40     (0–255)
 *   ledType=ws2812       (ws2812 or rgb4pin)
 *   ledBoot=FFFFFF,3,50,50                  (colour hex, flashes, onMs, offMs)
 *   ledGPSAcquire=FF3C00,0,400,400          (0 flashes = continuous toggle)
 *   ledGPSFound=64FF00,4,400,300            (one-off burst after module confirmed)
 *   ledGPSFix=64FF00,2,150,100
 *   ledScanCycle=FFDC00,1,100,0
 *   ledConnecting=FF6400,0,200,200          (0 flashes = continuous toggle)
 *   ledSyncOK=00FF00,2,150,100
 *   ledSyncFail=FF0000,3,80,80
 *   ledTooBig=FF6400,4,80,80
 *   ledLowHeap=FF0000,1,400,0
 *   ledDronePulse=0050FF,2,200,100
 *   ledHeartbeat=FF69B4,2,80,80
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <TinyGPS++.h>
#include <SD.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
#include <vector>
#include <sys/time.h>

// ── ESP-NOW ───────────────────────────────────────────────────────────────────
#define ESPNOW_CHANNEL 1
static const uint8_t NEST_MAC[6] = {0xA4, 0xF0, 0x0F, 0x5D, 0x96, 0xD4};

#define WASP_PKT_SUMMARY   0x01
#define WASP_PKT_HEARTBEAT 0x02
#define WASP_FIRMWARE_VER  13

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
#define NEST_UPLOAD_PORT  8080   // worker raw TCP uploads — fixed, matches nest server

// ── SD (SPI bus) ─────────────────────────────────────────────────────────────
// Cannot be moved to SD config — SD must be mounted before config can be read.
#define SD_CS    25
#define SD_SCK    8
#define SD_MISO   9
#define SD_MOSI  10

// ── LED count / internal GPS timing ──────────────────────────────────────────
#define LED_COUNT     1
#define GPS_DETECT_MS 2000
#define GPS_FEED_MS   500

// ── LED event descriptor ──────────────────────────────────────────────────────
// Holds colour + timing for one named LED state.
// flashes=0 in evGPSAcquire / evConnecting means "continuous toggle" (inline loops).
struct LedEvent {
  uint32_t colour;
  int      flashes;
  int      onMs;
  int      offMs;
};

// ── RAM buffer (drone mode) — fixed at compile time ──────────────────────────
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

// ── LED state ─────────────────────────────────────────────────────────────────
enum LedType { LED_WS2812, LED_RGB4PIN };
static LedType ledType       = LED_WS2812; // overridden by worker.cfg
static uint8_t ledBrightness = 40;         // overridden by worker.cfg
static bool    ledEnabled    = true;       // overridden by worker.cfg
static bool    gpsFired      = false;

// Default values match the hardcoded behaviour from prior stages.
// All overrideable via worker.cfg: ledBoot=RRGGBB,flashes,onMs,offMs
static LedEvent evBoot       = { 0xFFFFFF, 3,   50,  50 }; // white  — startup / config loaded
static LedEvent evGPSAcquire = { 0xFF3C00, 0,  400, 400 }; // amber  — acquiring GPS, no fix (continuous)
static LedEvent evGPSFound   = { 0x64FF00, 4,  400, 300 }; // lime   — GPS fix first acquired
static LedEvent evGPSFix     = { 0x64FF00, 2,  150, 100 }; // lime   — GPS has fix
static LedEvent evScanCycle  = { 0xFFDC00, 1,  100,   0 }; // yellow — new network / scan cycle
static LedEvent evConnecting = { 0xFF6400, 0,  200, 200 }; // orange — WiFi connecting (continuous)
static LedEvent evSyncOK     = { 0x00FF00, 2,  150, 100 }; // green  — sync / WiFi OK
static LedEvent evSyncFail   = { 0xFF0000, 3,   80,  80 }; // red    — sync failed
static LedEvent evTooBig     = { 0xFF6400, 4,   80,  80 }; // orange — file too big
static LedEvent evLowHeap    = { 0xFF0000, 1,  400,   0 }; // red    — low heap
static LedEvent evDronePulse = { 0x0050FF, 2,  200, 100 }; // blue   — drone connection pulse
static LedEvent evHeartbeat  = { 0xFF69B4, 2,   80,  80 }; // pink   — heartbeat / idle

Adafruit_NeoPixel led(LED_COUNT, 3, NEO_GRB + NEO_KHZ800); // pin overridden in setup() after config load

// ── SD-configurable runtime settings ─────────────────────────────────────────
// All overrideable via worker.cfg. Defaults match prior compiled behaviour.

// Nest connection
static char nestSsid[33] = "WASP-Nest";
static char nestPsk[65]  = "waspswarm";
static char nestIp[16]   = "192.168.4.1";

// Sync behaviour
static int syncEvery          = 25;    // sync to Nest every N scan cycles
static int heartbeatIntervalMs = 5000; // ms between ESP-NOW heartbeats

// Scan timing
static int wifiChanMs  =  80;   // ms spent on each WiFi channel
static int bleScanMs   = 3000;  // ms for each BLE scan pass
static int cycleDelayMs = 2000; // ms delay at end of each cycle

// Log file
static int maxLogBytes      = 8192;  // rotate log file above this size
static int lowHeapThreshold = 30000; // heap warning level in bytes

// GPS
static int gpsBaud   = 9600;
static int gpsRxPin  =   12;  // XIAO C5 = D7
static int gpsTxPin  =   11;  // XIAO C5 = D6

// LED pins (WS2812 data / rgb4pin R, G, B)
static int ledPin  =  3;  // D0
static int ledPinG = 23;  // D4
static int ledPinB = 24;  // D5

HardwareSerial gpsSerial(1);
TinyGPSPlus    gps;

// ── RGB LED helpers ───────────────────────────────────────────────────────────

static void ledOff() {
  if (ledType == LED_RGB4PIN) {
    analogWrite(ledPin, 0); analogWrite(ledPinG, 0); analogWrite(ledPinB, 0);
  } else {
    led.clear(); led.show();
  }
}

static void ledSet(uint32_t colour) {
  if (!ledEnabled) { ledOff(); return; }
  if (ledType == LED_RGB4PIN) {
    uint8_t r = (colour >> 16) & 0xFF;
    uint8_t g = (colour >>  8) & 0xFF;
    uint8_t b =  colour        & 0xFF;
    analogWrite(ledPin,   r * ledBrightness / 255);
    analogWrite(ledPinG, g * ledBrightness / 255);
    analogWrite(ledPinB, b * ledBrightness / 255);
  } else {
    led.setBrightness(ledBrightness);
    led.setPixelColor(0, colour);
    led.show();
  }
}

static void ledFlash(uint32_t colour, int times, int onMs, int offMs) {
  if (!ledEnabled) return;
  for (int i = 0; i < times; i++) {
    ledSet(colour);
    delay(onMs);
    ledOff();
    if (i < times - 1) delay(offMs);
  }
}

// Named wrappers — all delegate to configurable LedEvent structs (worker.cfg)
static void ledBoot()       { ledFlash(evBoot.colour,       evBoot.flashes,       evBoot.onMs,       evBoot.offMs); }
static void ledGPSFound()   { ledFlash(evGPSFound.colour,   evGPSFound.flashes,   evGPSFound.onMs,   evGPSFound.offMs); }
static void ledGPSFix()     { ledFlash(evGPSFix.colour,     evGPSFix.flashes,     evGPSFix.onMs,     evGPSFix.offMs); }
static void ledScanCycle()  { ledFlash(evScanCycle.colour,  evScanCycle.flashes,  evScanCycle.onMs,  evScanCycle.offMs); }
static void ledSyncOK()     { ledFlash(evSyncOK.colour,     evSyncOK.flashes,     evSyncOK.onMs,     evSyncOK.offMs); }
static void ledSyncFail()   { ledFlash(evSyncFail.colour,   evSyncFail.flashes,   evSyncFail.onMs,   evSyncFail.offMs); }
static void ledTooBig()     { ledFlash(evTooBig.colour,     evTooBig.flashes,     evTooBig.onMs,     evTooBig.offMs); }
static void ledLowHeap()    { ledFlash(evLowHeap.colour,    evLowHeap.flashes,    evLowHeap.onMs,    evLowHeap.offMs); }
static void ledHeartbeat()  { ledFlash(evHeartbeat.colour,  evHeartbeat.flashes,  evHeartbeat.onMs,  evHeartbeat.offMs); }
static void ledDronePulse() {
  ledFlash(evDronePulse.colour, evDronePulse.flashes, evDronePulse.onMs, evDronePulse.offMs);
  delay(300);
  ledFlash(evDronePulse.colour, evDronePulse.flashes, evDronePulse.onMs, evDronePulse.offMs);
}

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
  unsigned long start      = millis();
  unsigned long lastToggle = 0;
  bool          ledOn      = false;
  while (millis() - start < GPS_DETECT_MS) {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    // Slow amber pulse (400ms half-period = 800ms cycle) while waiting for GPS
    if (millis() - lastToggle >= (unsigned long)evGPSAcquire.onMs) {
      lastToggle = millis();
      ledOn = !ledOn;
      ledOn ? ledSet(evGPSAcquire.colour) : ledOff();
    }
    Serial.print(".");
    delay(100);
  }
  ledOff();
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
  if (gps.date.year() < 2020) return;
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
  if (logFile && (int)logFile.size() >= maxLogBytes) {
    Serial.printf("[SD] Rotating log: %s reached %u B (cap %d)\n",
                  logPath.c_str(), (unsigned)logFile.size(), maxLogBytes);
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
  logFile.println("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type,RCOIs,MfgrId");
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
  snprintf(line, sizeof(line), "%s,\"%s\",%s,%s,%d,%d,%d,%.6f,%.6f,%.0f,%.1f,WIFI,,",
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
  snprintf(line, sizeof(line), "%s,\"%s\",[BLE],%s,0,,%d,%.6f,%.6f,%.0f,%.1f,BLE,,%s",
           mac.c_str(), safe.c_str(), ts.c_str(), rssi, lat, lon, altM, accuracy, mfgrField);
  logFile.println(line);
  maybeFlush();
}

// ── RAM buffer helpers (drone mode) ───────────────────────────────────────────

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
  if (!cycleBuffer) return 0;
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
  csv += "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type,RCOIs,MfgrId\n";

  for (int i = 0; i < s.wifiCount; i++) {
    const wifi_entry_t& w = s.wifi[i];
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
             w.bssid[0], w.bssid[1], w.bssid[2], w.bssid[3], w.bssid[4], w.bssid[5]);
    String ssid = String(w.ssid);
    ssid.replace("\"", "\"\"");
    char line[200];
    snprintf(line, sizeof(line), "%s,\"%s\",%s,%s,%d,%d,%d,0.000000,0.000000,0,999.9,WIFI,,",
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
    snprintf(line, sizeof(line), "%s,\"%s\",[BLE],%s,0,,%d,0.000000,0.000000,0,999.9,BLE,,%s",
             addr, name.c_str(), nowTimestamp().c_str(), b.rssi, mfgrField);
    csv += line; csv += '\n';
  }
  return csv;
}

// ── Worker config loader ──────────────────────────────────────────────────────

static bool parseLedEvent(const String& val, LedEvent& ev) {
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

static void loadWorkerConfig() {
  if (!sdOk) return;

  // If /reset.cfg exists on the SD card, skip worker.cfg entirely and boot on compiled defaults.
  if (SD.exists("/reset.cfg")) {
    Serial.println("[CFG] /reset.cfg found — using compiled defaults, worker.cfg ignored");
    return;
  }

  File cfg = SD.open("/worker.cfg");
  if (!cfg) return;
  while (cfg.available()) {
    String line = cfg.readStringUntil('\n');
    line.trim();
    if (line.startsWith("#") || line.isEmpty()) continue;
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String key = line.substring(0, eq); key.trim();
    String val = line.substring(eq + 1);
    int comment = val.indexOf('#');
    if (comment >= 0) val = val.substring(0, comment);
    val.trim();

    // LED
    if      (key == "ledEnabled")    ledEnabled    = (val == "true" || val == "1");
    else if (key == "ledBrightness") ledBrightness = (uint8_t)constrain(val.toInt(), 0, 255);
    else if (key == "ledType")       ledType       = (val == "rgb4pin") ? LED_RGB4PIN : LED_WS2812;
    else if (key == "ledBoot")       parseLedEvent(val, evBoot);
    else if (key == "ledGPSAcquire") parseLedEvent(val, evGPSAcquire);
    else if (key == "ledGPSFound")   parseLedEvent(val, evGPSFound);
    else if (key == "ledGPSFix")     parseLedEvent(val, evGPSFix);
    else if (key == "ledScanCycle")  parseLedEvent(val, evScanCycle);
    else if (key == "ledConnecting") parseLedEvent(val, evConnecting);
    else if (key == "ledSyncOK")     parseLedEvent(val, evSyncOK);
    else if (key == "ledSyncFail")   parseLedEvent(val, evSyncFail);
    else if (key == "ledTooBig")     parseLedEvent(val, evTooBig);
    else if (key == "ledLowHeap")    parseLedEvent(val, evLowHeap);
    else if (key == "ledDronePulse") parseLedEvent(val, evDronePulse);
    else if (key == "ledHeartbeat")  parseLedEvent(val, evHeartbeat);
    // LED pins
    else if (key == "ledPin")        ledPin        = val.toInt();
    else if (key == "ledPinG")       ledPinG       = val.toInt();
    else if (key == "ledPinB")       ledPinB       = val.toInt();
    // GPS
    else if (key == "gpsBaud")       gpsBaud       = val.toInt();
    else if (key == "gpsRxPin")      gpsRxPin      = val.toInt();
    else if (key == "gpsTxPin")      gpsTxPin      = val.toInt();
    // Nest connection
    else if (key == "nestSsid")      val.toCharArray(nestSsid, sizeof(nestSsid));
    else if (key == "nestPsk")       val.toCharArray(nestPsk,  sizeof(nestPsk));
    else if (key == "nestIp")        val.toCharArray(nestIp,   sizeof(nestIp));
    // Sync & timing
    else if (key == "syncEvery")           syncEvery           = max(1, (int)val.toInt());
    else if (key == "heartbeatIntervalMs") heartbeatIntervalMs = max(1000, (int)val.toInt());
    else if (key == "wifiChanMs")          wifiChanMs          = constrain(val.toInt(), 20, 500);
    else if (key == "bleScanMs")           bleScanMs           = constrain(val.toInt(), 500, 10000);
    else if (key == "cycleDelayMs")        cycleDelayMs        = max(0, (int)val.toInt());
    // Log & memory
    else if (key == "maxLogBytes")      maxLogBytes      = constrain(val.toInt(), 1024, 65536);
    else if (key == "lowHeapThreshold") lowHeapThreshold = max(8192, (int)val.toInt());
  }
  cfg.close();
  Serial.printf("[CFG] led=%s/%d/%s  nest=%s  sync=%d  wifiChan=%dms  ble=%dms  cycle=%dms\n",
                ledEnabled ? "on" : "off", ledBrightness,
                ledType == LED_RGB4PIN ? "rgb4pin" : "ws2812",
                nestSsid, syncEvery, wifiChanMs, bleScanMs, cycleDelayMs);
  Serial.printf("[CFG] gps=%dbaud rx=%d tx=%d  ledPin=%d,%d,%d  maxLog=%dB  heap=%dB\n",
                gpsBaud, gpsRxPin, gpsTxPin, ledPin, ledPinG, ledPinB,
                maxLogBytes, lowHeapThreshold);
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
  ledHeartbeat();
}

static void maybeHeartbeat() {
  if (millis() - lastHeartbeatMs >= heartbeatIntervalMs) sendHeartbeat();
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
  WiFi.begin(nestSsid, nestPsk);
  unsigned long t0       = millis();
  unsigned long lastLedMs = 0;
  bool          ledOn    = false;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000) {
    if (gpsOk) while (gpsSerial.available()) gps.encode(gpsSerial.read());
    // Fast blue blink while connecting
    if (millis() - lastLedMs >= (unsigned long)evConnecting.onMs) {
      lastLedMs = millis();
      ledOn = !ledOn;
      ledOn ? ledSet(evConnecting.colour) : ledOff();
    }
    delay(evConnecting.onMs); Serial.print(".");
  }
  ledOff();
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

// Uploads a file in maxLogBytes segments — each chunk is a separate TCP
// connection so WiFi is fully down while reading the next chunk from SD.
static bool uploadFileChunked(const String& path, const String& name, int fileSize) {
  int totalChunks = (fileSize + maxLogBytes - 1) / maxLogBytes;
  Serial.printf("[SYNC]  chunked: %d chunk(s) × up to %d B\n", totalChunks, maxLogBytes);

  uint8_t* buf = (uint8_t*)malloc(maxLogBytes);
  if (!buf) {
    Serial.printf("[OOM for %d B chunk buf]\n", maxLogBytes);
    return false;
  }

  bool allOk = true;
  for (int ci = 0; ci < totalChunks && allOk; ci++) {
    int offset    = ci * maxLogBytes;
    int chunkSize = min(fileSize - offset, maxLogBytes);

    // ── Read chunk while WiFi is OFF ──────────────────────────────────────────
    {
      File f = SD.open(path);
      if (!f) {
        Serial.printf("[SD open failed chunk %d/%d]\n", ci + 1, totalChunks);
        allOk = false; break;
      }
      if (!f.seek(offset)) {
        Serial.printf("[SD seek failed chunk %d/%d]\n", ci + 1, totalChunks);
        f.close(); allOk = false; break;
      }
      int got = f.read(buf, chunkSize);
      f.close();
      if (got != chunkSize) {
        Serial.printf("[SD short read chunk %d/%d: %d/%d B]\n",
                      ci + 1, totalChunks, got, chunkSize);
        allOk = false; break;
      }
    }

    // ── Upload chunk over the already-open nest WiFi association ─────────────
    // Caller is responsible for connectToNest() before / disconnectFromNest() after.
    Serial.printf("         chunk %d/%d  %d B  ...", ci + 1, totalChunks, chunkSize);

    String myMac = WiFi.macAddress(); myMac.replace(":", "");

    WiFiClient tcp;
    bool chunkOk = false;
    if (tcp.connect(nestIp, NEST_UPLOAD_PORT)) {
      tcp.setNoDelay(true);
      tcp.setTimeout(5000);
      tcp.printf("UPLOAD_CHUNK %s %s %d %d %d\n",
                 myMac.c_str(), name.c_str(), ci, totalChunks, chunkSize);
      tcp.flush();
      String ready = tcp.readStringUntil('\n'); ready.trim();
      if (ready == "READY") {
        const uint8_t* ptr = buf;
        int sent = 0, rem = chunkSize;
        uint32_t deadline = millis() + 15000;
        while (rem > 0 && tcp.connected() && millis() < deadline) {
          int w = tcp.write(ptr, min(rem, 1460));
          if (w > 0) { ptr += w; sent += w; rem -= w; }
          else if (w < 0) break;
          else delay(1);
        }
        tcp.flush();
        String resp = tcp.readStringUntil('\n'); resp.trim();
        Serial.printf("[%s]", resp.c_str());
        chunkOk = (resp == "OK") && (sent == chunkSize);
      } else {
        Serial.printf("[bad READY: %s]", ready.c_str());
      }
    } else {
      Serial.print("[tcp FAIL]");
    }
    tcp.stop();
    Serial.println(chunkOk ? " OK" : " FAILED");
    if (!chunkOk) allOk = false;
    if (ci < totalChunks - 1) delay(50);
  }

  free(buf);
  return allOk;
}

static void syncFiles() {
  Serial.println("\n[SYNC] Starting...");
  if (logFile) { logFile.flush(); logFile.close(); }

  int uploaded = 0, failed = 0;

  // ── Restore .toobig files — they will be retried via chunked upload ─────────
  {
    File dir = SD.open("/logs");
    if (dir) {
      while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        String n = String(entry.name());
        bool   d = entry.isDirectory();
        entry.close();
        if (!d && n.endsWith(".csv.toobig")) {
          String op = "/logs/" + n;
          String np = op.substring(0, op.length() - 8);
          SD.rename(op.c_str(), np.c_str());
          Serial.printf("[SYNC]  Queued for chunked retry: %s\n", np.c_str());
        }
      }
      dir.close();
    }
  }

  // SD+WiFi DMA coexistence on the ESP32-C5 is unreliable mid-transfer:
  // f.read() hangs after the first chunk once the WiFi stack is active.
  // Fix: read the whole file into RAM while WiFi is OFF, then connect and
  // stream from the buffer. OOM files are deferred and retried next sync.
  //
  // Scan /logs/ once upfront — avoids re-walking all .done entries on every
  // file (O(n) not O(n²) when many completed files accumulate on the card).
  const int MAX_QUEUE = 100;
  String queueName[MAX_QUEUE];
  int    queueSize[MAX_QUEUE];
  int    queueLen = 0;
  {
    File dir = SD.open("/logs");
    if (dir) {
      while (queueLen < MAX_QUEUE) {
        File entry = dir.openNextFile();
        if (!entry) break;
        String n = String(entry.name());
        bool d = entry.isDirectory();
        int s = (int)entry.size();
        entry.close();
        if (!d && n.endsWith(".csv") && s > 0) {
          queueName[queueLen] = n;
          queueSize[queueLen] = s;
          queueLen++;
        }
      }
      dir.close();
    }
  }
  Serial.printf("[SYNC] %d file(s) queued\n", queueLen);

  // ── Connect to nest ONCE, hold the association across all files ────────────
  // Previously we connected/disconnected per file (and per chunk!), burning
  // ~5 s of WiFi handshake overhead on every transfer. One association is
  // enough — TCP sockets are cheap to open over an existing WiFi link.
  if (queueLen > 0) {
    if (!connectToNest()) {
      Serial.println("[SYNC] Nest WiFi unreachable — aborting sync");
      // Fall through to deferred-restore + log-reopen at the end of syncFiles().
      queueLen = 0;
    } else {
      Serial.printf("[SYNC] Connected  IP: %s\n", WiFi.localIP().toString().c_str());
    }
  }

  for (int qi = 0; qi < queueLen; qi++) {
    String name = queueName[qi];
    int    sz   = queueSize[qi];
    String path = name.startsWith("/") ? name : "/logs/" + name;

    // ── Route by size: small → single-shot, large → chunked ─────────────────
    if (sz > maxLogBytes) {
      Serial.printf("[SYNC]  %-32s  %5d B\n", name.c_str(), sz);
      bool ok = uploadFileChunked(path, name, sz);
      if (ok) {
        SD.rename(path.c_str(), (path + ".done").c_str());
        Serial.printf("[SYNC]  %s → .done\n", name.c_str()); uploaded++;
      } else {
        ledTooBig();  // orange 4× — large file failed even after chunked attempt
        SD.rename(path.c_str(), (path + ".defer").c_str());
        Serial.printf("[SYNC]  %s → .defer\n", name.c_str()); failed++;
      }
      delay(50);
      continue;
    }

    Serial.printf("[SYNC]  %-32s  %5d B  ...", name.c_str(), sz);

    // ── Pre-buffer entire file from SD ─────────────────────────────────────
    // SD/WiFi DMA coexistence on ESP32-C5: f.read() can hang once WiFi is
    // active. Reading the whole file into RAM up front side-steps that.
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

    // ── Stream over the existing nest WiFi association ───────────────────────
    bool ok      = false;
    int  sent    = 0;
    bool tcpFail = false;
    {
      String myMac = WiFi.macAddress(); myMac.replace(":", "");
      WiFiClient tcp;
      Serial.print(" tcp..");
      if (tcp.connect(nestIp, NEST_UPLOAD_PORT)) {
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
            int toSend = min(rem, 1460);
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

    if (ok) {
      SD.rename(path.c_str(), (path + ".done").c_str());
      Serial.printf(" OK\n"); uploaded++;
    } else if (tcpFail) {
      // Nest AP still associated but its TCP server isn't responding — likely
      // restarting, busy with home upload, or moved out of range. Stop now,
      // restore deferred files at the end of syncFiles, retry next cycle.
      Serial.println("\n[SYNC] Nest TCP not reachable — stopping");
      break;
    } else {
      Serial.printf(" FAILED (%d/%d B) — deferred\n", sent, sz);
      SD.rename(path.c_str(), (path + ".defer").c_str());
      failed++;
    }
    delay(50);
  }

  // ── Disconnect from nest ONCE after all files attempted ────────────────────
  if (WiFi.status() == WL_CONNECTED) disconnectFromNest();

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

  // LED summary: one flash at end of the whole sync rather than per-file
  if (uploaded > 0 && failed == 0) ledSyncOK();
  else if (failed > 0)             ledSyncFail();

  if (sdOk) openLogFile();
}

// ── Buffer sync (drone mode) ──────────────────────────────────────────────────

static void syncBuffer() {
  int pending = countUnuploaded();
  if (pending == 0) { Serial.println("[SYNC] Buffer empty — nothing to send"); return; }

  Serial.printf("\n[SYNC] Connecting to Nest AP (%d slot(s) pending)...\n", pending);

  if (!connectToNest()) {
    Serial.println("[SYNC] Nest not reachable — buffer retained");
    ledSyncFail();
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
    http.begin("http://" + String(nestIp) + "/upload?worker=" + myMac + "&file=" + fileName);
    http.addHeader("Content-Type", "text/csv");
    int code = http.POST(csv);
    http.end();

    if (code == 200) { cycleBuffer[i].uploaded = true; Serial.println(" OK"); uploaded++; }
    else             { Serial.printf(" FAILED (HTTP %d)\n", code); failed++; }
  }
  Serial.printf("[SYNC] Done — %d uploaded, %d failed\n", uploaded, failed);

  if (uploaded > 0 && failed == 0) ledSyncOK();
  else if (failed > 0)             ledSyncFail();

  disconnectFromNest();
  ledDronePulse();
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
  int n = WiFi.scanNetworks(false, true, false, wifiChanMs);

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
  Serial.printf("\n[WORKER] Starting BLE scan (%d ms)...\n", bleScanMs);
  Serial.println("  RSSI  Address                 Name");
  Serial.println("  ----  -------                 ----");

  bleResults.clear();
  pBLEScan->clearResults();
  pBLEScan->start(bleScanMs, false, false);

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

  // ── SD + config first — ledType must be known before boot flash ──────────────
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, SPI);
  if (sdOk && !SD.exists("/logs")) SD.mkdir("/logs");
  loadWorkerConfig();

  // ── LED — init hardware for the configured type, then boot flash ──────────────
  if (ledType == LED_WS2812) {
    led.setPin(ledPin);  // apply pin override from config before RMT init
    led.begin();
    led.setBrightness(ledBrightness);
  } else {
    pinMode(ledPin,  OUTPUT); analogWrite(ledPin,  0);
    pinMode(ledPinG, OUTPUT); analogWrite(ledPinG, 0);
    pinMode(ledPinB, OUTPUT); analogWrite(ledPinB, 0);
  }
  ledOff();
  ledBoot();

  Serial.println("\n========================================");
  Serial.println(" W.A.S.P. — Stage 14  SD Config");
  Serial.println("========================================");

  gpsSerial.setRxBufferSize(512);
  gpsSerial.begin(gpsBaud, SERIAL_8N1, gpsRxPin, gpsTxPin);

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

  gpsOk = detectGPS();
  if (gpsOk) ledGPSFound();  // amber burst: module confirmed; cyan ledGPSFix() fires in loop() on valid fix

  droneMode = !sdOk;

  if (droneMode) {
    cycleBuffer = (cycle_slot_t*)calloc(CYCLE_SLOTS, sizeof(cycle_slot_t));
    pendingWifi = (wifi_entry_t*)malloc(MAX_WIFI_PER_SLOT * sizeof(wifi_entry_t));
    pendingBle  = (ble_entry_t*)malloc(MAX_BLE_PER_SLOT  * sizeof(ble_entry_t));
    if (!cycleBuffer || !pendingWifi || !pendingBle) {
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
  Serial.printf(" │  Sync  :  every %d cycles             │\n", syncEvery);
  Serial.printf(" │  LED   :  %-7s  bright %-3d          │\n",
                ledType == LED_RGB4PIN ? "rgb4pin" : "ws2812", ledBrightness);
  Serial.println(" └─────────────────────────────────────┘\n");

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

  // Heap warning — fires once per cycle while heap is low
  if (ESP.getFreeHeap() < lowHeapThreshold) {
    Serial.printf("[WARN] Low heap: %u B\n", (unsigned)ESP.getFreeHeap());
    ledLowHeap();
  }

  if (gpsOk) { feedGPS(GPS_FEED_MS); setClockFromGPS(); }
  printGPSStatus();

  // One-shot cyan flash on first confirmed GPS fix
  if (gpsOk && gps.location.isValid() && !gpsFired) {
    gpsFired = true;
    ledGPSFix();
  }

  if (droneMode) clearPending();

  ledScanCycle();  // yellow flash at the top of every active scan
  WiFiScanResult wifi = runWiFiScan();
  int            ble  = runBLEScan();

  if (!droneMode && sdOk && logFile) flushLog();
  if (droneMode)                     commitCycle();

  sendSummary(wifi.total, wifi.g2, wifi.g5, ble, wifi.bestRssi);
  cycleCount++;

  if (cycleCount % syncEvery == 0) {
    if (droneMode) syncBuffer();
    else           syncFiles();
  }

  Serial.println("\n[WORKER] Waiting before next cycle...");
  delay(cycleDelayMs);
}
