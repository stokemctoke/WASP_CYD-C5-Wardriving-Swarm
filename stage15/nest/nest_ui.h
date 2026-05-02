#pragma once
#include "nest_types.h"

#define UI_STACK_MAX 6

extern ScreenId uiStack[UI_STACK_MAX];
extern int      uiStackDepth;
extern char     uiDetailMac[18];    // formatted "AA:BB:CC:DD:EE:FF" for detail screens
extern char     uiDetailMac12[13];  // bare 12-char hex for SD path lookups

ScreenId uiCurrent();
void     uiPush(ScreenId s);
void     uiPop();
void     uiFadeOut();
void     uiFadeIn();
void     uiTransitionTo(ScreenId s);
void     uiBack();
bool     uiBackHit(int px, int py);
void     handleTouch();
