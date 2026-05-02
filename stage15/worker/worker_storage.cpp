#include "worker_storage.h"
#include "worker_gps.h"
#include "worker_config.h"

File logFile;
String logPath;
uint32_t linesSinceFlush = 0;
uint32_t lastFlushMs = 0;

void flushLog() {
  if (logFile) logFile.flush();
  linesSinceFlush = 0;
  lastFlushMs = millis();
}

void maybeFlush() {
  if (++linesSinceFlush >= 25 || (millis() - lastFlushMs) >= 2000) flushLog();
  if (logFile && (int)logFile.size() >= maxLogBytes) {
    Serial.printf("[SD] Rotating log: %s reached %u B (cap %d)\n",
                  logPath.c_str(), (unsigned)logFile.size(), maxLogBytes);
    logFile.close();
    openLogFile();
  }
}

String newLogPath() {
  if (!SD.exists("/logs")) SD.mkdir("/logs");
  for (int i = 0; i < 25; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "/logs/WASP_%lu_%08lX.csv",
             (unsigned long)millis(), (unsigned long)esp_random());
    if (!SD.exists(buf)) return String(buf);
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "/logs/WASP_%lu.csv", (unsigned long)millis());
  return String(buf);
}

bool openLogFile() {
  logPath = newLogPath();
  logFile = SD.open(logPath, FILE_WRITE);
  if (!logFile) { Serial.printf("[SD] Failed to open %s\n", logPath.c_str()); return false; }
  logFile.println("WigleWifi-1.6,appRelease=1,model=WASP-WarDriver_v1,release=1,device=WASP-Worker,display=,board=XIAO-ESP32C5,brand=Seeed,star=Sol,body=3,subBody=0");
  logFile.println("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type,RCOIs,MfgrId");
  logFile.flush();
  Serial.printf("[SD] Log: %s\n", logPath.c_str());
  return true;
}

int channelToFreq(int ch) {
  if (ch >= 1 && ch <= 13) return 2412 + (ch - 1) * 5;
  if (ch == 14) return 2484;
  if (ch >= 36)  return 5000 + ch * 5;
  return 0;
}

const char* wigleAuth(wifi_auth_mode_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN:            return "[ESS]";
    case WIFI_AUTH_WEP:             return "[WEP]";
    case WIFI_AUTH_WPA_PSK:         return "[WPA][ESS]";
    case WIFI_AUTH_WPA2_PSK:        return "[WPA2][ESS]";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "[WPA][WPA2][ESS]";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "[WPA2-EAP][ESS]";
    case WIFI_AUTH_WPA3_PSK:        return "[WPA3][ESS]";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "[WPA2][WPA3][ESS]";
    default:                        return "[ESS]";
  }
}

void logWiFiRow(const String& mac, const String& ssid, wifi_auth_mode_t auth,
                int channel, int rssi,
                double lat, double lon, double altM, double accuracy) {
  String ts = gpsTimestamp();
  if (ts.isEmpty()) ts = "1970-01-01 00:00:00";
  String safe = ssid; safe.replace("\"", "\"\"");
  char line[256];
  snprintf(line, sizeof(line), "%s,\"%s\",%s,%s,%d,%d,%d,%.6f,%.6f,%.0f,%.1f,WIFI,,",
           mac.c_str(), safe.c_str(), wigleAuth(auth),
           ts.c_str(), channel, channelToFreq(channel), rssi, lat, lon, altM, accuracy);
  logFile.println(line);
  maybeFlush();
}

void logBLERow(const String& mac, const String& name, int rssi,
               double lat, double lon, double altM, double accuracy,
               bool hasMfgr, uint16_t mfgrId) {
  String ts = gpsTimestamp();
  if (ts.isEmpty()) ts = "1970-01-01 00:00:00";
  String safe = name; safe.replace("\"", "\"\"");
  char mfgrField[8] = "";
  if (hasMfgr) snprintf(mfgrField, sizeof(mfgrField), "%u", mfgrId);
  char line[256];
  snprintf(line, sizeof(line), "%s,\"%s\",[BLE],%s,0,,%d,%.6f,%.6f,%.0f,%.1f,BLE,,%s",
           mac.c_str(), safe.c_str(), ts.c_str(), rssi, lat, lon, altM, accuracy, mfgrField);
  logFile.println(line);
  maybeFlush();
}
