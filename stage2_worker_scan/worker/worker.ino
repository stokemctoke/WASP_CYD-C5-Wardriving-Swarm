/*
 * WORKER - Stage 2: Standalone WiFi + BLE Scan
 * Board: Seeed XIAO ESP32-C5
 *
 * Goal: confirm the C5's dual-band WiFi and BLE radios work before
 *       adding GPS, SD card, or ESP-NOW into the mix.
 *
 * What this sketch does:
 *   1. Scans all 2.4 GHz and 5 GHz WiFi channels
 *   2. Prints each network: band, channel, RSSI, security type, BSSID, SSID
 *   3. Scans for BLE advertisements
 *   4. Prints each BLE device: RSSI, address, name (if advertised)
 *   5. Repeats every few seconds
 *
 * Expected output (example):
 *   [WORKER] Starting WiFi scan...
 *     #   Band  Ch   RSSI  Security        BSSID              SSID
 *     1   2.4G   6   -42   WPA2_PSK        AA:BB:CC:DD:EE:FF  HomeNetwork
 *     2   5GHz  36   -61   WPA2/WPA3       11:22:33:44:55:66  HomeNetwork_5G
 *   [WORKER] WiFi: 2 networks (1 x 2.4GHz, 1 x 5GHz)
 *
 *   [WORKER] Starting BLE scan (3s)...
 *     RSSI  -55   38:44:BE:BA:01:23   MyPhone
 *     RSSI  -72   11:22:33:44:55:66   [unnamed]
 *   [WORKER] BLE scan complete
 *
 * Library required: NimBLE-Arduino by h2zero  (Tools > Manage Libraries)
 * Board package:    arduino-esp32 v3.x
 * IDE setting:      Tools > USB CDC On Boot > Enabled
 */

#include <WiFi.h>
#include <NimBLEDevice.h>

// Time spent on each WiFi channel during a scan.
// Lower = faster scan, but may miss slower-beacon APs.
#define WIFI_MS_PER_CHAN  80

// How long the BLE scan runs each cycle (milliseconds).
#define BLE_SCAN_MS  3000

// Delay between full scan cycles.
#define CYCLE_DELAY_MS  2000

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

  // WIFI_BAND_MODE_AUTO tells the C5 to scan both bands.
  // Without this it defaults to 2.4 GHz only — same as a standard ESP32.
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);

  // Synchronous scan: blocks until all channels are done.
  // show_hidden=true captures networks that don't broadcast their SSID.
  int n = WiFi.scanNetworks(/*async*/false, /*show_hidden*/true,
                            /*passive*/false, WIFI_MS_PER_CHAN);

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

  // Free the scan result memory before next scan.
  WiFi.scanDelete();
}

// ─── BLE helpers ─────────────────────────────────────────────────────────────

// NimBLE fires onDiscovered() for every advertisement packet received.
// setDuplicateFilter(false) means the same device can appear multiple times
// if it advertises repeatedly — useful later when we want RSSI averaging,
// but for now it makes the output noisy so we skip it.
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
  pBLEScan->start(BLE_SCAN_MS, /*is_continue*/false, /*restart*/false);

  while (pBLEScan->isScanning()) delay(50);

  Serial.println("[WORKER] BLE scan complete");
}

// ─── Setup / Loop ────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n========================================");
  Serial.println(" W.A.S.P. Drone — Stage 2");
  Serial.println(" WiFi + BLE Scan");
  Serial.println("========================================");

  // STA mode is required for scanning even without joining a network.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.printf(" MAC: %s\n", WiFi.macAddress().c_str());

  // Initialise NimBLE with no device name — we're purely scanning, not advertising.
  NimBLEDevice::init("");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(new BLEScanCallbacks(), /*report_duplicates*/false);
  pBLEScan->setActiveScan(true);   // active = request scan responses (more device info)
  pBLEScan->setDuplicateFilter(true);  // suppress repeated ads from same device
  pBLEScan->setMaxResults(0);      // 0 = don't buffer, fire callback immediately

  Serial.println(" Setup complete\n");
}

void loop() {
  runWiFiScan();
  runBLEScan();

  Serial.println("\n[WORKER] Waiting before next cycle...");
  Serial.println("========================================");
  delay(CYCLE_DELAY_MS);
}
