#pragma once

#include "worker_types.h"
#include <NimBLEDevice.h>
#include <vector>
#include <WString.h>

extern std::vector<String> bleAddrs;
extern std::vector<String> bleNames;
extern std::vector<int> bleRssis;
extern std::vector<uint16_t> bleMfgrIds;
extern std::vector<bool> bleMfgrFlags;

extern NimBLEScan* pBLEScan;

class BLEScanCallbacks : public NimBLEScanCallbacks {
public:
  void onDiscovered(const NimBLEAdvertisedDevice* d) override;
};

WiFiScanResult runWiFiScan();
int runBLEScan();
