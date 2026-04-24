# Stage 1 — ESP-NOW Ping-Pong

**Goal:** Prove that the CYD (standard ESP32) and the XIAO ESP32-C5 can talk to each other over ESP-NOW before any wardriving code is written.

## What it does

The Nest sends numbered `PING` packets over ESP-NOW. The Worker receives each one and immediately replies with a matching `PONG`. Both sides print RSSI for every exchange so you can see link quality at a glance.

```
[NEST] TX PING #0 ... sent
[NEST] RX PONG #0 from 38:44:BE:BA:0F:30 | RSSI -35 dBm

[WORKER] RX PING #0 from A4:F0:0F:5D:96:D4 | RSSI -34 dBm
[WORKER] TX PONG #0 ... sent
```

## What was learned

- ESP-NOW works cross-chip (standard ESP32 ↔ RISC-V ESP32-C5) with no special config.
- The CYD's STA MAC is the address workers must target — not the AP MAC.
- Channel must match on both sides; `WIFI_AP_STA` mode is needed on the nest to keep the STA MAC stable.

## Files

| File | Board |
|---|---|
| `nest/nest.ino` | CYD (ESP32 Dev Module) |
| `worker/worker.ino` | XIAO ESP32-C5 |
