#pragma once

#include "worker_types.h"
#include <WString.h>

extern bool sdOk;

bool hasPendingFiles();
bool connectToNest();
void disconnectFromNest();
bool uploadFileChunked(const String& path, const String& name, int fileSize);
void syncFiles();
void syncBuffer();
