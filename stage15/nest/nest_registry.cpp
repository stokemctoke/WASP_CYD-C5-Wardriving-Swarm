#include "nest_registry.h"
#include <Arduino.h>

worker_entry_t workers[MAX_WORKERS] = {};
portMUX_TYPE   gLock = portMUX_INITIALIZER_UNLOCKED;

int findWorker(const uint8_t* mac) {
  for (int i = 0; i < MAX_WORKERS; i++)
    if (memcmp(workers[i].mac, mac, 6) == 0) return i;
  return -1;
}

int findOrAddWorker(const uint8_t* mac) {
  int idx = findWorker(mac);
  if (idx >= 0) return idx;
  for (int i = 0; i < MAX_WORKERS; i++)
    if (workers[i].lastSeenMs == 0) { memcpy(workers[i].mac, mac, 6); return i; }
  return -1;
}

int countActiveWorkers() {
  int n = 0;
  uint32_t now = millis();
  for (int i = 0; i < MAX_WORKERS; i++)
    if (workers[i].lastSeenMs > 0 && (now - workers[i].lastSeenMs) < WORKER_TIMEOUT_MS) n++;
  return n;
}

void cleanRegistry() {
  uint32_t now = millis();
  for (int i = 0; i < MAX_WORKERS; i++) {
    taskENTER_CRITICAL(&gLock);
    bool expired = workers[i].lastSeenMs > 0 &&
                   (now - workers[i].lastSeenMs) >= WORKER_REMOVE_MS;
    uint8_t macB[6];
    if (expired) { memcpy(macB, workers[i].mac, 6); memset(&workers[i], 0, sizeof(workers[i])); }
    taskEXIT_CRITICAL(&gLock);
    if (!expired) continue;
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             macB[0], macB[1], macB[2], macB[3], macB[4], macB[5]);
    Serial.printf("[NEST] Worker %s expired — slot freed\n", mac);
  }
}
