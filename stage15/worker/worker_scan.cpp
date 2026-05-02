#include "worker_scan.h"
#include "worker_gps.h"
#include "worker_storage.h"
#include "worker_drone.h"
#include "worker_led.h"
#include "worker_config.h"
#include <WiFi.h>
#include <vector>

std::vector<String> bleAddrs;
std::vector<String> bleNames;
std::vector<int> bleRssis;
std::vector<uint16_t> bleMfgrIds;
std::vector<bool> bleMfgrFlags;

NimBLEScan* pBLEScan = nullptr;

void BLEScanCallbacks::onDiscovered(const NimBLEAdvertisedDevice* d) {
  String   name    = d->getName().empty() ? "" : String(d->getName().c_str());
  uint16_t mfgrId  = 0;
  bool     hasMfgr = false;
  if (d->haveManufacturerData()) {
    std::string mfg = d->getManufacturerData();
    if (mfg.size() >= 2) { mfgrId = (uint8_t)mfg[0] | ((uint8_t)mfg[1] << 8); hasMfgr = true; }
  }
  bleAddrs.push_back(String(d->getAddress().toString().c_str()));
  bleNames.push_back(name);
  bleRssis.push_back(d->getRSSI());
  bleMfgrIds.push_back(mfgrId);
  bleMfgrFlags.push_back(hasMfgr);
  Serial.printf("  RSSI %4d  %-22s  %s\n",
                d->getRSSI(), d->getAddress().toString().c_str(),
                name.isEmpty() ? "[unnamed]" : name.c_str());
}

const char* authTypeStr(wifi_auth_mode_t auth) {
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
  extern bool droneMode;
  extern File logFile;
  extern bool sdOk;
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

int runBLEScan() {
  extern bool droneMode;
  extern File logFile;
  extern bool sdOk;
  Serial.printf("\n[WORKER] Starting BLE scan (%d ms)...\n", bleScanMs);
  Serial.println("  RSSI  Address                 Name");
  Serial.println("  ----  -------                 ----");

  bleAddrs.clear();
  bleNames.clear();
  bleRssis.clear();
  bleMfgrIds.clear();
  bleMfgrFlags.clear();
  pBLEScan->clearResults();
  pBLEScan->start(bleScanMs, false, false);

  while (pBLEScan->isScanning()) {
    if (gpsOk) while (gpsSerial.available()) gps.encode(gpsSerial.read());
    extern void maybeHeartbeat();
    maybeHeartbeat();
    delay(10);
  }

  if (!droneMode) {
    if (gpsOk && gps.location.isValid() && sdOk && logFile) {
      double lat      = gps.location.lat();
      double lon      = gps.location.lng();
      double alt      = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
      double accuracy = gps.hdop.isValid()     ? gps.hdop.hdop() * 5.0  : 999.9;
      for (int i = 0; i < (int)bleAddrs.size(); i++)
        logBLERow(bleAddrs[i], bleNames[i], bleRssis[i], lat, lon, alt, accuracy, bleMfgrFlags[i], bleMfgrIds[i]);
    }
  } else {
    for (int i = 0; i < (int)bleAddrs.size(); i++) {
      if (pendingBleCount >= MAX_BLE_PER_SLOT) break;
      ble_entry_t& b = pendingBle[pendingBleCount++];
      sscanf(bleAddrs[i].c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
             &b.addr[0], &b.addr[1], &b.addr[2],
             &b.addr[3], &b.addr[4], &b.addr[5]);
      strncpy(b.name, bleNames[i].c_str(), 20); b.name[20] = '\0';
      b.rssi    = (int8_t)bleRssis[i];
      b.mfgrId  = bleMfgrIds[i];
      b.hasMfgr = bleMfgrFlags[i];
    }
  }

  Serial.printf("[WORKER] BLE: %d device(s)\n", (int)bleAddrs.size());
  return (int)bleAddrs.size();
}
