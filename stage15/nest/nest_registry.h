#pragma once

#include "nest_types.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern worker_entry_t workers[MAX_WORKERS];
extern portMUX_TYPE   gLock;

int  findWorker(const uint8_t* mac);
int  findOrAddWorker(const uint8_t* mac);
int  countActiveWorkers();
void cleanRegistry();
