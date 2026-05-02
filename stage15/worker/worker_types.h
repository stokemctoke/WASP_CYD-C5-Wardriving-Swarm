#pragma once

#include <cstdint>

// ── ESP-NOW ───────────────────────────────────────────────────────────────────
#define ESPNOW_CHANNEL        1
#define WASP_PKT_SUMMARY      0x01
#define WASP_PKT_HEARTBEAT    0x02
#define WASP_FIRMWARE_VER     15

// ── Nest AP ───────────────────────────────────────────────────────────────────
#define NEST_UPLOAD_PORT      8080

// ── SD card pins ──────────────────────────────────────────────────────────────
#define SD_CS    25
#define SD_SCK    8
#define SD_MISO   9
#define SD_MOSI  10

// ── LED ────────────────────────────────────────────────────────────────────────
#define LED_COUNT             1
#define GPS_DETECT_MS         2000
#define GPS_FEED_MS           500

// ── RAM buffer (drone mode) ───────────────────────────────────────────────────
#define CYCLE_SLOTS           25
#define MAX_WIFI_PER_SLOT     40
#define MAX_BLE_PER_SLOT      20

// ── Nest MAC (used by ESP-NOW) ────────────────────────────────────────────────
static const uint8_t NEST_MAC[6] = {0xA4, 0xF0, 0x0F, 0x5D, 0x96, 0xD4};

// ── LED type enum ─────────────────────────────────────────────────────────────
enum LedType { LED_WS2812, LED_RGB4PIN };

// ── LED event descriptor ──────────────────────────────────────────────────────
// Holds colour + timing for one named LED state.
// flashes=0 in evGPSAcquire / evConnecting means "continuous toggle" (inline loops).
struct LedEvent {
  uint32_t colour;
  int      flashes;
  int      onMs;
  int      offMs;
};

// ── WiFi scan result ──────────────────────────────────────────────────────────
struct WiFiScanResult {
  int total, g2, g5;
  int8_t bestRssi;
};

// ── RAM buffer (drone mode) — fixed at compile time ──────────────────────────
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

struct cycle_slot_t {
  bool         used;
  bool         uploaded;
  uint32_t     capturedMs;
  uint8_t      wifiCount;
  uint8_t      bleCount;
  wifi_entry_t wifi[MAX_WIFI_PER_SLOT];
  ble_entry_t  ble[MAX_BLE_PER_SLOT];
};

// ── ESP-NOW packet types ──────────────────────────────────────────────────────
// Extended packet header (appended to both packet types). All fields zero-filled
// by senders that do not yet populate them. Receiver never rejects solely because
// extended fields are zero.
//
//  swarmId     — djb2 hash of swarm name; nest filters ESP-NOW on this
//  loyaltyLevel— 0=wild, 255=fully loyal (drone game mechanic)
//  gangId      — WDGWars gang affiliation (0=ungrouped)
//  firmwareVer — sender firmware stage; for cross-swarm compat checks
//  battLevel   — 0–100 %, 255=unknown
//  playerLevel — rank / promotion mechanic
//  boostLevel  — active boost / buff tier
//  reserved    — zero-filled; reserved for future game fields
//
// heartbeat_t  : 8 + 16 = 24 bytes
// scan_summary_t: 36 + 16 = 52 bytes

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
