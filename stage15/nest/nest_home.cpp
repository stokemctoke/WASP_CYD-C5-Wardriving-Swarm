#include "nest_home.h"
#include "nest_config.h"
#include "nest_led.h"
#include "nest_espnow.h"
#include "nest_registry.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <esp_wifi.h>
#include <Arduino.h>

char     lastWigleStr[32]    = "never";
char     lastWdgStr[32]      = "never";
uint32_t lastUploadAttemptMs = 0;
uint8_t  homeStatus          = 0;

extern bool sdOk;
static bool uploadRunning = false;

void restoreNestAP() {
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  WiFi.softAP(cfg.apSsid, cfg.apPsk, ESPNOW_CHANNEL);
  delay(100);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() == ESP_OK) esp_now_register_recv_cb(onDataRecv);
  delay(50);
  Serial.println("[HOME] AP + ESP-NOW restored");
}

static bool hasFilesToUpload() {
  File root = SD.open("/logs");
  if (!root) return false;
  bool found = false;
  while (!found) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) { entry.close(); continue; }
    String sub = "/logs/" + String(entry.name());
    entry.close();
    File dir = SD.open(sub.c_str());
    if (!dir) continue;
    while (true) {
      File f = dir.openNextFile();
      if (!f) break;
      String n = String(f.name());
      bool ok = n.endsWith(".csv") && !f.isDirectory() && f.size() > 0;
      f.close();
      if (ok) { found = true; break; }
    }
    dir.close();
  }
  root.close();
  return found;
}

static bool writeAll(WiFiClientSecure& wc, const uint8_t* data, size_t len, size_t& sentOut) {
  size_t offset = 0;
  uint32_t lastProgress = millis();
  while (offset < len) {
    if (!wc.connected()) return false;
    int w = wc.write(data + offset, len - offset);
    if (w > 0) {
      offset += w;
      sentOut += w;
      lastProgress = millis();
    } else if (w == 0) {
      if (millis() - lastProgress > 30000) return false;
      delay(10);
    } else {
      return false;
    }
  }
  return true;
}

static int streamMultipartPost(const char* host, const char* urlPath,
                                const char* authHeader, const char* authValue,
                                const String& filePath, const String& fileName) {
  File f = SD.open(filePath.c_str());
  if (!f) { Serial.println("[UPLOAD] SD open failed"); return -1; }
  int sz = (int)f.size();
  if (sz <= 0) { f.close(); Serial.println("[UPLOAD] empty file"); return -1; }
  f.close();

  const char* boundary = "WASPupload0123456789";
  String pre  = String("--") + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"file\"; filename=\"" + fileName + "\"\r\n"
                "Content-Type: text/csv\r\n\r\n";
  String post = String("\r\n--") + boundary + "--\r\n";
  int total   = (int)pre.length() + sz + (int)post.length();

  Serial.printf("[UPLOAD] %s%s  body=%d B (pre=%d + file=%d + post=%d)\n",
                host, urlPath, total, (int)pre.length(), sz, (int)post.length());
  Serial.printf("[UPLOAD] heap before TLS: %u\n", ESP.getFreeHeap());

  WiFiClientSecure wc; wc.setInsecure();
  wc.setTimeout(30);

  uint32_t tConnect = millis();
  if (!wc.connect(host, 443)) {
    Serial.printf("[UPLOAD] TLS connect to %s failed (lastError=%d)\n", host, wc.lastError(NULL, 0));
    return -1;
  }
  Serial.printf("[UPLOAD] TLS connected in %lu ms\n", (unsigned long)(millis() - tConnect));

  String hdr;
  hdr.reserve(256);
  hdr  = String("POST ") + urlPath + " HTTP/1.1\r\n";
  hdr += String("Host: ") + host + "\r\n";
  hdr += String(authHeader) + ": " + authValue + "\r\n";
  hdr += String("Content-Type: multipart/form-data; boundary=") + boundary + "\r\n";
  hdr += String("Content-Length: ") + total + "\r\n";
  hdr += "Accept: application/json\r\n";
  hdr += "Connection: close\r\n\r\n";

  size_t hdrBytes = 0, bodyBytes = 0;
  if (!writeAll(wc, (const uint8_t*)hdr.c_str(), hdr.length(), hdrBytes)) {
    Serial.println("[UPLOAD] header write failed"); wc.stop(); return -1;
  }
  if (!writeAll(wc, (const uint8_t*)pre.c_str(), pre.length(), bodyBytes)) {
    Serial.println("[UPLOAD] pre-boundary write failed"); wc.stop(); return -1;
  }

  File sf = SD.open(filePath.c_str());
  if (!sf) { wc.stop(); Serial.println("[UPLOAD] re-open failed"); return -1; }
  uint8_t buf[512];
  size_t  n;
  uint32_t bodyStart = millis();
  bool bodyOk = true;
  while ((n = sf.read(buf, sizeof(buf))) > 0) {
    if (!writeAll(wc, buf, n, bodyBytes)) { bodyOk = false; break; }
  }
  sf.close();
  if (!bodyOk) {
    Serial.printf("[UPLOAD] body write stalled after %u/%d B in %lu ms\n",
                  (unsigned)bodyBytes, total, (unsigned long)(millis() - bodyStart));
    wc.stop(); return -1;
  }
  if (!writeAll(wc, (const uint8_t*)post.c_str(), post.length(), bodyBytes)) {
    Serial.println("[UPLOAD] post-boundary write failed"); wc.stop(); return -1;
  }

  Serial.printf("[UPLOAD] sent header=%u B body=%u/%d B in %lu ms — waiting for response...\n",
                (unsigned)hdrBytes, (unsigned)bodyBytes, total,
                (unsigned long)(millis() - bodyStart));

  uint32_t respStart    = millis();
  uint32_t respDeadline = respStart + 30000UL;
  String   statusLine;
  enum { EXIT_GOT_LINE, EXIT_DISCONNECT, EXIT_TIMEOUT } exitReason = EXIT_TIMEOUT;
  while (millis() < respDeadline) {
    if (wc.available()) { statusLine = wc.readStringUntil('\n'); exitReason = EXIT_GOT_LINE; break; }
    if (!wc.connected()) { exitReason = EXIT_DISCONNECT; break; }
    delay(10);
  }
  statusLine.trim();
  unsigned long respMs = millis() - respStart;

  int code = 0;
  if (statusLine.startsWith("HTTP/")) {
    int sp = statusLine.indexOf(' ');
    if (sp > 0) code = statusLine.substring(sp + 1, sp + 4).toInt();
  } else {
    const char* why = (exitReason == EXIT_DISCONNECT) ? "server closed connection"
                    : (exitReason == EXIT_TIMEOUT)    ? "30 s timeout — no response"
                    :                                   "got data but not HTTP/";
    Serial.printf("[UPLOAD] no HTTP status line — %s (waited %lu ms, got: '%s')\n",
                  why, respMs, statusLine.c_str());
  }

  if (code != 200 && code != 201) {
    uint32_t drainDeadline = millis() + 4000;
    while (millis() < drainDeadline && wc.connected()) {
      String hline = wc.readStringUntil('\n'); hline.trim();
      if (hline.isEmpty()) break;
    }
    String errBody; errBody.reserve(256);
    drainDeadline = millis() + 2000;
    while (millis() < drainDeadline && wc.connected() && errBody.length() < 512) {
      while (wc.available() && errBody.length() < 512) errBody += (char)wc.read();
      delay(5);
    }
    if (errBody.length()) Serial.printf("[UPLOAD] error body: %s\n", errBody.c_str());
  }

  wc.stop();
  return code;
}

static bool uploadFileToWigle(const String& path, const String& fileName) {
  if (!cfg.wigleBasicToken[0]) return false;
  Serial.printf("[WIGLE] Uploading %s...\n", fileName.c_str());
  String auth = String("Basic ") + cfg.wigleBasicToken;
  int code = streamMultipartPost("api.wigle.net", "/api/v2/file/upload",
                                  "Authorization", auth.c_str(), path, fileName);
  Serial.printf("[WIGLE] %s  HTTP %d\n", fileName.c_str(), code);
  return (code == 200);
}

static bool uploadFileToWdgwars(const String& path, const String& fileName) {
  if (!cfg.wdgwarsApiKey[0]) return false;
  Serial.printf("[WDG] Uploading %s...\n", fileName.c_str());
  int code = streamMultipartPost("wdgwars.pl", "/api/upload-csv",
                                  "X-API-Key", cfg.wdgwarsApiKey, path, fileName);
  Serial.printf("[WDG] %s  HTTP %d\n", fileName.c_str(), code);
  return (code == 200);
}

static int buildMergedCsv(const String& outPath) {
  if (SD.exists(outPath.c_str())) SD.remove(outPath.c_str());
  File out = SD.open(outPath.c_str(), FILE_WRITE);
  if (!out) return 0;

  bool headerWritten = false;
  int  count = 0;
  uint8_t buf[512];

  File root = SD.open("/logs");
  if (!root) { out.close(); return 0; }
  while (true) {
    File macDir = root.openNextFile();
    if (!macDir) break;
    if (!macDir.isDirectory()) { macDir.close(); continue; }
    String macName = "/logs/" + String(macDir.name());
    macDir.close();
    File sub = SD.open(macName.c_str());
    if (!sub) continue;
    while (true) {
      File cf = sub.openNextFile();
      if (!cf) break;
      String name = String(cf.name());
      bool isCsv = name.endsWith(".csv") && !cf.isDirectory() && cf.size() > 0;
      cf.close();
      if (!isCsv) continue;
      File src = SD.open((macName + "/" + name).c_str());
      if (!src) continue;
      if (!headerWritten) {
        size_t n;
        while ((n = src.read(buf, sizeof(buf))) > 0) out.write(buf, n);
        headerWritten = true;
      } else {
        src.readStringUntil('\n');
        src.readStringUntil('\n');
        size_t n;
        while ((n = src.read(buf, sizeof(buf))) > 0) out.write(buf, n);
      }
      src.close();
      count++;
    }
    sub.close();
  }
  root.close();
  out.close();
  return count;
}

static void markAllCsvsDone() {
  File root = SD.open("/logs");
  if (!root) return;
  while (true) {
    File macDir = root.openNextFile();
    if (!macDir) break;
    if (!macDir.isDirectory()) { macDir.close(); continue; }
    String macName = "/logs/" + String(macDir.name());
    macDir.close();
    File sub = SD.open(macName.c_str());
    if (!sub) continue;
    while (true) {
      File cf = sub.openNextFile();
      if (!cf) break;
      String name = String(cf.name());
      bool isCsv = name.endsWith(".csv") && !cf.isDirectory();
      cf.close();
      if (!isCsv) continue;
      String fp = macName + "/" + name;
      SD.rename(fp.c_str(), (fp + ".done").c_str());
    }
    sub.close();
  }
  root.close();
}

void runHomeUploads() {
  if (uploadRunning || !sdOk) return;
  if (!cfg.homeSsid[0]) return;
  if (!cfg.wigleBasicToken[0] && !cfg.wdgwarsApiKey[0]) return;
  if (!hasFilesToUpload()) { Serial.println("[HOME] No files to upload"); return; }

  uploadRunning = true;
  nestLedFlashEvent(evNestUploadAct);

  const String mergePath = "/merge_tmp.csv";
  int mergedCount = buildMergedCsv(mergePath);
  if (mergedCount == 0) {
    Serial.println("[HOME] Merge produced no files — aborting");
    nestLedOff(); nestLedFlashEvent(evNestUploadFail);
    uploadRunning = false;
    return;
  }
  { File mf = SD.open(mergePath.c_str());
    Serial.printf("[HOME] Merged %d file(s) → %s (%d B)\n",
                  mergedCount, mergePath.c_str(), mf ? (int)mf.size() : 0);
    if (mf) mf.close(); }

  Serial.printf("[HOME] Connecting to %s ...", cfg.homeSsid);
  esp_now_deinit();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.homeSsid, cfg.homePsk);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < HOME_CONNECT_TIMEOUT_MS) {
    delay(200); Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HOME] Connect failed — restoring AP");
    nestLedOff(); nestLedFlashEvent(evNestUploadFail);
    homeStatus = 2;
    SD.remove(mergePath.c_str());
    WiFi.disconnect(true); delay(200);
    restoreNestAP();
    uploadRunning = false;
    return;
  }
  Serial.printf("[HOME] Connected  IP: %s\n", WiFi.localIP().toString().c_str());

  bool wOk = cfg.wigleBasicToken[0] ? uploadFileToWigle(mergePath,   "WASP_merged.csv") : true;
  bool dOk = cfg.wdgwarsApiKey[0]   ? uploadFileToWdgwars(mergePath, "WASP_merged.csv") : true;

  SD.remove(mergePath.c_str());

  taskENTER_CRITICAL(&gLock);
  if (cfg.wigleBasicToken[0])
    snprintf(lastWigleStr, sizeof(lastWigleStr), wOk ? "%d files OK" : "FAIL", mergedCount);
  if (cfg.wdgwarsApiKey[0])
    snprintf(lastWdgStr,   sizeof(lastWdgStr),   dOk ? "%d files OK" : "FAIL", mergedCount);
  taskEXIT_CRITICAL(&gLock);

  Serial.printf("[HOME] Done — WiGLE %s  WDGWars %s\n",
                wOk ? "OK" : "FAIL", dOk ? "OK" : "FAIL");

  nestLedOff();
  if (wOk && dOk) {
    markAllCsvsDone();
    nestLedFlashEvent(evNestUploadOK);
    homeStatus = 1;
  } else {
    nestLedFlashEvent(evNestUploadFail);
    homeStatus = 2;
  }

  WiFi.disconnect(true); delay(200);
  restoreNestAP();
  uploadRunning = false;
}
