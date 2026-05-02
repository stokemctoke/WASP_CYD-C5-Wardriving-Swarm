#include "worker_gps.h"
#include <time.h>
#include <sys/time.h>

// ── GPS state ─────────────────────────────────────────────────────────────────
bool gpsOk = false;
bool clockSet = false;
HardwareSerial gpsSerial(1);
TinyGPSPlus gps;

// ── GPS helpers ───────────────────────────────────────────────────────────────

void feedGPS(unsigned long ms) {
  unsigned long deadline = millis() + ms;
  while (millis() < deadline) {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    delay(1);
  }
}

bool detectGPS() {
  extern LedEvent evGPSAcquire;
  extern void ledSet(uint32_t colour);
  extern void ledOff();
  Serial.print(" Detecting GPS ");
  unsigned long start      = millis();
  unsigned long lastToggle = 0;
  bool          ledOn      = false;
  while (millis() - start < GPS_DETECT_MS) {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    if (millis() - lastToggle >= (unsigned long)evGPSAcquire.onMs) {
      lastToggle = millis();
      ledOn = !ledOn;
      ledOn ? ledSet(evGPSAcquire.colour) : ledOff();
    }
    Serial.print(".");
    delay(100);
  }
  ledOff();
  Serial.println();
  return gps.charsProcessed() > 0;
}

void printGPSStatus() {
  if (!gpsOk) { Serial.println("[WORKER] GPS  not present"); return; }
  if (gps.location.isValid()) {
    Serial.printf("[WORKER] GPS  %.6f, %.6f | alt %.1fm | sats %d | hdop %.2f\n",
                  gps.location.lat(), gps.location.lng(),
                  gps.altitude.isValid()   ? gps.altitude.meters()       : 0.0,
                  gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                  gps.hdop.isValid()       ? gps.hdop.hdop()             : 99.99);
  } else {
    Serial.printf("[WORKER] GPS  NO FIX (sats seen: %d)\n",
                  gps.satellites.isValid() ? (int)gps.satellites.value() : 0);
  }
}

String gpsTimestamp() {
  if (!gps.date.isValid() || !gps.time.isValid()) return "";
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           gps.date.year(), gps.date.month(), gps.date.day(),
           gps.time.hour(), gps.time.minute(), gps.time.second());
  return String(buf);
}

String nowTimestamp() {
  time_t now = time(nullptr);
  if (now < 1000000000UL) return "1970-01-01 00:00:00";
  struct tm* t = gmtime(&now);
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);
  return String(buf);
}

void setClockFromGPS() {
  if (clockSet || !gps.date.isValid() || !gps.time.isValid()) return;
  if (gps.date.year() < 2020) return;
  struct tm t = {};
  t.tm_year = gps.date.year() - 1900;
  t.tm_mon  = gps.date.month() - 1;
  t.tm_mday = gps.date.day();
  t.tm_hour = gps.time.hour();
  t.tm_min  = gps.time.minute();
  t.tm_sec  = gps.time.second();
  time_t epoch = mktime(&t);
  struct timeval tv = { epoch, 0 };
  settimeofday(&tv, nullptr);
  clockSet = true;
  Serial.printf("[GPS] Clock set: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                gps.date.year(), gps.date.month(), gps.date.day(),
                gps.time.hour(), gps.time.minute(), gps.time.second());
}
