#pragma once
#include "nest_types.h"

void touchBegin();
bool touchRead(int* px, int* py);  // true while touched; fills calibrated pixel coords
void touchDiag();                  // 1Hz raw-state print, call every loop iteration
