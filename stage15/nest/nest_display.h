#pragma once

#include "nest_types.h"
#include <TFT_eSPI.h>

extern TFT_eSPI tft;

// Boot helpers
void drawBootMsg(const char* msg);
void drawHeader();

// Main dispatcher — called from loop() every second
void refreshDisplay();
// Touch dispatcher — called from handleTouch() in nest_ui
void dispatchTap(int px, int py);

// Per-screen renderers
void drawHome();
void drawWorkerDetail();
void drawFileBrowser();
void drawFileList();
void drawSettings();

// Per-screen tap handlers
void handleTapHome(int px, int py);
void handleTapWorkerDetail(int px, int py);
void handleTapFileBrowser(int px, int py);
void handleTapFileList(int px, int py);
void handleTapSettings(int px, int py);

// Notify display module that file browser/list cache should refresh
void uiInvalidateBrowser();
void uiInvalidateFileList();
