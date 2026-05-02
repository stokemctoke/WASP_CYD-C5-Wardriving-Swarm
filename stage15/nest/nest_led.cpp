#include "nest_led.h"
#include <Arduino.h>

LedEvent evNestBoot       = { 0xFFFFFF, 3,  50,  50 };
LedEvent evNestHeartbeat  = { 0xFF69B4, 2,  80,  80 };
LedEvent evNestChunk      = { 0x0050FF, 1,  80,   0 };
LedEvent evNestUploadAct  = { 0xFF00B4, 0,   0,   0 };
LedEvent evNestUploadOK   = { 0x64FF00, 2, 200, 200 };
LedEvent evNestUploadFail = { 0xFF0000, 3, 200, 200 };

volatile bool ledHeartbeatFlag = false;

void nestLedOff() {
  digitalWrite(NEST_LED_R, HIGH);
  digitalWrite(NEST_LED_G, HIGH);
  digitalWrite(NEST_LED_B, HIGH);
}

void nestLedSet(bool r, bool g, bool b) {
  digitalWrite(NEST_LED_R, r ? LOW : HIGH);
  digitalWrite(NEST_LED_G, g ? LOW : HIGH);
  digitalWrite(NEST_LED_B, b ? LOW : HIGH);
}

void nestLedFlash(bool r, bool g, bool b, int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    nestLedSet(r, g, b);
    delay(onMs);
    nestLedOff();
    if (i < times - 1) delay(offMs);
  }
}

void nestLedFlashEvent(const LedEvent& ev) {
  bool r = ((ev.colour >> 16) & 0xFF) > 0;
  bool g = ((ev.colour >>  8) & 0xFF) > 0;
  bool b = ( ev.colour        & 0xFF) > 0;
  if (ev.flashes == 0) { nestLedSet(r, g, b); return; }
  nestLedFlash(r, g, b, ev.flashes, ev.onMs, ev.offMs);
}
