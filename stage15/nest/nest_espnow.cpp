#include "nest_espnow.h"
#include "nest_registry.h"
#include "nest_led.h"
#include <Arduino.h>

void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (!info || !data || len < 1) return;
  int rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
  char mac[18];

  if (data[0] == WASP_PKT_HEARTBEAT && len >= 7) {
    ledHeartbeatFlag = true;
    const heartbeat_t* pkt = (const heartbeat_t*)data;
    uint8_t nodeType = (len >= 8) ? pkt->nodeType : 0;

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

  if (data[0] != WASP_PKT_SUMMARY || len < 36) return;

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
