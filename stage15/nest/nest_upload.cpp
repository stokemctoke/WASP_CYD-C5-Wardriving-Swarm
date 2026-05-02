#include "nest_upload.h"
#include "nest_registry.h"
#include "nest_led.h"
#include <SD.h>
#include <WiFi.h>
#include <Arduino.h>

WebServer  server(80);
WiFiServer rawServer(8080);
char       lastSyncStr[48] = "none";

extern bool sdOk;

bool isValidMac(const String& mac) {
  if (mac.length() != 12) return false;
  for (unsigned int i = 0; i < mac.length(); i++) {
    char c = mac[i];
    if (!((c >= '0' && c <= '9') ||
          (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

bool isValidFilename(const String& name) {
  if (name.isEmpty() || name.length() > 64) return false;
  if (!name.endsWith(".csv")) return false;
  if (name.indexOf("..") >= 0) return false;
  for (unsigned int i = 0; i < name.length(); i++) {
    char c = name[i];
    if (!isAlphaNumeric(c) && c != '_' && c != '-' && c != '.') return false;
  }
  return true;
}

void handleRawUpload() {
  WiFiClient client = rawServer.accept();
  if (!client) return;

  client.setTimeout(15000);
  String hdr = client.readStringUntil('\n');
  hdr.trim();

  bool isChunked = hdr.startsWith("UPLOAD_CHUNK ");
  bool isSingle  = !isChunked && hdr.startsWith("UPLOAD ");
  if (!isSingle && !isChunked) {
    client.println("ERR bad header");
    client.stop();
    return;
  }

  String workerMac, fileName;
  int fileSize = 0, chunkIndex = 0, totalChunks = 1;

  if (isSingle) {
    int s1 = hdr.indexOf(' ');
    int s2 = hdr.indexOf(' ', s1 + 1);
    int s3 = hdr.indexOf(' ', s2 + 1);
    if (s3 < 0) { client.println("ERR bad header"); client.stop(); return; }
    workerMac = hdr.substring(s1 + 1, s2);
    fileName  = hdr.substring(s2 + 1, s3);
    fileSize  = hdr.substring(s3 + 1).toInt();
  } else {
    int s1 = hdr.indexOf(' ');
    int s2 = hdr.indexOf(' ', s1 + 1);
    int s3 = hdr.indexOf(' ', s2 + 1);
    int s4 = hdr.indexOf(' ', s3 + 1);
    int s5 = hdr.indexOf(' ', s4 + 1);
    if (s5 < 0) { client.println("ERR bad header"); client.stop(); return; }
    workerMac   = hdr.substring(s1 + 1, s2);
    fileName    = hdr.substring(s2 + 1, s3);
    chunkIndex  = hdr.substring(s3 + 1, s4).toInt();
    totalChunks = hdr.substring(s4 + 1, s5).toInt();
    fileSize    = hdr.substring(s5 + 1).toInt();
  }

  if (!sdOk || fileSize <= 0) {
    client.println("ERR bad params");
    client.stop();
    return;
  }
  if (!isValidMac(workerMac) || !isValidFilename(fileName)) {
    Serial.printf("[NEST] Rejected upload: mac='%s' file='%s' (validation failed)\n",
                  workerMac.c_str(), fileName.c_str());
    client.println("ERR bad params");
    client.stop();
    return;
  }

  String dir  = "/logs/" + workerMac;
  String path = dir + "/" + fileName;
  if (!SD.exists("/logs")) SD.mkdir("/logs");
  if (!SD.exists(dir))     SD.mkdir(dir);

  File f;
  if (chunkIndex == 0) {
    if (SD.exists(path.c_str())) SD.remove(path.c_str());
    f = SD.open(path.c_str(), FILE_WRITE);
  } else {
    f = SD.open(path.c_str(), FILE_APPEND);
    if (!f) f = SD.open(path.c_str(), FILE_WRITE);
  }
  if (!f) { client.println("ERR sd open failed"); client.stop(); return; }

  client.println("READY");

  static uint8_t buf[4096];
  int      remaining = fileSize;
  size_t   written   = 0;
  uint32_t deadline  = millis() + 30000;
  bool     sdFail    = false;
  while (remaining > 0 && client.connected() && millis() < deadline) {
    int n = client.read(buf, min(remaining, (int)sizeof(buf)));
    if (n > 0) {
      size_t wrote = f.write(buf, n);
      written += wrote;
      if ((int)wrote < n) {
        Serial.printf("[NEST] SD write short %u/%d — aborting\n", (unsigned)wrote, n);
        sdFail = true;
        break;
      }
      remaining -= n;
    } else {
      yield();
    }
  }
  f.close();

  if (sdFail) {
    SD.remove(path.c_str());
    client.println("ERR sd write failed");
    client.stop();
    return;
  }

  Serial.printf("[NEST] recv %u/%d B for %s\n", (unsigned)written, fileSize, fileName.c_str());

  if ((int)written < fileSize) {
    client.println("ERR transfer incomplete");
    client.stop();
    SD.remove(path.c_str());
    return;
  }

  client.println("OK");
  client.stop();

  nestLedFlashEvent(evNestChunk);

  Serial.printf("[NEST] Saved %s chunk %d/%d (%u bytes)\n",
                fileName.c_str(), chunkIndex + 1, totalChunks, (unsigned)written);
  if (chunkIndex == totalChunks - 1) {
    taskENTER_CRITICAL(&gLock);
    snprintf(lastSyncStr, sizeof(lastSyncStr), "%s  %dB", workerMac.c_str(), (int)written);
    taskEXIT_CRITICAL(&gLock);
    Serial.printf("[NEST] Complete: %s\n", path.c_str());
  }
}

void handleUpload() {
  if (!sdOk) { server.send(503, "text/plain", "SD not ready"); return; }

  String workerMac = server.arg("worker");
  String fileName  = server.arg("file");

  if (!isValidMac(workerMac))     { server.send(400, "text/plain", "Bad worker");   return; }
  if (!isValidFilename(fileName)) { server.send(400, "text/plain", "Bad filename"); return; }

  String dir  = "/logs/" + workerMac;
  String path = dir + "/" + fileName;

  if (!SD.exists("/logs")) SD.mkdir("/logs");
  if (!SD.exists(dir))     SD.mkdir(dir);

  String body = server.arg("plain");
  if (SD.exists(path.c_str())) SD.remove(path.c_str());
  File f = SD.open(path.c_str(), FILE_WRITE);
  if (!f) { server.send(500, "text/plain", "SD open failed"); return; }
  size_t wrote = f.print(body);
  f.close();
  if (wrote < body.length()) {
    SD.remove(path.c_str());
    Serial.printf("[NEST] SD write short %u/%u for %s\n",
                  (unsigned)wrote, (unsigned)body.length(), path.c_str());
    server.send(500, "text/plain", "SD write failed");
    return;
  }

  taskENTER_CRITICAL(&gLock);
  snprintf(lastSyncStr, sizeof(lastSyncStr), "%s  %dB", workerMac.c_str(), body.length());
  taskEXIT_CRITICAL(&gLock);
  Serial.printf("[NEST] Saved %s (%d bytes)\n", path.c_str(), body.length());
  server.send(200, "text/plain", "OK");
}
