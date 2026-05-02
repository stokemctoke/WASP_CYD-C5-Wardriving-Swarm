#include "worker_sync.h"
#include "worker_gps.h"
#include "worker_led.h"
#include "worker_storage.h"
#include "worker_drone.h"
#include "worker_espnow.h"
#include "worker_config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD.h>

bool sdOk = false;

bool hasPendingFiles() {
  File dir = SD.open("/logs");
  if (!dir) return false;
  bool found = false;
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;
    bool isDir = entry.isDirectory();
    String name = String(entry.name());
    size_t sz = entry.size();
    entry.close();
    if (!isDir && name.endsWith(".csv") && sz > 0) { found = true; break; }
  }
  dir.close();
  return found;
}

bool connectToNest() {
  sendHeartbeat();
  delay(50);
  esp_now_deinit();
  WiFi.mode(WIFI_STA);
  WiFi.begin(nestSsid, nestPsk);
  unsigned long t0 = millis();
  unsigned long lastLedMs = 0;
  bool ledOn = false;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000) {
    if (gpsOk) while (gpsSerial.available()) gps.encode(gpsSerial.read());
    if (millis() - lastLedMs >= (unsigned long)evConnecting.onMs) {
      lastLedMs = millis();
      ledOn = !ledOn;
      ledOn ? ledSet(evConnecting.colour) : ledOff();
    }
    delay(evConnecting.onMs); Serial.print(".");
  }
  ledOff();
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

void disconnectFromNest() {
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  initEspNow();
  delay(50);
  sendHeartbeat();
}

bool uploadFileChunked(const String& path, const String& name, int fileSize) {
  int totalChunks = (fileSize + maxLogBytes - 1) / maxLogBytes;
  Serial.printf("[SYNC]  chunked: %d chunk(s) × up to %d B\n", totalChunks, maxLogBytes);

  uint8_t* buf = (uint8_t*)malloc(maxLogBytes);
  if (!buf) {
    Serial.printf("[OOM for %d B chunk buf]\n", maxLogBytes);
    return false;
  }

  bool allOk = true;
  for (int ci = 0; ci < totalChunks && allOk; ci++) {
    int offset = ci * maxLogBytes;
    int chunkSize = min(fileSize - offset, maxLogBytes);

    {
      File f = SD.open(path);
      if (!f) {
        Serial.printf("[SD open failed chunk %d/%d]\n", ci + 1, totalChunks);
        allOk = false; break;
      }
      if (!f.seek(offset)) {
        Serial.printf("[SD seek failed chunk %d/%d]\n", ci + 1, totalChunks);
        f.close(); allOk = false; break;
      }
      int got = f.read(buf, chunkSize);
      f.close();
      if (got != chunkSize) {
        Serial.printf("[SD short read chunk %d/%d: %d/%d B]\n",
                      ci + 1, totalChunks, got, chunkSize);
        allOk = false; break;
      }
    }

    Serial.printf("         chunk %d/%d  %d B  ...", ci + 1, totalChunks, chunkSize);

    String myMac = WiFi.macAddress(); myMac.replace(":", "");

    WiFiClient tcp;
    bool chunkOk = false;
    if (tcp.connect(nestIp, NEST_UPLOAD_PORT)) {
      tcp.setNoDelay(true);
      tcp.setTimeout(5000);
      tcp.printf("UPLOAD_CHUNK %s %s %d %d %d\n",
                 myMac.c_str(), name.c_str(), ci, totalChunks, chunkSize);
      tcp.flush();
      String ready = tcp.readStringUntil('\n'); ready.trim();
      if (ready == "READY") {
        const uint8_t* ptr = buf;
        int sent = 0, rem = chunkSize;
        uint32_t deadline = millis() + 15000;
        while (rem > 0 && tcp.connected() && millis() < deadline) {
          int w = tcp.write(ptr, min(rem, 1460));
          if (w > 0) { ptr += w; sent += w; rem -= w; }
          else if (w < 0) break;
          else delay(1);
        }
        tcp.flush();
        String resp = tcp.readStringUntil('\n'); resp.trim();
        Serial.printf("[%s]", resp.c_str());
        chunkOk = (resp == "OK") && (sent == chunkSize);
      } else {
        Serial.printf("[bad READY: %s]", ready.c_str());
      }
    } else {
      Serial.print("[tcp FAIL]");
    }
    tcp.stop();
    Serial.println(chunkOk ? " OK" : " FAILED");
    if (!chunkOk) allOk = false;
    if (ci < totalChunks - 1) delay(50);
  }

  free(buf);
  return allOk;
}

void syncFiles() {
  Serial.println("\n[SYNC] Starting...");
  if (logFile) { logFile.flush(); logFile.close(); }

  int uploaded = 0, failed = 0;

  {
    File dir = SD.open("/logs");
    if (dir) {
      while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        String n = String(entry.name());
        bool d = entry.isDirectory();
        entry.close();
        if (!d && n.endsWith(".csv.toobig")) {
          String op = "/logs/" + n;
          String np = op.substring(0, op.length() - 8);
          SD.rename(op.c_str(), np.c_str());
          Serial.printf("[SYNC]  Queued for chunked retry: %s\n", np.c_str());
        }
      }
      dir.close();
    }
  }

  const int MAX_QUEUE = 100;
  String queueName[MAX_QUEUE];
  int queueSize[MAX_QUEUE];
  int queueLen = 0;
  {
    File dir = SD.open("/logs");
    if (dir) {
      while (queueLen < MAX_QUEUE) {
        File entry = dir.openNextFile();
        if (!entry) break;
        String n = String(entry.name());
        bool d = entry.isDirectory();
        int s = (int)entry.size();
        entry.close();
        if (!d && n.endsWith(".csv") && s > 0) {
          queueName[queueLen] = n;
          queueSize[queueLen] = s;
          queueLen++;
        }
      }
      dir.close();
    }
  }
  Serial.printf("[SYNC] %d file(s) queued\n", queueLen);

  bool nestAttempted = false;
  if (queueLen > 0) {
    nestAttempted = true;
    if (!connectToNest()) {
      Serial.println("[SYNC] Nest WiFi unreachable — aborting sync");
      queueLen = 0;
    } else {
      Serial.printf("[SYNC] Connected  IP: %s\n", WiFi.localIP().toString().c_str());
    }
  }

  for (int qi = 0; qi < queueLen; qi++) {
    String name = queueName[qi];
    int sz = queueSize[qi];
    String path = name.startsWith("/") ? name : "/logs/" + name;

    if (sz > maxLogBytes) {
      Serial.printf("[SYNC]  %-32s  %5d B\n", name.c_str(), sz);
      bool ok = uploadFileChunked(path, name, sz);
      if (ok) {
        SD.rename(path.c_str(), (path + ".done").c_str());
        Serial.printf("[SYNC]  %s → .done\n", name.c_str()); uploaded++;
      } else {
        ledTooBig();
        SD.rename(path.c_str(), (path + ".defer").c_str());
        Serial.printf("[SYNC]  %s → .defer\n", name.c_str()); failed++;
      }
      delay(50);
      continue;
    }

    Serial.printf("[SYNC]  %-32s  %5d B  ...", name.c_str(), sz);

    uint8_t* fileBuf = (uint8_t*)malloc(sz);
    if (!fileBuf) {
      Serial.printf("[OOM %dB] — deferred\n", sz);
      SD.rename(path.c_str(), (path + ".defer").c_str());
      failed++;
      continue;
    }
    {
      File f = SD.open(path);
      if (!f) {
        Serial.println("[SD open failed] — deferred");
        free(fileBuf);
        SD.rename(path.c_str(), (path + ".defer").c_str());
        failed++;
        continue;
      }
      int got = f.read(fileBuf, sz);
      f.close();
      if (got != sz) {
        Serial.printf("[SD short read %d/%d] — deferred\n", got, sz);
        free(fileBuf);
        SD.rename(path.c_str(), (path + ".defer").c_str());
        failed++;
        continue;
      }
    }

    bool ok = false;
    int sent = 0;
    bool tcpFail = false;
    {
      String myMac = WiFi.macAddress(); myMac.replace(":", "");
      WiFiClient tcp;
      Serial.print(" tcp..");
      if (tcp.connect(nestIp, NEST_UPLOAD_PORT)) {
        tcp.setNoDelay(true);
        tcp.setTimeout(5000);
        Serial.print("OK hdr..");
        tcp.printf("UPLOAD %s %s %d\n", myMac.c_str(), name.c_str(), sz);
        tcp.flush();
        Serial.print("rdy..");
        String ready = tcp.readStringUntil('\n'); ready.trim();
        if (ready == "READY") {
          Serial.print("OK data..");
          const uint8_t* ptr = fileBuf;
          int rem = sz;
          uint32_t wDeadline = millis() + 30000;
          while (rem > 0 && tcp.connected() && millis() < wDeadline) {
            int toSend = min(rem, 1460);
            int w = tcp.write(ptr, toSend);
            if (w > 0) { ptr += w; sent += w; rem -= w; }
            else if (w < 0) break;
            else delay(1);
          }
          tcp.flush();
          Serial.printf("%dB resp..", sent);
          String resp = tcp.readStringUntil('\n'); resp.trim();
          Serial.printf("[%s]", resp.c_str());
          ok = (resp == "OK") && (sent == sz);
        } else {
          Serial.printf("[nest READY? got: %s]", ready.c_str());
        }
      } else {
        Serial.print("FAIL");
        tcpFail = true;
      }
      tcp.stop();
    }
    free(fileBuf);

    if (ok) {
      SD.rename(path.c_str(), (path + ".done").c_str());
      Serial.printf(" OK\n"); uploaded++;
    } else if (tcpFail) {
      Serial.println("\n[SYNC] Nest TCP not reachable — stopping");
      break;
    } else {
      Serial.printf(" FAILED (%d/%d B) — deferred\n", sent, sz);
      SD.rename(path.c_str(), (path + ".defer").c_str());
      failed++;
    }
    delay(50);
  }

  if (nestAttempted) disconnectFromNest();

  {
    File dir = SD.open("/logs");
    if (dir) {
      while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        String n = String(entry.name());
        bool d = entry.isDirectory();
        entry.close();
        if (!d && n.endsWith(".csv.defer")) {
          String op = "/logs/" + n;
          SD.rename(op.c_str(), op.substring(0, op.length() - 6).c_str());
        }
      }
      dir.close();
    }
  }

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  SD.begin(SD_CS, SPI);

  Serial.printf("[SYNC] Done — %d uploaded, %d failed\n", uploaded, failed);

  if (uploaded > 0 && failed == 0) ledSyncOK();
  else if (failed > 0) ledSyncFail();

  if (sdOk) openLogFile();
}

void syncBuffer() {
  int pending = countUnuploaded();
  if (pending == 0) { Serial.println("[SYNC] Buffer empty — nothing to send"); return; }

  Serial.printf("\n[SYNC] Connecting to Nest AP (%d slot(s) pending)...\n", pending);

  if (!connectToNest()) {
    Serial.println("[SYNC] Nest not reachable — buffer retained");
    ledSyncFail();
    disconnectFromNest();
    return;
  }

  Serial.printf("[SYNC] Connected  IP: %s\n", WiFi.localIP().toString().c_str());
  String myMac = WiFi.macAddress(); myMac.replace(":", "");
  int uploaded = 0, failed = 0;

  for (int i = 0; i < CYCLE_SLOTS; i++) {
    if (!cycleBuffer[i].used || cycleBuffer[i].uploaded) continue;
    char fileName[40];
    snprintf(fileName, sizeof(fileName), "DRONE_%lu_%02d.csv",
             (unsigned long)cycleBuffer[i].capturedMs, i);

    String csv = buildCSV(i);
    Serial.printf("[SYNC]  slot %2d  %-28s  %5d B  ...", i, fileName, csv.length());

    HTTPClient http;
    http.begin("http://" + String(nestIp) + "/upload?worker=" + myMac + "&file=" + fileName);
    http.addHeader("Content-Type", "text/csv");
    int code = http.POST(csv);
    http.end();

    if (code == 200) { cycleBuffer[i].uploaded = true; Serial.println(" OK"); uploaded++; }
    else { Serial.printf(" FAILED (HTTP %d)\n", code); failed++; }
  }
  Serial.printf("[SYNC] Done — %d uploaded, %d failed\n", uploaded, failed);

  if (uploaded > 0 && failed == 0) ledSyncOK();
  else if (failed > 0) ledSyncFail();

  disconnectFromNest();
  ledDronePulse();
}
