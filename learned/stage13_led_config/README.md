# Stage 13 — LED Config from SD

All LED event colours and timing are now configurable via SD config files — no
reflash needed to change any flash colour, flash count, or on/off timing.

---

## What's new

### Worker — `worker.cfg`

Every named LED event has a corresponding config key. Format is:

```
ledEvent=RRGGBB,flashes,onMs,offMs
```

| Key | Default | Notes |
|---|---|---|
| `ledBoot` | `FFFFFF,3,50,50` | White 3× at power-on |
| `ledGPSAcquire` | `FAA307,0,400,400` | Amber toggle during GPS detect window. `flashes=0` = continuous toggle |
| `ledGPSFound` | `FAA307,4,400,300` | Prominent amber burst after module confirmed (new in Stage 13) |
| `ledGPSFix` | `00FFFF,2,150,100` | Cyan 2× when valid lat/lon fix acquired |
| `ledScanCycle` | `FFFF00,1,100,0` | Yellow 1× at top of each scan |
| `ledConnecting` | `4488FF,0,200,200` | Blue toggle while connecting to Nest AP. `flashes=0` = continuous |
| `ledSyncOK` | `00FF00,2,150,100` | Green 2× after successful sync |
| `ledSyncFail` | `FF0000,3,80,80` | Red 3× if Nest unreachable |
| `ledTooBig` | `FF6600,4,80,80` | Orange 4× if chunked upload fails (→ .defer) |
| `ledLowHeap` | `FF0000,1,400,0` | Red 1× slow pulse — heap below 30 KB |
| `ledDronePulse` | `00FFFF,2,200,100` | Cyan double-double-pulse after drone buffer upload |
| `ledHeartbeat` | `FF69B4,2,80,80` | Pink 2× each time an ESP-NOW heartbeat is sent |

Any key can be omitted — compiled-in defaults apply. See `worker.cfg.example`.

### `ledGPSFound` — new event

Previously the GPS detect flash (amber) only fired during the 2-second detect
window and was easy to miss. Stage 13 adds `ledGPSFound`: a prominent amber
burst that fires once immediately after the GPS module is confirmed. Default is
4 flashes × 400 ms — about 2.8 seconds of clear amber. Unmissable.

### Heartbeat LED — now active

`ledHeartbeat` fires every time an ESP-NOW heartbeat packet is sent to the Nest
(every 5 seconds). Default is pink `#FF69B4` 2× fast.

---

### Nest — `wasp.cfg`

| Key | Default | Notes |
|---|---|---|
| `nestLedBoot` | `FFFFFF,3,50,50` | White 3× at power-on |
| `nestLedHeartbeat` | `FF00FF,2,80,80` | Pink 2× when worker heartbeat received |
| `nestLedChunk` | `0000FF,1,80,0` | Blue 1× per chunk received from worker |
| `nestLedUploadAct` | `00FFFF,0,0,0` | Cyan solid during home WiFi upload (`flashes=0` = solid) |
| `nestLedUploadOK` | `00FF00,2,200,200` | Green 2× after successful WiGLE/WDGWars upload |
| `nestLedUploadFail` | `FF0000,3,200,200` | Red 3× if upload or home WiFi connect fails |

> **Note:** The CYD onboard RGB LED has no PWM — colours are binary per channel
> (non-zero = on). `#FF8800` and `#FF0000` both produce red; only the channel
> presence (R/G/B on or off) matters.

---

## Flash targets

| Board | File |
|---|---|
| Nest (CYD) | `nest/nest.ino` |
| Worker / Drone (XIAO ESP32-C5) | `worker/worker.ino` |

Arduino IDE settings unchanged from Stage 12.
