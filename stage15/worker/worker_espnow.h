#pragma once

#include "worker_types.h"
#include <esp_now.h>

extern uint32_t lastHeartbeatMs;

void onSendResult(const wifi_tx_info_t*, esp_now_send_status_t status);
void initEspNow();
void sendHeartbeat();
void maybeHeartbeat();
void sendSummary(int wifiTotal, int wifi2g, int wifi5g, int bleCount, int8_t bestRssi);
