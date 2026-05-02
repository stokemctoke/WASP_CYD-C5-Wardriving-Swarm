#pragma once

#include <stdint.h>
#include <WString.h>

// ── Pins ──────────────────────────────────────────────────────────────────────
#define TFT_BACKLIGHT  21
#define SD_CS           5
#define SD_SCK         18
#define SD_MISO        19
#define SD_MOSI        23

// ── CYD onboard RGB LED (active LOW) ─────────────────────────────────────────
// GPIO 4 shared with TFT_RST — safe to use after tft.init() completes.
#define NEST_LED_R  4
#define NEST_LED_G  16
#define NEST_LED_B  17

// ── Network ───────────────────────────────────────────────────────────────────
#define ESPNOW_CHANNEL          1
#define HOME_UPLOAD_INTERVAL_MS 300000
#define HOME_CONNECT_TIMEOUT_MS  15000

// ── Worker registry ───────────────────────────────────────────────────────────
#define MAX_WORKERS        8
#define WORKER_TIMEOUT_MS  30000
#define WORKER_DISPLAY_MS  90000
#define WORKER_REMOVE_MS  300000

// ── Display layout (240 × 320 portrait) ──────────────────────────────────────
#define HEADER_H  28
#define STATUS_H  16
#define ROW_H     52
#define MAX_ROWS   5
#define FOOTER_H  16

// ── Colours (RGB565) ──────────────────────────────────────────────────────────
#define CLR_BG        0x0002
#define CLR_HDR_BG    0xFD00
#define CLR_HDR_FG    0xF7BF
#define CLR_LABEL     0x9D15
#define CLR_ACTIVE    0xFFE0
#define CLR_STALE     0xFD00
#define CLR_OFFLINE   0x2945
#define CLR_GPS_OK    0x07FF
#define CLR_GPS_NO    0x9D15
#define CLR_DIVIDER   0x2945
#define CLR_FTR_BG    0x2945

// ── Packet types ─────────────────────────────────────────────────────────────
#define WASP_PKT_SUMMARY   0x01
#define WASP_PKT_HEARTBEAT 0x02
#define WASP_FIRMWARE_VER  10

// ── LED event descriptor ──────────────────────────────────────────────────────
// colour = 24-bit RGB; CYD LED is binary so only non-zero channels are checked.
// flashes = 0 means solid on (used for upload-in-progress indicator).
struct LedEvent {
  uint32_t colour;
  int      flashes;
  int      onMs;
  int      offMs;
};

// ── Worker registry entry ─────────────────────────────────────────────────────
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

// ── Config struct ─────────────────────────────────────────────────────────────
struct wasp_config_t {
  char homeSsid[64];
  char homePsk[64];
  char apSsid[32];
  char apPsk[32];
  char wigleBasicToken[128];
  char wdgwarsApiKey[72];
};

// ── ESP-NOW packet structs ────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
  uint8_t  type;
  uint8_t  workerMac[6];
  uint8_t  nodeType;
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
