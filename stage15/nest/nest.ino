/*
 * W.A.S.P. — Nest Firmware — Stage 15
 * Board: CYD (JC2432W328C) — standard ESP32
 *
 * ── TFT_eSPI setup (required before first flash) ─────────────────────────────
 * Install "TFT_eSPI" by Bodmer via Library Manager.
 * Edit Arduino/libraries/TFT_eSPI/User_Setup.h — comment out existing driver
 * and pins, then add:
 *
 *   #define ILI9341_DRIVER
 *   #define TFT_MISO  12
 *   #define TFT_MOSI  13
 *   #define TFT_SCLK  14
 *   #define TFT_CS    15
 *   #define TFT_DC     2
 *   #define TFT_RST    4
 *   #define LOAD_GLCD
 *   #define LOAD_FONT2
 *   #define LOAD_FONT4
 *   #define SPI_FREQUENCY       40000000
 *   #define SPI_READ_FREQUENCY  20000000
 *
 * ── CYD wiring ───────────────────────────────────────────────────────────────
 *   Display backlight : GPIO21  (PWM via LEDC channel 0)
 *   Display SPI       : HSPI — configured in User_Setup.h above
 *   SD card SPI       : VSPI — CS=5  SCK=18  MISO=19  MOSI=23
 *   Touch SPI         : HSPI shared (CLK=25 MISO=39 MOSI=32 CS=33 IRQ=36)
 *                       If touch is unresponsive swap to CLK=14/MISO=12/MOSI=13
 *
 * ── wasp.cfg keys ────────────────────────────────────────────────────────────
 *   homeSsid / homePsk         Home WiFi credentials
 *   apSsid / apPsk             Nest AP credentials (default: WASP-Nest / waspswarm)
 *   wigleBasicToken            WiGLE Basic auth token
 *   wdgwarsApiKey              WDGWars API key (64 hex chars)
 *   nestLedBoot=FFFFFF,3,50,50 LED events: colour,flashes,onMs,offMs
 *   nestLedHeartbeat / nestLedChunk / nestLedUploadAct / nestLedUploadOK / nestLedUploadFail
 */

#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include "nest_types.h"
#include "nest_config.h"
#include "nest_led.h"
#include "nest_registry.h"
#include "nest_espnow.h"
#include "nest_upload.h"
#include "nest_home.h"
#include "nest_display.h"
#include "nest_touch.h"
#include "nest_ui.h"

bool     sdOk = false;
SPIClass sdSpi(VSPI);

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             info.wifi_ap_staconnected.mac[0], info.wifi_ap_staconnected.mac[1],
             info.wifi_ap_staconnected.mac[2], info.wifi_ap_staconnected.mac[3],
             info.wifi_ap_staconnected.mac[4], info.wifi_ap_staconnected.mac[5]);
    Serial.printf("[NEST] Worker connected: %s\n", mac);
    taskENTER_CRITICAL(&gLock);
    int idx = findWorker(info.wifi_ap_staconnected.mac);
    if (idx >= 0) workers[idx].lastSeenMs = millis();
    taskEXIT_CRITICAL(&gLock);
  } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    Serial.println("[NEST] Worker disconnected");
  }
}

static void uploadTask(void*) {
  for (;;) {
    server.handleClient();
    handleRawUpload();
    vTaskDelay(1);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n========================================");
  Serial.println(" W.A.S.P. Nest — Stage 15  Modular Refactor");
  Serial.println("========================================");
  Serial.printf("[BOOT] heap=%u\n", ESP.getFreeHeap());

  Serial.println("[BOOT] 1/9 backlight");
  ledcAttach(TFT_BACKLIGHT, 5000, 8);
  ledcWrite(TFT_BACKLIGHT, 255);

  Serial.println("[BOOT] 2/9 tft.init()");
  tft.init();
  // GPIO 4 (NEST_LED_R) free after tft.init() — TFT_RST pulse completed
  pinMode(NEST_LED_R, OUTPUT);
  pinMode(NEST_LED_G, OUTPUT);
  pinMode(NEST_LED_B, OUTPUT);
  nestLedOff();
  nestLedFlashEvent(evNestBoot);

  tft.setRotation(0);
  tft.fillScreen(CLR_BG);
  drawHeader();

  Serial.println("[BOOT] 3/9 SD init");
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  drawBootMsg("SD...");
  if (SD.begin(SD_CS, sdSpi)) {
    sdOk = true;
    if (!SD.exists("/logs")) SD.mkdir("/logs");
    Serial.println(" SD OK");
  } else {
    Serial.println(" SD FAIL — uploads will be rejected");
  }

  Serial.println("[BOOT] 4/9 loadConfig()");
  drawBootMsg("Reading config...");
  loadConfig();
  if (cfg.homeSsid[0]) {
    homeStatus = 0;
    Serial.printf(" Home WiFi  : %s (credentials set)\n", cfg.homeSsid);
    Serial.printf(" WiGLE      : %s\n", cfg.wigleBasicToken[0] ? "token set" : "not set");
    Serial.printf(" WDGWars    : %s\n", cfg.wdgwarsApiKey[0]   ? "key set"   : "not set");
  } else {
    Serial.println(" Home WiFi  : not configured");
  }

  Serial.println("[BOOT] 5/9 WiFi mode + softAP");
  drawBootMsg("WiFi...");
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  String staMac = WiFi.macAddress();
  WiFi.softAP(cfg.apSsid, cfg.apPsk, ESPNOW_CHANNEL);
  delay(100);

  Serial.printf(" STA MAC: %s\n", staMac.c_str());
  Serial.printf(" AP  MAC: %s\n", WiFi.softAPmacAddress().c_str());
  Serial.printf(" AP: %-12s  IP: %s  ch: %d\n",
                cfg.apSsid, WiFi.softAPIP().toString().c_str(), ESPNOW_CHANNEL);

  Serial.println("[BOOT] 6/9 esp_now_init");
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    Serial.println(" ERROR: esp_now_init() failed");
  } else {
    esp_now_register_recv_cb(onDataRecv);
    Serial.printf(" ESP-NOW ready on channel %d\n", ESPNOW_CHANNEL);
  }

  Serial.println("[BOOT] 7/9 HTTP servers");
  server.on("/upload", HTTP_POST, handleUpload);
  server.begin();
  rawServer.begin();
  Serial.println(" HTTP server started (port 80)");
  Serial.println(" Raw upload server started (port 8080)");

  Serial.println("[BOOT] 8/9 uploadTask spawn");
  xTaskCreatePinnedToCore(uploadTask, "upload", 12288, NULL, 5, NULL, 0);

  Serial.println("[BOOT] 9/9 touch init");
  touchBegin();
  Serial.println(" Touch ready");

  Serial.println("[BOOT] 10/10 setup() complete");
  Serial.printf("[BOOT] heap=%u\n", ESP.getFreeHeap());
  tft.fillRect(0, HEADER_H, 240, 320 - HEADER_H, CLR_BG);
  drawCurrentScreen();
}

void loop() {
  static uint32_t lastRefresh = 0;
  static uint32_t lastClean   = 0;
  uint32_t now = millis();

  handleTouch();
  touchDiag();

  if (ledHeartbeatFlag) { ledHeartbeatFlag = false; nestLedFlashEvent(evNestHeartbeat); }

  if (now - lastUploadAttemptMs >= HOME_UPLOAD_INTERVAL_MS) {
    lastUploadAttemptMs = now;
    runHomeUploads();
  }

  if (now - lastRefresh >= 1000) { refreshDisplay(); lastRefresh = now; }
  if (now - lastClean >= 30000) { cleanRegistry(); lastClean = now; }
}
