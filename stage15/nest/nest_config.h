#pragma once

#include "nest_types.h"

extern wasp_config_t cfg;

bool loadConfig();
bool parseNestLedEvent(const String& val, LedEvent& ev);
