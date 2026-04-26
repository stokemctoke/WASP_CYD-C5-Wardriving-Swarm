# Stage 2 — Worker Standalone Scan

**Goal:** Confirm the XIAO ESP32-C5 can scan both 2.4 GHz and 5 GHz WiFi bands plus BLE in a single cycle — no GPS, no logging, no nest.

## What it does

The worker loops continuously: full dual-band WiFi scan followed by a 3-second BLE scan. Results are printed to serial each cycle with BSSID, SSID, channel, RSSI, and security type.

```
[WORKER] WiFi: 14 total (9 x 2.4G, 5 x 5G)
  #  Band  Ch  RSSI  Security    BSSID              SSID
   1  5GHz  36   -41  WPA2_PSK   AA:BB:CC:DD:EE:FF  HomeNet_5G
   2  2.4G   6   -55  WPA2_PSK   11:22:33:44:55:66  HomeNet
...
[WORKER] BLE: 3 device(s)
```

## What was learned

- The C5's dual-band radio works out of the box — no extra hardware.
- 5 GHz scanning requires the full channel list (36, 40, 44 … 165).
- BLE and WiFi scanning cannot run simultaneously; BLE scan blocks for its duration.

## Files

| File | Board |
|---|---|
| `worker/worker.ino` | XIAO ESP32-C5 |
