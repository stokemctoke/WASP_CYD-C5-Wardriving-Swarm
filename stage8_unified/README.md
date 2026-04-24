# Stage 8 — Unified Worker / Drone Firmware

**Goal:** A single firmware image that auto-detects at boot whether it is running on a Worker (SD + GPS) or a Drone (RAM buffer only) — no recompile, no reflash to switch modes.

## What it does

On boot the firmware probes for hardware:

1. `SD.begin()` — succeeds → **Worker mode** (full SD logging, GPS used if detected)
2. `SD.begin()` — fails → **Drone mode** (25-slot circular RAM buffer, no GPS)

Both modes send ESP-NOW heartbeats every 5 s and sync scan data to the nest every 25 cycles (`SYNC_EVERY`).

| | Worker | Drone |
|---|---|---|
| Storage | WiGLE CSV on SD | 25-slot RAM circular buffer |
| GPS | Used if detected | Not used |
| Sync | Raw TCP upload (port 8080) | HTTP POST of buffer slots |
| Nest display | Green dot · **W** | Cyan dot · **D** |

## Extended packet header introduced here

Both `heartbeat_t` and `scan_summary_t` were extended with 16 bytes of game-ready fields:

| Field | Type | Purpose |
|---|---|---|
| `swarmId` | `uint16_t` | djb2 hash of swarm name — nest filtering |
| `loyaltyLevel` | `uint8_t` | 0=wild → 255=loyal (drone mechanic) |
| `gangId` | `uint8_t` | WDGWars gang affiliation |
| `firmwareVer` | `uint8_t` | Cross-swarm compatibility |
| `battLevel` | `uint8_t` | 0–100 %, 255=unknown |
| `playerLevel` | `uint16_t` | Rank / promotion |
| `boostLevel` | `uint8_t` | Active boost / buff tier |
| `reserved[7]` | `uint8_t` | Zero-filled, future use |

`heartbeat_t` = 24 bytes · `scan_summary_t` = 52 bytes · ESP-NOW limit = 250 bytes.

## What was learned

- SD probe at boot is a reliable hardware-detection method with no user input needed.
- Raw TCP upload (port 8080) avoids the `HTTPClient` heap allocation failures seen in Stage 6 at scale.
- Packet backward compatibility is preserved — nest accepts old 8-byte heartbeats and 36-byte summaries from earlier firmware.

## Files

| File | Board |
|---|---|
| `worker/worker.ino` | XIAO ESP32-C5 (Worker or Drone) |

> **Nest firmware:** use `stage7_nest_display/nest/nest.ino` — fully compatible with Stage 8 workers.
