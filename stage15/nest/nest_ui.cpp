#include "nest_ui.h"
#include "nest_display.h"
#include "nest_touch.h"
#include <Arduino.h>

ScreenId uiStack[UI_STACK_MAX] = { SCR_HOME };
int      uiStackDepth  = 1;
char     uiDetailMac[18]    = {};
char     uiDetailMac12[13]  = {};

ScreenId uiCurrent() { return uiStack[uiStackDepth - 1]; }

void uiPush(ScreenId s) {
  if (uiStackDepth < UI_STACK_MAX) uiStack[uiStackDepth++] = s;
}

void uiPop() { if (uiStackDepth > 1) uiStackDepth--; }

void uiFadeOut() {
  for (int b = 255; b >= 0; b -= 15) { ledcWrite(BACKLIGHT_CH, b); delay(2); }
  ledcWrite(BACKLIGHT_CH, 0);
}

void uiFadeIn() {
  for (int b = 0; b <= 255; b += 15) { ledcWrite(BACKLIGHT_CH, b); delay(2); }
  ledcWrite(BACKLIGHT_CH, 255);
}

void uiTransitionTo(ScreenId s) {
  uiFadeOut();
  uiPush(s);
  refreshDisplay();
  uiFadeIn();
}

void uiBack() {
  if (uiStackDepth <= 1) return;
  uiFadeOut();
  uiPop();
  refreshDisplay();
  uiFadeIn();
}

bool uiBackHit(int px, int py) {
  return px < BACK_BTN_W && py < BACK_BTN_H;
}

void handleTouch() {
  static bool     wasDown = false;
  static int      lastPx  = 0, lastPy = 0;
  static uint32_t downMs  = 0;

  int  px, py;
  bool isDown = touchRead(&px, &py);
  if (isDown) { lastPx = px; lastPy = py; }
  if (isDown && !wasDown) downMs = millis();
  if (!isDown && wasDown && millis() - downMs >= 50) {
    dispatchTap(lastPx, lastPy);
  }
  wasDown = isDown;
}
