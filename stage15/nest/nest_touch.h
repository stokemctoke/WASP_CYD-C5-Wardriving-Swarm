#pragma once
#include "nest_types.h"
#include <XPT2046_Touchscreen.h>
#include <SPI.h>

// Calibration — CYD rotation 0 (portrait, USB-C bottom).
// If taps feel offset, adjust these four values after hardware testing.
#define TOUCH_X_MIN   300
#define TOUCH_X_MAX  3800
#define TOUCH_Y_MIN   200
#define TOUCH_Y_MAX  3900

extern XPT2046_Touchscreen ts;

void touchBegin();
bool touchRead(int* px, int* py);  // true while touched; fills calibrated pixel coords
