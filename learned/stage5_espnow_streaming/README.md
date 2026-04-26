# Stage 5 — ESP-NOW Streaming

**Goal:** Workers send live scan summaries to the nest after every cycle while continuing to log to SD — first time the swarm communicates as a unit.

## What it does

After each scan cycle the worker packs a summary struct (GPS fix, WiFi totals, BLE count, best RSSI, cycle number) and sends it to the nest via ESP-NOW. The nest receives packets from any number of workers and prints a live feed to serial. SD logging continues in parallel — no data is sacrificed for the stream.

```
[NEST] Worker 38:44:BE:BA:0F:30 | cycle 5 | RSSI link -39 dBm
       GPS FIX  55.636124, -4.779442 | alt -27.8m | sats 8 | hdop 3.10
       WiFi: 12 total (8 x 2.4G, 4 x 5G) best -34 dBm
       BLE:  1 device(s)
```

## What was learned

- ESP-NOW packet size is limited to 250 bytes — the summary struct fits comfortably.
- The nest must be in `WIFI_AP_STA` mode and workers must target the STA MAC (not the AP MAC) for reliable delivery.
- Multiple workers are handled by a registry keyed on sender MAC — scales to 8 workers with no code changes.

## Files

| File | Board |
|---|---|
| `nest/nest.ino` | CYD (ESP32 Dev Module) |
| `worker/worker.ino` | XIAO ESP32-C5 |
