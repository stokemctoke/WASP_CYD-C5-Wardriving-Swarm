#pragma once

#include "worker_types.h"
#include <TinyGPS++.h>
#include <HardwareSerial.h>
#include <WString.h>

extern bool gpsOk;
extern bool clockSet;
extern HardwareSerial gpsSerial;
extern TinyGPSPlus gps;

void feedGPS(unsigned long ms);
bool detectGPS();
void printGPSStatus();
String gpsTimestamp();
String nowTimestamp();
void setClockFromGPS();
