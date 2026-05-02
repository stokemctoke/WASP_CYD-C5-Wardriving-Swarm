#include "nest_display.h"
#include "nest_registry.h"
#include "nest_home.h"
#include "nest_config.h"
#include "nest_upload.h"
#include <Arduino.h>

TFT_eSPI tft = TFT_eSPI();

static uint16_t rssiColor(int8_t r) {
  if (r > -50) return 0xFFE0;
  if (r > -70) return 0xFD00;
  if (r > -85) return 0x9D15;
  return TFT_RED;
}

void drawBootMsg(const char* msg) {
  tft.setTextFont(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(CLR_LABEL, CLR_BG);
  tft.fillRect(0, HEADER_H + STATUS_H, 240, ROW_H, CLR_BG);
  tft.drawString(msg, 120, HEADER_H + STATUS_H + ROW_H / 2);
}

void drawHeader() {
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

void refreshDisplay() {
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
