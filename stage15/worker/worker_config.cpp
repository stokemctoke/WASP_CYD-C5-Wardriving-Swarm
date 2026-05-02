#include "worker_config.h"
#include "worker_led.h"
#include <SD.h>

// ── Nest connection ───────────────────────────────────────────────────────────
char nestSsid[33] = "WASP-Nest";
char nestPsk[65]  = "waspswarm";
char nestIp[16]   = "192.168.4.1";

// ── Sync behaviour ───────────────────────────────────────────────────────────
int syncEvery          = 25;    // sync to Nest every N scan cycles
int heartbeatIntervalMs = 5000; // ms between ESP-NOW heartbeats

// ── Scan timing ──────────────────────────────────────────────────────────────
int wifiChanMs  =  80;   // ms spent on each WiFi channel
int bleScanMs   = 3000;  // ms for each BLE scan pass
int cycleDelayMs = 2000; // ms delay at end of each cycle

// ── Log file ─────────────────────────────────────────────────────────────────
int maxLogBytes      = 8192;  // rotate log file above this size
int lowHeapThreshold = 30000; // heap warning level in bytes

// ── GPS ──────────────────────────────────────────────────────────────────────
int gpsBaud   = 9600;
int gpsRxPin  =   12;  // XIAO C5 = D7
int gpsTxPin  =   11;  // XIAO C5 = D6

// ── LED pins (WS2812 data / rgb4pin R, G, B) ───────────────────────────────
int ledPin  =  3;  // D0
int ledPinG = 23;  // D4
int ledPinB = 24;  // D5

bool parseLedEvent(const String& val, LedEvent& ev) {
  int c1 = val.indexOf(',');
  int c2 = val.indexOf(',', c1 + 1);
  int c3 = val.indexOf(',', c2 + 1);
  if (c1 < 0 || c2 < 0 || c3 < 0) return false;
  ev.colour  = (uint32_t)strtoul(val.substring(0, c1).c_str(), nullptr, 16);
  ev.flashes = val.substring(c1 + 1, c2).toInt();
  ev.onMs    = val.substring(c2 + 1, c3).toInt();
  ev.offMs   = val.substring(c3 + 1).toInt();
  return true;
}

void loadWorkerConfig() {
  if (!SD.exists("/reset.cfg")) {
    File cfg = SD.open("/worker.cfg");
    if (!cfg) return;
    while (cfg.available()) {
      String line = cfg.readStringUntil('\n');
      line.trim();
      if (line.startsWith("#") || line.isEmpty()) continue;
      int eq = line.indexOf('=');
      if (eq < 0) continue;
      String key = line.substring(0, eq); key.trim();
      String val = line.substring(eq + 1);
      int comment = val.indexOf('#');
      if (comment >= 0) val = val.substring(0, comment);
      val.trim();

      // LED
      if      (key == "ledEnabled")    ledEnabled    = (val == "true" || val == "1");
      else if (key == "ledBrightness") ledBrightness = (uint8_t)constrain(val.toInt(), 0, 255);
      else if (key == "ledType")       ledType       = (val == "rgb4pin") ? LED_RGB4PIN : LED_WS2812;
      else if (key == "ledBoot")       parseLedEvent(val, evBoot);
      else if (key == "ledGPSAcquire") parseLedEvent(val, evGPSAcquire);
      else if (key == "ledGPSFound")   parseLedEvent(val, evGPSFound);
      else if (key == "ledGPSFix")     parseLedEvent(val, evGPSFix);
      else if (key == "ledScanCycle")  parseLedEvent(val, evScanCycle);
      else if (key == "ledConnecting") parseLedEvent(val, evConnecting);
      else if (key == "ledSyncOK")     parseLedEvent(val, evSyncOK);
      else if (key == "ledSyncFail")   parseLedEvent(val, evSyncFail);
      else if (key == "ledTooBig")     parseLedEvent(val, evTooBig);
      else if (key == "ledLowHeap")    parseLedEvent(val, evLowHeap);
      else if (key == "ledDronePulse") parseLedEvent(val, evDronePulse);
      else if (key == "ledHeartbeat")  parseLedEvent(val, evHeartbeat);
      // LED pins
      else if (key == "ledPin")        ledPin        = val.toInt();
      else if (key == "ledPinG")       ledPinG       = val.toInt();
      else if (key == "ledPinB")       ledPinB       = val.toInt();
      // GPS
      else if (key == "gpsBaud")       gpsBaud       = val.toInt();
      else if (key == "gpsRxPin")      gpsRxPin      = val.toInt();
      else if (key == "gpsTxPin")      gpsTxPin      = val.toInt();
      // Nest connection
      else if (key == "nestSsid")      val.toCharArray(nestSsid, sizeof(nestSsid));
      else if (key == "nestPsk")       val.toCharArray(nestPsk,  sizeof(nestPsk));
      else if (key == "nestIp")        val.toCharArray(nestIp,   sizeof(nestIp));
      // Sync & timing
      else if (key == "syncEvery")           syncEvery           = max(1, (int)val.toInt());
      else if (key == "heartbeatIntervalMs") heartbeatIntervalMs = max(1000, (int)val.toInt());
      else if (key == "wifiChanMs")          wifiChanMs          = constrain(val.toInt(), 20, 500);
      else if (key == "bleScanMs")           bleScanMs           = constrain(val.toInt(), 500, 10000);
      else if (key == "cycleDelayMs")        cycleDelayMs        = max(0, (int)val.toInt());
      // Log & memory
      else if (key == "maxLogBytes")      maxLogBytes      = constrain(val.toInt(), 1024, 65536);
      else if (key == "lowHeapThreshold") lowHeapThreshold = max(8192, (int)val.toInt());
    }
    cfg.close();
  }
}
