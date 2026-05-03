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
  for (int b = 255; b >= 0; b -= 15) { ledcWrite(TFT_BACKLIGHT, b); delay(2); }
  ledcWrite(TFT_BACKLIGHT, 0);
}

void uiFadeIn() {
  for (int b = 0; b <= 255; b += 15) { ledcWrite(TFT_BACKLIGHT, b); delay(2); }
  ledcWrite(TFT_BACKLIGHT, 255);
}

void uiTransitionTo(ScreenId s) {
  uiFadeOut();
  uiPush(s);
  drawCurrentScreen();
  uiFadeIn();
}

void uiBack() {
  if (uiStackDepth <= 1) return;
  uiFadeOut();
  uiPop();
  drawCurrentScreen();
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
  if (isDown && !wasDown) {
    downMs = millis();
    Serial.printf("[TOUCH] down @ %d,%d  screen=%d\n", px, py, (int)uiCurrent());
  }
  if (!isDown && wasDown && millis() - downMs >= 50) {
    Serial.printf("[TOUCH] tap  @ %d,%d  screen=%d\n", lastPx, lastPy, (int)uiCurrent());
    dispatchTap(lastPx, lastPy);
  }
  wasDown = isDown;
}
