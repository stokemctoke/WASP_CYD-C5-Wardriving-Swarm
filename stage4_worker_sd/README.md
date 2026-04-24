# Stage 4 — Worker with SD Card

**Goal:** Persist scan data locally so the worker operates fully standalone — no nest required, no data lost if out of range.

## What it does

Each scan cycle appends WiFi and BLE entries to a WiGLE-format CSV file on the SD card. A new file is created each boot (timestamped by cycle count). Files survive power cycles and can be uploaded to WiGLE or WDGWars directly.

```
WigleWifi-1.6,appRelease=WASP,model=XIAO-C5,...
MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,...
AA:BB:CC:DD:EE:FF,HomeNet,[WPA2_PSK],2026-04-09 12:00:00,6,2437,-45,55.636...
```

## What was learned

- WiGLE CSV format is straightforward and accepted by both WiGLE and WDGWars without modification.
- SD card SPI bus must be kept separate from any other SPI devices (display etc.) to avoid conflicts.
- File handles should be opened and closed per-cycle rather than held open — prevents corruption on unexpected power loss.

## Files

| File | Board |
|---|---|
| `worker/worker.ino` | XIAO ESP32-C5 |
