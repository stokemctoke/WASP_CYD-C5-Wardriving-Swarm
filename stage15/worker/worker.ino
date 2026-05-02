/*
 * W.A.S.P. — Unified Worker / Drone Firmware — Stage 15
 * Board: Seeed XIAO ESP32-C5 (on Xiao Expansion dev board)
 *
 * One firmware, two modes — auto-detected at boot:
 *
 *   WORKER — SD card found → logs to SD, GPS used if present
 *   DRONE  — no SD card   → logs to 25-slot RAM circular buffer
 *
 * Both modes sync to the Nest via HTTP upload every syncEvery cycles.
 * If the Nest is not in range, Drone data is retained in the buffer
 * (oldest slot overwritten first when full). Worker renames uploaded
 * files to .done as before.
 *
 * ── Wiring (XIAO Expansion Board) ─────────────────────────────────────────────────
 *   GPS TX  →  D7 (GPIO12)   UART1 RX
 *   GPS RX  →  D6 (GPIO11)   UART1 TX  (optional)
 *   SD CS   →  D2 (GPIO25)   SPI CS
 *   SD SCK  →  D8 (GPIO8)    SPI SCK
 *   SD MISO →  D9 (GPIO9)    SPI MISO
 *   SD MOSI →  D10 (GPIO10)  SPI MOSI
 *   RGB LED →  D0 (GPIO3)    WS2812B data (ws2812 mode) / R-channel (rgb4pin mode)
 *   LED G   →  D4 (GPIO23)   green — rgb4pin mode only (was OLED SDA, free since Stage 11)
 *   LED B   →  D5 (GPIO24)   blue  — rgb4pin mode only (was OLED SCL, free since Stage 11)
 *
 * ── Arduino IDE settings ──────────────────────────────────────────────────────
 *   Tools > Board              > XIAO_ESP32C5
 *   Tools > USB CDC On Boot    > Enabled
 *   Tools > Partition Scheme   > Huge APP (3MB No OTA/1MB SPIFFS)
 *   Tools > PSRAM              > Disabled
 *
 * ── Libraries ────────────────────────────────────────────────────────────────
 *   NimBLE-Arduino by h2zero    (Tools > Manage Libraries)
 *   TinyGPS++ by Mikal Hart      (Tools > Manage Libraries)
 *   Adafruit NeoPixel by Adafruit (Tools > Manage Libraries)
 *
 * ── LED flash vocabulary ──────────────────────────────────────────────────────
 *   White  3×  fast    Boot / power-on confirm
 *   Amber  slow pulse  GPS acquiring (setup)
 *   Cyan   2×          GPS fix acquired
 *   Yellow 1×          Scan cycle start
 *   Blue   fast blink  Connecting to Nest AP
 *   Green  2×          Sync success
 *   Red    3×  fast    Sync fail (nest unreachable)
 *   Orange 4×  fast    Chunked upload failed (file → .defer)
 *   Red    1×  slow    Low heap warning
 *
 * ── Worker config (worker.cfg on worker SD) ───────────────────────────────────
 *   ledEnabled=true      (true/false or 1/0)
 *   ledBrightness=40     (0–255)
 *   ledType=ws2812       (ws2812 or rgb4pin)
 *   ledBoot=FFFFFF,3,50,50                  (colour hex, flashes, onMs, offMs)
 *   ledGPSAcquire=FF3C00,0,400,400          (0 flashes = continuous toggle)
 *   ledGPSFound=64FF00,4,400,300            (one-off burst after module confirmed)
 *   ledGPSFix=64FF00,2,150,100
 *   ledScanCycle=FFDC00,1,100,0
 *   ledConnecting=FF6400,0,200,200          (0 flashes = continuous toggle)
 *   ledSyncOK=00FF00,2,150,100
 *   ledSyncFail=FF0000,3,80,80
 *   ledTooBig=FF6400,4,80,80
 *   ledLowHeap=FF0000,1,400,0
 *   ledDronePulse=0050FF,2,200,100
 *   ledHeartbeat=FF69B4,2,80,80
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <TinyGPS++.h>
#include <SD.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>

#include "worker_types.h"
#include "worker_config.h"
#include "worker_led.h"
#include "worker_gps.h"
#include "worker_storage.h"
#include "worker_drone.h"
#include "worker_scan.h"
#include "worker_espnow.h"
#include "worker_sync.h"

// ── Global state (owned by worker.ino) ─────────────────────────────────────────
// SD pin defines are now in worker_types.h (shared with worker_sync.cpp).
uint32_t cycleCount = 0;
bool     droneMode  = false;

void setup() {
  Serial.begin(115200);
  delay(1000);

  // ── SD + config first — ledType must be known before boot flash ──────────────
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, SPI);
  if (sdOk && !SD.exists("/logs")) SD.mkdir("/logs");
  loadWorkerConfig();

  // ── LED — init hardware for the configured type, then boot flash ──────────────
  if (ledType == LED_WS2812) {
    led.setPin(ledPin);  // apply pin override from config before RMT init
    led.begin();
    led.setBrightness(ledBrightness);
  } else {
    pinMode(ledPin,  OUTPUT); analogWrite(ledPin,  0);
    pinMode(ledPinG, OUTPUT); analogWrite(ledPinG, 0);
    pinMode(ledPinB, OUTPUT); analogWrite(ledPinB, 0);
  }
  ledOff();
  ledBoot();

  Serial.println("\n========================================");
  Serial.println(" W.A.S.P. — Stage 15  Modular Refactor");
  Serial.println("========================================");

  gpsSerial.setRxBufferSize(512);
  gpsSerial.begin(gpsBaud, SERIAL_8N1, gpsRxPin, gpsTxPin);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  Serial.printf(" MAC: %s\n", WiFi.macAddress().c_str());

  initEspNow();
  Serial.println(" ESP-NOW OK");

  NimBLEDevice::init("");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(new BLEScanCallbacks(), false);
  pBLEScan->setActiveScan(true);
  pBLEScan->setDuplicateFilter(true);
  pBLEScan->setMaxResults(0);

  gpsOk = detectGPS();
  if (gpsOk) ledGPSFound();  // amber burst: module confirmed; cyan ledGPSFix() fires in loop() on valid fix

  droneMode = !sdOk;

  if (droneMode) {
    cycleBuffer = (cycle_slot_t*)calloc(CYCLE_SLOTS, sizeof(cycle_slot_t));
    pendingWifi = (wifi_entry_t*)malloc(MAX_WIFI_PER_SLOT * sizeof(wifi_entry_t));
    pendingBle  = (ble_entry_t*)malloc(MAX_BLE_PER_SLOT  * sizeof(ble_entry_t));
    if (!cycleBuffer || !pendingWifi || !pendingBle) {
      Serial.printf("\n[FATAL] Drone-mode buffer alloc failed (free heap=%u). Rebooting in 2s...\n",
                    (unsigned)ESP.getFreeHeap());
      delay(2000);
      ESP.restart();
    }
  }

  // ── Mode banner ──────────────────────────────────────────────────────────────
  Serial.println("\n ┌─────────────────────────────────────┐");
  if (droneMode) {
    Serial.println(" │  Mode  :  DRONE                     │");
    Serial.println(" │  Store :  RAM circular buffer        │");
    Serial.printf(" │  Slots :  %2d x (WiFi:%d + BLE:%d)     │\n",
                  CYCLE_SLOTS, MAX_WIFI_PER_SLOT, MAX_BLE_PER_SLOT);
    Serial.printf(" │  RAM   :  ~%d KB reserved             │\n",
                  (int)(CYCLE_SLOTS * sizeof(cycle_slot_t) / 1024));
  } else {
    Serial.println(" │  Mode  :  WORKER                    │");
    Serial.printf(" │  SD    :  %s                         │\n", sdOk  ? "OK  " : "FAIL");
    Serial.printf(" │  GPS   :  %s                    │\n", gpsOk ? "detected" : "not found");
    if (sdOk) openLogFile();
  }
  Serial.printf(" │  Sync  :  every %d cycles             │\n", syncEvery);
  Serial.printf(" │  LED   :  %-7s  bright %-3d          │\n",
                ledType == LED_RGB4PIN ? "rgb4pin" : "ws2812", ledBrightness);
  Serial.println(" └─────────────────────────────────────┘\n");

  if (!droneMode && sdOk && hasPendingFiles()) {
    Serial.println(" Pending files found — syncing to Nest before first cycle...");
    syncFiles();
  }

  feedGPS(500);
  Serial.println(" Setup complete\n");
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void loop() {
  Serial.printf("\n======================================== [cycle %u]\n", cycleCount + 1);
  maybeHeartbeat();

  // Heap warning — fires once per cycle while heap is low
  if (ESP.getFreeHeap() < lowHeapThreshold) {
    Serial.printf("[WARN] Low heap: %u B\n", (unsigned)ESP.getFreeHeap());
    ledLowHeap();
  }

  if (gpsOk) { feedGPS(GPS_FEED_MS); setClockFromGPS(); }
  printGPSStatus();

  // One-shot cyan flash on first confirmed GPS fix
  if (gpsOk && gps.location.isValid() && !gpsFired) {
    gpsFired = true;
    ledGPSFix();
  }

  if (droneMode) clearPending();

  ledScanCycle();  // yellow flash at the top of every active scan
  WiFiScanResult wifi = runWiFiScan();
  int            ble  = runBLEScan();

  if (!droneMode && sdOk && logFile) flushLog();
  if (droneMode)                     commitCycle();

  sendSummary(wifi.total, wifi.g2, wifi.g5, ble, wifi.bestRssi);
  cycleCount++;

  if (cycleCount % syncEvery == 0) {
    if (droneMode) syncBuffer();
    else           syncFiles();
  }

  Serial.println("\n[WORKER] Waiting before next cycle...");
  delay(cycleDelayMs);
}
