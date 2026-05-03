#include "nest_touch.h"
#include <Arduino.h>
#include <Wire.h>

// Direct CST820 driver — no external library required.
// Register layout (shared with CST816S family):
//   0x01 Gesture   0x02 Fingers   0x03 X_high (low nibble)   0x04 X_low
//                                 0x05 Y_high (low nibble)   0x06 Y_low

static bool readBlock(uint8_t reg, uint8_t* buf, uint8_t n) {
  Wire.beginTransmission(CST820_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(CST820_ADDR, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

void touchBegin() {
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_RST, HIGH);
  delay(50);  // chip needs ~50 ms after reset before responding
  Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
}

bool touchRead(int* px, int* py) {
  uint8_t b[6];
  if (!readBlock(0x01, b, 6)) return false;
  uint8_t fingers = b[1];
  if (fingers == 0) return false;
  uint16_t x = ((b[2] & 0x0F) << 8) | b[3];
  uint16_t y = ((b[4] & 0x0F) << 8) | b[5];
  *px = constrain((int)x, 0, 239);
  *py = constrain((int)y, 0, 319);
  return true;
}

void touchDiag() {
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  if (now - lastMs < 1000) return;
  lastMs = now;

  uint8_t b[6];
  if (!readBlock(0x01, b, 6)) {
    Serial.println("[TDIAG] I2C read failed (no CST820 at 0x15?)");
    return;
  }
  uint8_t gesture = b[0];
  uint8_t fingers = b[1];
  uint16_t x = ((b[2] & 0x0F) << 8) | b[3];
  uint16_t y = ((b[4] & 0x0F) << 8) | b[5];
  if (fingers > 0)
    Serial.printf("[TDIAG] gesture=%02X fingers=%d  raw x=%d y=%d\n",
                  gesture, fingers, x, y);
  else
    Serial.printf("[TDIAG] gesture=%02X fingers=0 (idle)\n", gesture);
}
