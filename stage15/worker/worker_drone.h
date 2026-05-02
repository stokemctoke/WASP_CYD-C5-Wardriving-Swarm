#pragma once

#include "worker_types.h"
#include <WString.h>

extern cycle_slot_t* cycleBuffer;
extern uint8_t writeHead;

extern wifi_entry_t* pendingWifi;
extern uint8_t pendingWifiCount;
extern ble_entry_t* pendingBle;
extern uint8_t pendingBleCount;

void clearPending();
void commitCycle();
int countUnuploaded();
String buildCSV(int idx);
