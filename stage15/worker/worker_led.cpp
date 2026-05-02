#include "worker_led.h"

// ── LED state ─────────────────────────────────────────────────────────────────
LedType ledType       = LED_WS2812; // overridden by worker.cfg
uint8_t ledBrightness = 40;         // overridden by worker.cfg
bool    ledEnabled    = true;       // overridden by worker.cfg
bool    gpsFired      = false;

Adafruit_NeoPixel led(LED_COUNT, 3, NEO_GRB + NEO_KHZ800);

// ── LED event defaults (overridden by worker.cfg at boot) ─────────────────────
LedEvent evBoot       = { 0xFFFFFF, 3,   50,  50 }; // white  — startup / config loaded
LedEvent evGPSAcquire = { 0xFF3C00, 0,  400, 400 }; // amber  — acquiring GPS, no fix (continuous)
LedEvent evGPSFound   = { 0x64FF00, 4,  400, 300 }; // lime   — GPS fix first acquired
LedEvent evGPSFix     = { 0x64FF00, 2,  150, 100 }; // lime   — GPS has fix
LedEvent evScanCycle  = { 0xFFDC00, 1,  100,   0 }; // yellow — new network / scan cycle
LedEvent evConnecting = { 0xFF6400, 0,  200, 200 }; // orange — WiFi connecting (continuous)
LedEvent evSyncOK     = { 0x00FF00, 2,  150, 100 }; // green  — sync / WiFi OK
LedEvent evSyncFail   = { 0xFF0000, 3,   80,  80 }; // red    — sync failed
LedEvent evTooBig     = { 0xFF6400, 4,   80,  80 }; // orange — file too big
LedEvent evLowHeap    = { 0xFF0000, 1,  400,   0 }; // red    — low heap
LedEvent evDronePulse = { 0x0050FF, 2,  200, 100 }; // blue   — drone connection pulse
LedEvent evHeartbeat  = { 0xFF69B4, 2,   80,  80 }; // pink   — heartbeat / idle

// ── RGB LED helpers ───────────────────────────────────────────────────────────

static void ledFlashEvent(const LedEvent& ev) {
  uint32_t colour = ev.colour;
  uint8_t r = (colour >> 16) & 0xFF;
  uint8_t g = (colour >>  8) & 0xFF;
  uint8_t b =  colour        & 0xFF;

  if (ev.flashes == 0) {
    ledSet(colour);
    return;
  }
  ledFlash(colour, ev.flashes, ev.onMs, ev.offMs);
}

void ledOff() {
  if (ledType == LED_RGB4PIN) {
    extern int ledPin, ledPinG, ledPinB;
    analogWrite(ledPin, 0);
    analogWrite(ledPinG, 0);
    analogWrite(ledPinB, 0);
  } else {
    led.clear();
    led.show();
  }
}

void ledSet(uint32_t colour) {
  if (!ledEnabled) { ledOff(); return; }
  if (ledType == LED_RGB4PIN) {
    extern int ledPin, ledPinG, ledPinB;
    uint8_t r = (colour >> 16) & 0xFF;
    uint8_t g = (colour >>  8) & 0xFF;
    uint8_t b =  colour        & 0xFF;
    analogWrite(ledPin,   r * ledBrightness / 255);
    analogWrite(ledPinG, g * ledBrightness / 255);
    analogWrite(ledPinB, b * ledBrightness / 255);
  } else {
    led.setBrightness(ledBrightness);
    led.setPixelColor(0, colour);
    led.show();
  }
}

void ledFlash(uint32_t colour, int times, int onMs, int offMs) {
  if (!ledEnabled) return;
  for (int i = 0; i < times; i++) {
    ledSet(colour);
    delay(onMs);
    ledOff();
    if (i < times - 1) delay(offMs);
  }
}

void ledBoot()       { ledFlashEvent(evBoot); }
void ledGPSFound()   { ledFlashEvent(evGPSFound); }
void ledGPSFix()     { ledFlashEvent(evGPSFix); }
void ledScanCycle()  { ledFlashEvent(evScanCycle); }
void ledSyncOK()     { ledFlashEvent(evSyncOK); }
void ledSyncFail()   { ledFlashEvent(evSyncFail); }
void ledTooBig()     { ledFlashEvent(evTooBig); }
void ledLowHeap()    { ledFlashEvent(evLowHeap); }
void ledHeartbeat()  { ledFlashEvent(evHeartbeat); }

void ledDronePulse() {
  ledFlashEvent(evDronePulse);
  delay(300);
  ledFlashEvent(evDronePulse);
}
