#pragma once

#include "worker_types.h"
#include <SD.h>
#include <esp_wifi.h>
#include <WString.h>

extern File logFile;
extern String logPath;
extern uint32_t linesSinceFlush;
extern uint32_t lastFlushMs;

void flushLog();
void maybeFlush();
String newLogPath();
bool openLogFile();

int channelToFreq(int ch);
const char* wigleAuth(wifi_auth_mode_t auth);

void logWiFiRow(const String& mac, const String& ssid, wifi_auth_mode_t auth,
                int channel, int rssi,
                double lat, double lon, double altM, double accuracy);

void logBLERow(const String& mac, const String& name, int rssi,
               double lat, double lon, double altM, double accuracy,
               bool hasMfgr, uint16_t mfgrId);
