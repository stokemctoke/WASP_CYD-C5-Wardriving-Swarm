# Stage 3 — Worker with GPS

**Goal:** Geo-tag every scan cycle so that WiFi and BLE results carry real-world coordinates.

## What it does

A UART GPS module is wired to the C5. TinyGPS++ parses NMEA sentences in the background. At the start of each scan cycle the worker reads the current fix — if valid, coordinates are stamped on that cycle's data; if not, a `NO FIX` row is recorded so no data is silently lost.

```
[WORKER] GPS  55.636124, -4.779442 | alt -27.8m | sats 8 | hdop 3.10
[WORKER] WiFi: 12 total (8 x 2.4G, 4 x 5G) best -34 dBm
[WORKER] BLE:  1 device(s)
```

## What was learned

- GPS cold-start takes 30–90 seconds outdoors; the worker scans usefully while waiting for a fix.
- HDOP < 2.0 is a reliable quality threshold for wardriving data.
- All networks found in a cycle share the GPS fix from the start of that cycle — good enough at walking/driving speeds.

## Wiring

| GPS pin | C5 pin | GPIO |
|---|---|---|
| TX | D7 | GPIO12 |
| RX | D6 | GPIO11 |
| VCC | 3V3 | — |
| GND | GND | — |

## Files

| File | Board |
|---|---|
| `worker/worker.ino` | XIAO ESP32-C5 |
