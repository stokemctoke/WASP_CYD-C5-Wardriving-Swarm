#pragma once

#include "nest_types.h"
#include <esp_now.h>

void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len);
