/*
 * WORKER - Stage 4: GPS-Tagged WiFi + BLE Scan + SD Logging (WiGLE CSV)
 * Board: Seeed XIAO ESP32-C5 (on Piglet dev board)
 *
 * Adds SD card logging to Stage 3. Each boot creates a new WiGLE-format
 * CSV in /logs/ on the SD card. Rows are only written when a GPS fix is
 * valid so every entry carries real coordinates.
 *
 * Wiring (Piglet dev board, confirmed working):
 *   GPS TX  →  D7 (GPIO12)   UART1 RX
 *   GPS RX  →  D6 (GPIO11)   UART1 TX  (optional)
 *   SD CS   →  D2 (GPIO25)   SPI CS
 *   SD SCK  →  D8 (GPIO8)    SPI SCK
 *   SD MISO →  D9 (GPIO9)    SPI MISO
 *   SD MOSI →  D10 (GPIO10)  SPI MOSI
 *
 * Libraries required:
 *   NimBLE-Arduino by h2zero   (Tools > Manage Libraries)
 *   TinyGPS++ by Mikal Hart     (Tools > Manage Libraries)
 *   SD (built-in)
 *
 * Arduino IDE settings:
 *   Tools > USB CDC On Boot    > Enabled
 *   Tools > Partition Scheme   > Huge APP (3MB No OTA/1MB SPIFFS)
 *   Tools > PSRAM              > Disabled
 */

#include <WiFi.h>
#include <NimBLEDevice.h>
#include <TinyGPS++.h>
#include <SD.h>
#include <SPI.h>
#include <vector>

// ── GPS ──────────────────────────────────────────────────────────────────────
#define GPS_BAUD    9600
#define GPS_RX_PIN  12    // GPIO12 = D7
#define GPS_TX_PIN  11    // GPIO11 = D6

// ── SD (Piglet dev board C5 pin map, confirmed working) ──────────────────────
#define SD_CS    25   // GPIO25 = D2
#define SD_SCK    8   // GPIO8  = D8
#define SD_MISO   9   // GPIO9  = D9
#define SD_MOSI  10   // GPIO10 = D10

// ── Scan timing ──────────────────────────────────────────────────────────────
#define GPS_FEED_MS       500
#define WIFI_MS_PER_CHAN   80
#define BLE_SCAN_MS      3000
#define CYCLE_DELAY_MS   2000

HardwareSerial gpsSerial(1);
TinyGPSPlus    gps;

static File     logFile;
static bool     sdOk            = false;
static String   logPath;
static uint32_t linesSinceFlush = 0;
static uint32_t lastFlushMs     = 0;

// ── GPS helpers ───────────────────────────────────────────────────────────────

void feedGPS(unsigned long ms) {
  unsigned long deadline = millis() + ms;
  while (millis() < deadline) {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    delay(1);
  }
}

void printGPSStatus() {
  if (gps.location.isValid()) {
    Serial.printf("[WORKER] GPS  %.6f, %.6f | alt %.1fm | sats %d | hdop %.2f\n",
                  gps.location.lat(), gps.location.lng(),
                  gps.altitude.isValid()   ? gps.altitude.meters()       : 0.0,
                  gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                  gps.hdop.isValid()       ? gps.hdop.hdop()             : 99.99);
  } else {
    Serial.printf("[WORKER] GPS  NO FIX (sats seen: %d, chars parsed: %lu)\n",
                  gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                  gps.charsProcessed());
  }
}

// "YYYY-MM-DD HH:MM:SS" from GPS, or empty if time not yet valid.
static String gpsTimestamp() {
  if (!gps.date.isValid() || !gps.time.isValid()) return "";
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           gps.date.year(), gps.date.month(), gps.date.day(),
           gps.time.hour(), gps.time.minute(), gps.time.second());
  return String(buf);
}

// ── SD helpers ────────────────────────────────────────────────────────────────

static void flushLog() {
  if (logFile) logFile.flush();
  linesSinceFlush = 0;
  lastFlushMs     = millis();
}

static void maybeFlush() {
  linesSinceFlush++;
  if (linesSinceFlush >= 25 || (millis() - lastFlushMs) >= 2000) flushLog();
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
  if (!logFile) {
    Serial.printf("[SD] Failed to open %s\n", logPath.c_str());
    return false;
  }
  logFile.println("WigleWifi-1.6,appRelease=1,model=XIAO-ESP32C5,release=1,device=WASP-Worker,display=,board=XIAO-ESP32C5,brand=Seeed,star=Sol,body=3,subBody=0");
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

  String safe = ssid;
  safe.replace("\"", "\"\"");

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

  String safe = name;
  safe.replace("\"", "\"\"");

  char mfgrField[8] = "";
  if (hasMfgr) snprintf(mfgrField, sizeof(mfgrField), "%u", mfgrId);

  char line[256];
  snprintf(line, sizeof(line), "%s,\"%s\",[BLE],%s,0,,%d,%.6f,%.6f,%.0f,%.1f,,%s,BLE",
           mac.c_str(), safe.c_str(), ts.c_str(), rssi, lat, lon, altM, accuracy, mfgrField);
  logFile.println(line);
  maybeFlush();
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

void runWiFiScan() {
  Serial.println("\n[WORKER] Starting WiFi scan (2.4 GHz + 5 GHz)...");
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);
  int n = WiFi.scanNetworks(false, true, false, WIFI_MS_PER_CHAN);

  if (n == WIFI_SCAN_FAILED || n < 0) { Serial.println("[WORKER] WiFi scan failed"); return; }
  if (n == 0) { Serial.println("[WORKER] No networks found"); WiFi.scanDelete(); return; }

  bool   hasFix   = gps.location.isValid();
  double lat      = hasFix ? gps.location.lat()    : 0.0;
  double lon      = hasFix ? gps.location.lng()    : 0.0;
  double alt      = hasFix && gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
  double accuracy = hasFix && gps.hdop.isValid()     ? gps.hdop.hdop() * 5.0  : 999.9;

  int count_2g = 0, count_5g = 0;
  Serial.println("  #    Band  Ch   RSSI  Security        BSSID              SSID");
  Serial.println("  ---  ----  --   ----  --------------  -----------------  ----");

  for (int i = 0; i < n; i++) {
    int  ch    = WiFi.channel(i);
    bool is_5g = (ch > 14);
    is_5g ? count_5g++ : count_2g++;

    String ssid = WiFi.SSID(i);

    Serial.printf("  %3d  %s  %2d  %4d  %-14s  %s  %s\n",
                  i + 1, is_5g ? "5GHz" : "2.4G", ch, WiFi.RSSI(i),
                  authTypeStr(WiFi.encryptionType(i)),
                  WiFi.BSSIDstr(i).c_str(),
                  ssid.isEmpty() ? "[hidden]" : ssid.c_str());

    if (hasFix && sdOk && logFile)
      logWiFiRow(WiFi.BSSIDstr(i), ssid, WiFi.encryptionType(i),
                 ch, WiFi.RSSI(i), lat, lon, alt, accuracy);
  }

  Serial.printf("[WORKER] WiFi: %d network(s) — %d x 2.4GHz, %d x 5GHz%s\n",
                n, count_2g, count_5g,
                hasFix ? "" : " | no GPS fix — not logged");
  WiFi.scanDelete();
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
      if (mfg.size() >= 2) {
        mfgrId  = (uint8_t)mfg[0] | ((uint8_t)mfg[1] << 8);
        hasMfgr = true;
      }
    }
    bleResults.push_back({ String(d->getAddress().toString().c_str()), name, d->getRSSI(), mfgrId, hasMfgr });
    Serial.printf("  RSSI %4d  %-22s  mfgr %-6s  %s\n",
                  d->getRSSI(), d->getAddress().toString().c_str(),
                  hasMfgr ? String(mfgrId).c_str() : "-",
                  name.isEmpty() ? "[unnamed]" : name.c_str());
  }
};

static NimBLEScan* pBLEScan = nullptr;

void runBLEScan() {
  Serial.printf("\n[WORKER] Starting BLE scan (%d ms)...\n", BLE_SCAN_MS);
  Serial.println("  RSSI  Address                 Name");
  Serial.println("  ----  -------                 ----");

  bleResults.clear();
  pBLEScan->clearResults();
  pBLEScan->start(BLE_SCAN_MS, false, false);

  while (pBLEScan->isScanning()) {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    delay(10);
  }

  if (gps.location.isValid() && sdOk && logFile) {
    double lat      = gps.location.lat();
    double lon      = gps.location.lng();
    double alt      = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
    double accuracy = gps.hdop.isValid()     ? gps.hdop.hdop() * 5.0  : 999.9;
    for (auto& e : bleResults)
      logBLERow(e.addr, e.name, e.rssi, lat, lon, alt, accuracy, e.hasMfgr, e.mfgrId);
  }

  Serial.printf("[WORKER] BLE: %d device(s)%s\n",
                (int)bleResults.size(),
                gps.location.isValid() ? "" : " | no GPS fix — not logged");
}

// ── Setup / Loop ──────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n========================================");
  Serial.println(" W.A.S.P. Worker — Stage 4");
  Serial.println(" GPS + SD WiGLE Logging");
  Serial.println("========================================");

  gpsSerial.setRxBufferSize(512);
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  Serial.printf(" MAC: %s\n", WiFi.macAddress().c_str());

  NimBLEDevice::init("");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(new BLEScanCallbacks(), false);
  pBLEScan->setActiveScan(true);
  pBLEScan->setDuplicateFilter(true);
  pBLEScan->setMaxResults(0);

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, SPI)) {
    Serial.println(" SD OK");
    sdOk = openLogFile();
  } else {
    Serial.println(" SD FAIL — logging disabled, scanning continues");
  }

  Serial.println(" Waiting for GPS data...");
  feedGPS(2000);
  Serial.println(" Setup complete\n");
}

void loop() {
  Serial.println("\n========================================");
  feedGPS(GPS_FEED_MS);
  printGPSStatus();

  runWiFiScan();
  runBLEScan();

  if (sdOk && logFile) flushLog();

  Serial.println("\n[WORKER] Waiting before next cycle...");
  delay(CYCLE_DELAY_MS);
}
