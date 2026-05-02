#pragma once

#include "worker_types.h"
#include <Adafruit_NeoPixel.h>

// ── LED state ─────────────────────────────────────────────────────────────────
extern LedType ledType;
extern uint8_t ledBrightness;
extern bool    ledEnabled;
extern bool    gpsFired;

extern Adafruit_NeoPixel led;

// ── LED event instances – overrideable via worker.cfg at boot ──────────────────
extern LedEvent evBoot;
extern LedEvent evGPSAcquire;
extern LedEvent evGPSFound;
extern LedEvent evGPSFix;
extern LedEvent evScanCycle;
extern LedEvent evConnecting;
extern LedEvent evSyncOK;
extern LedEvent evSyncFail;
extern LedEvent evTooBig;
extern LedEvent evLowHeap;
extern LedEvent evDronePulse;
extern LedEvent evHeartbeat;

// ── RGB LED helpers ───────────────────────────────────────────────────────────
void ledOff();
void ledSet(uint32_t colour);
void ledFlash(uint32_t colour, int times, int onMs, int offMs);

// Named wrappers
void ledBoot();
void ledGPSFound();
void ledGPSFix();
void ledScanCycle();
void ledSyncOK();
void ledSyncFail();
void ledTooBig();
void ledLowHeap();
void ledHeartbeat();
void ledDronePulse();
