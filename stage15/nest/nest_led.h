#pragma once

#include "nest_types.h"

// ── LED event instances (overrideable from wasp.cfg) ─────────────────────────
extern LedEvent evNestBoot;
extern LedEvent evNestHeartbeat;
extern LedEvent evNestChunk;
extern LedEvent evNestUploadAct;
extern LedEvent evNestUploadOK;
extern LedEvent evNestUploadFail;

// Flag set by ESP-NOW callback; consumed by loop() to avoid delay() in ISR context
extern volatile bool ledHeartbeatFlag;

void nestLedOff();
void nestLedSet(bool r, bool g, bool b);
void nestLedFlash(bool r, bool g, bool b, int times, int onMs, int offMs);
void nestLedFlashEvent(const LedEvent& ev);
