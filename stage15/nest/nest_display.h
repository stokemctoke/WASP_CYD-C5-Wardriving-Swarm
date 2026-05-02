#pragma once

#include "nest_types.h"
#include <TFT_eSPI.h>

extern TFT_eSPI tft;

void drawBootMsg(const char* msg);
void drawHeader();
void refreshDisplay();
