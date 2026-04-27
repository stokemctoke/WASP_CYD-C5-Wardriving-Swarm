#pragma once
#include <stdint.h>

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
