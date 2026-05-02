#include "nest_config.h"
#include "nest_led.h"
#include <SD.h>

wasp_config_t cfg = {};

bool parseNestLedEvent(const String& val, LedEvent& ev) {
  int c1 = val.indexOf(',');
  int c2 = val.indexOf(',', c1 + 1);
  int c3 = val.indexOf(',', c2 + 1);
  if (c1 < 0 || c2 < 0 || c3 < 0) return false;
  ev.colour  = (uint32_t)strtoul(val.substring(0, c1).c_str(), nullptr, 16);
  ev.flashes = val.substring(c1 + 1, c2).toInt();
  ev.onMs    = val.substring(c2 + 1, c3).toInt();
  ev.offMs   = val.substring(c3 + 1).toInt();
  return true;
}

bool loadConfig() {
  strlcpy(cfg.apSsid, "WASP-Nest", sizeof(cfg.apSsid));
  strlcpy(cfg.apPsk,  "waspswarm", sizeof(cfg.apPsk));

  if (SD.exists("/reset.cfg")) {
    Serial.println("[CFG] /reset.cfg found — using compiled defaults, wasp.cfg ignored");
    return false;
  }

  File f = SD.open("/wasp.cfg");
  if (!f) {
    Serial.println("[CFG] /wasp.cfg not found — using defaults");
    return false;
  }

  int loaded = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) continue;

    int eq = line.indexOf('=');
    if (eq < 1) continue;

    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    key.trim();
    val.trim();

    if      (key == "homeSsid")        { val.toCharArray(cfg.homeSsid,        sizeof(cfg.homeSsid));        loaded++; }
    else if (key == "homePsk")         { val.toCharArray(cfg.homePsk,         sizeof(cfg.homePsk));         loaded++; }
    else if (key == "apSsid")          { val.toCharArray(cfg.apSsid,          sizeof(cfg.apSsid));          loaded++; }
    else if (key == "apPsk")           { val.toCharArray(cfg.apPsk,           sizeof(cfg.apPsk));           loaded++; }
    else if (key == "wigleBasicToken") { val.toCharArray(cfg.wigleBasicToken, sizeof(cfg.wigleBasicToken)); loaded++; }
    else if (key == "wdgwarsApiKey")   { val.toCharArray(cfg.wdgwarsApiKey,   sizeof(cfg.wdgwarsApiKey));   loaded++; }
    else if (key == "nestLedBoot")        parseNestLedEvent(val, evNestBoot);
    else if (key == "nestLedHeartbeat")   parseNestLedEvent(val, evNestHeartbeat);
    else if (key == "nestLedChunk")       parseNestLedEvent(val, evNestChunk);
    else if (key == "nestLedUploadAct")   parseNestLedEvent(val, evNestUploadAct);
    else if (key == "nestLedUploadOK")    parseNestLedEvent(val, evNestUploadOK);
    else if (key == "nestLedUploadFail")  parseNestLedEvent(val, evNestUploadFail);
    else {
      Serial.printf("[CFG] Unknown key ignored: %s\n", key.c_str());
    }
  }

  f.close();
  Serial.printf("[CFG] Loaded %d key(s) from /wasp.cfg\n", loaded);
  Serial.printf("[CFG]   AP      : %s\n", cfg.apSsid);
  Serial.printf("[CFG]   Home    : %s\n", cfg.homeSsid[0] ? cfg.homeSsid : "(not set)");
  Serial.printf("[CFG]   WiGLE   : %s\n", cfg.wigleBasicToken[0] ? "token set" : "(not set)");
  Serial.printf("[CFG]   WDGWars : %s\n", cfg.wdgwarsApiKey[0]   ? "key set"   : "(not set)");
  return loaded > 0;
}
