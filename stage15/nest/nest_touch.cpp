#include "nest_touch.h"

// Uses HSPI remapped to TOUCH_CLK/MISO/MOSI pins.
// XPT2046 and TFT share the HSPI peripheral — safe because they are never
// accessed simultaneously (both live in loop() on Core 1).
static SPIClass touchSpi(HSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

void touchBegin() {
  touchSpi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSpi);
}

bool touchRead(int* px, int* py) {
  if (!ts.tirqTouched() || !ts.touched()) return false;
  TS_Point p = ts.getPoint();
  *px = constrain(map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, 239), 0, 239);
  *py = constrain(map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, 319), 0, 319);
  return true;
}
