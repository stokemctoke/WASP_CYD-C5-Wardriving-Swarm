#pragma once

#include "nest_types.h"

extern char     lastWigleStr[32];
extern char     lastWdgStr[32];
extern uint32_t lastUploadAttemptMs;
extern uint8_t  homeStatus;

void restoreNestAP();
void runHomeUploads();
