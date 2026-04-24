/*
 * WORKER - Stage 3: GPS-Tagged WiFi + BLE Scan
 * Board: Seeed XIAO ESP32-C5
 *
 * Adds a UART GPS module to the Stage 2 scan. At the start of each cycle
 * the current GPS fix is captured and printed alongside the scan results.
 * Data with no fix is clearly marked so it can be discarded or filtered later.
 *
 * Wiring:
 *   GPS TX  →  D7 (GPIO12)   C5 UART1 RX
 *   GPS RX  →  D6 (GPIO11)   C5 UART1 TX  (only needed to configure module)
 *   GPS VCC →  3V3
 *   GPS GND →  GND
 *
 * Libraries required:
 *   NimBLE-Arduino by h2zero   (Tools > Manage Libraries)
 *   TinyGPS++ by Mikal Hart     (Tools > Manage Libraries)
 *
 * Arduino IDE settings (all three required):
 *   Tools > USB CDC On Boot    > Enabled
 *   Tools > Partition Scheme   > Huge APP (3MB No OTA/1MB SPIFFS)
 *   Tools > PSRAM              > Disabled
 */

#include <WiFi.h>
#include <NimBLEDevice.h>
#include <TinyGPS++.h>

// GPS UART — Serial1 on the C5's UART1 pins
// D7 = GPIO12 (UART1 RX), D6 = GPIO11 (UART1 TX) on XIAO ESP32-C5
#define GPS_BAUD    9600
#define GPS_RX_PIN  12    // GPIO12 = D7 on XIAO — receives GPS TX
#define GPS_TX_PIN  11    // GPIO11 = D6 on XIAO — sends to GPS RX (optional)

// How long to drain GPS serial at the top of each cycle (ms).
#define GPS_FEED_MS  500

// Time spent on each WiFi channel during a scan.
#define WIFI_MS_PER_CHAN  80

// How long the BLE scan runs each cycle (milliseconds).
#define BLE_SCAN_MS  3000

// Delay between full scan cycles.
#define CYCLE_DELAY_MS  2000

HardwareSerial gpsSerial(1);
TinyGPSPlus    gps;

// ─── GPS helpers ─────────────────────────────────────────────────────────────

// Drain the GPS serial buffer into the parser for up to `ms` milliseconds.
void feedGPS(unsigned long ms) {
  unsigned long deadline = millis() + ms;
  while (millis() < deadline) {
    while (gpsSerial.available())
      gps.encode(gpsSerial.read());
    delay(1);
  }
}

void printGPSStatus() {
  if (gps.location.isValid()) {
    Serial.printf("[WORKER] GPS  %.6f, %.6f | alt %.1fm | sats %d | hdop %.2f\n",
                  gps.location.lat(),
                  gps.location.lng(),
                  gps.altitude.isValid()   ? gps.altitude.meters()       : 0.0,
                  gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                  gps.hdop.isValid()       ? gps.hdop.hdop()             : 99.99);
  } else {
    Serial.printf("[WORKER] GPS  NO FIX (sats seen: %d, chars parsed: %lu, checksum fails: %lu) — scan data has no coordinates\n",
                  gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                  gps.charsProcessed(),
                  gps.failedChecksum());
  }
}

// ─── WiFi helpers ────────────────────────────────────────────────────────────

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

  if (n == WIFI_SCAN_FAILED || n < 0) {
    Serial.println("[WORKER] WiFi scan failed");
    return;
  }
  if (n == 0) {
    Serial.println("[WORKER] No networks found");
    WiFi.scanDelete();
    return;
  }

  int count_2g = 0, count_5g = 0;

  Serial.println("  #    Band  Ch   RSSI  Security        BSSID              SSID");
  Serial.println("  ---  ----  --   ----  --------------  -----------------  ----");

  for (int i = 0; i < n; i++) {
    int ch = WiFi.channel(i);
    bool is_5g = (ch > 14);
    is_5g ? count_5g++ : count_2g++;

    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) ssid = "[hidden]";

    Serial.printf("  %3d  %s  %2d  %4d  %-14s  %s  %s\n",
                  i + 1,
                  is_5g ? "5GHz" : "2.4G",
                  ch,
                  WiFi.RSSI(i),
                  authTypeStr(WiFi.encryptionType(i)),
                  WiFi.BSSIDstr(i).c_str(),
                  ssid.c_str());
  }

  Serial.printf("[WORKER] WiFi: %d network(s) — %d x 2.4GHz, %d x 5GHz\n",
                n, count_2g, count_5g);
  WiFi.scanDelete();
}

// ─── BLE helpers ─────────────────────────────────────────────────────────────

class BLEScanCallbacks : public NimBLEScanCallbacks {
  void onDiscovered(const NimBLEAdvertisedDevice* device) override {
    Serial.printf("  RSSI %4d  %-22s  %s\n",
                  device->getRSSI(),
                  device->getAddress().toString().c_str(),
                  device->getName().empty() ? "[unnamed]" : device->getName().c_str());
  }
};

static NimBLEScan* pBLEScan = nullptr;

void runBLEScan() {
  Serial.printf("\n[WORKER] Starting BLE scan (%d ms)...\n", BLE_SCAN_MS);
  Serial.println("  RSSI  Address                 Name");
  Serial.println("  ----  -------                 ----");

  pBLEScan->clearResults();
  pBLEScan->start(BLE_SCAN_MS, false, false);

  // Feed GPS while waiting — the BLE scan is the longest idle window per cycle.
  while (pBLEScan->isScanning()) {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    delay(10);
  }

  Serial.println("[WORKER] BLE scan complete");
}

// ─── Setup / Loop ────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n========================================");
  Serial.println(" W.A.S.P. Worker — Stage 3");
  Serial.println(" GPS-Tagged WiFi + BLE Scan");
  Serial.println("========================================");

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

  // Brief initial GPS drain — catches a warm fix immediately, won't block on cold start.
  Serial.println(" Waiting for GPS data...");
  feedGPS(2000);

  Serial.println(" Setup complete\n");
}

void loop() {
  Serial.println("\n========================================");

  // Feed GPS and print fix status before each scan cycle.
  // All networks found this cycle share these coordinates.
  feedGPS(GPS_FEED_MS);
  printGPSStatus();

  runWiFiScan();
  runBLEScan();

  Serial.println("\n[WORKER] Waiting before next cycle...");
  delay(CYCLE_DELAY_MS);
}
