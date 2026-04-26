# Stage 6 — File Sync

**Goal:** Workers upload completed CSV log files to the nest automatically, so all scan data aggregates in one place without manual SD card swapping.

## What it does

Every `SYNC_EVERY` cycles the worker pauses scanning, connects to the nest's WiFi AP, and uploads all pending `.csv` files via raw TCP (port 8080) in 256-byte chunks. The nest writes each file directly to its own SD card under `/logs/<workerMAC>/`. Successfully uploaded files are renamed `.csv.done` on the worker. The worker then reconnects to wardriving and resumes.

The nest also tracks worker presence via ESP-NOW heartbeats — connecting and disconnecting events are logged to serial.

```
[SYNC]  wasp_0001.csv   4821 B  ... OK
[SYNC]  wasp_0002.csv   5103 B  ... OK
[SYNC] Done — 2 uploaded, 0 failed, 0 skipped
```

## What was learned

- Arduino's `HTTPClient` caused cascade failures (`-3 → -1`) on sequential uploads of files > ~4 KB due to heap-backed String allocation. Replaced with raw TCP streaming which has no heap overhead.
- `entry.name()` on SD returns either a full path or bare filename depending on library version — path normalisation is required.
- `SYNC_EVERY = 25` (cycles) balances upload frequency against scan continuity.

## Files

| File | Board |
|---|---|
| `nest/nest.ino` | CYD (ESP32 Dev Module) |
| `worker/worker.ino` | XIAO ESP32-C5 |
