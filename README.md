[![Ko-Fi](https://img.shields.io/badge/Ko--Fi-Support%20Me-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/stoke)
[![My Website](https://img.shields.io/badge/Website-stokemctoke.com-FAA307)](https://stokemctoke.com)
[![Platform: ESP32-C5](https://img.shields.io/badge/Platform-ESP32--C5-blue)](https://www.espressif.com/en/products/socs/esp32-c5)

# W.A.S.P. - Wardriving Swarm Project

![W.A.S.P. - Wardriving Swarm Project](WASP_Repo-Image.jpg)

A from-scratch wardriving mesh built to learn how all the pieces fit together.

**W.A.S.P.** uses a **Nest / Worker** architecture:

- **Nest** (Cheap Yellow Display) — the base station. Purely UI, control, and connectivity. No wardriving. Manages worker status, handles file aggregation, and uploads to WiGLE and WDGWars when connected to a home network. Yellow. **All rich display work lives here.**
- **Worker** (Seeed XIAO ESP32-C5) — the autonomous scanner. Dual-band 2.4 GHz + 5 GHz WiFi and BLE. Carries its own GPS and SD card so it operates fully independently. Syncs back to the nest when in range. Black. **Status indicated by a single addressable RGB LED** — keeps power draw minimal for extended battery runs.

> Nest = yellow. Workers = black. Casings to follow.

---

## Worker vs Drone

Both run the same unified firmware. Mode is **auto-detected at boot** — no recompile needed.

| | Worker | Drone |
|---|---|---|
| SD card | Present | Absent |
| GPS | Present (used if detected) | Absent |
| Scan storage | WiGLE CSV on SD | 25-slot RAM circular buffer |
| Buffer capacity | Unbounded (SD) | 25 cycles × 40 WiFi + 20 BLE entries (~55 KB) |
| Buffer full behaviour | n/a | Oldest slot overwritten |
| Sync to nest | Raw TCP stream of `.csv` files (port 8080) | HTTP POST of serialised buffer slots |
| Coordinates | Real GPS fix (or NO FIX row) | 0.000000, 0.000000 |
| ESP-NOW heartbeat | Yes — `nodeType = 0x00` | Yes — `nodeType = 0x01` |
| Nest display label | Green dot · **W** | Cyan dot · **D** |

Detection order at boot:
1. `SD.begin()` — if it succeeds, Worker mode. If it fails, Drone mode.
2. GPS UART fed for 2 s — if `charsProcessed > 0`, GPS is live and used.

Both modes send an ESP-NOW heartbeat every 5 s and sync scan data to the nest every 25 cycles (configurable via `syncEvery` in `worker.cfg`).

---

## Why this split?

The CYD's standard ESP32 is 2.4 GHz only — adding wardriving there would introduce a second-class scanner into the data. More importantly, one radio cannot cleanly handle ESP-NOW (listening for workers) + channel-hopping (wardriving) + WiFi STA (home network uploads) simultaneously. Keeping the nest off scanning duty eliminates that radio contention entirely and keeps the UI fully responsive.

The result is a clean separation:

```
WORKERS  — scan everything, log locally, push to nest
NEST     — display, control, receive, upload
```

Want more coverage? Add another worker. The nest never changes.

---

## Architecture

```
┌──────────────────────────────────────────────┐
│                NEST (CYD)                    │
│  Standard ESP32 — yellow                     │
│                                              │
│  ILI9341 touch display (built-in)            │
│  SD card (built-in)                          │
│                                              │
│  • Worker status dashboard                   │
│  • Receives scan data via ESP-NOW            │
│  • File browser and log management           │
│  • Connects to home WiFi for uploads         │
│  • WiGLE + WDGWars upload                    │
│  • No wardriving — radio free for comms      │
└────────────────┬─────────────────────────────┘
                 │  ESP-NOW (heartbeat / data)
                 │  WiFi AP/STA (file sync)
     ┌───────────┼───────────┐
     │           │           │
┌────┴────┐ ┌────┴────┐ ┌────┴────┐
│ WORKER  │ │ WORKER  │ │  DRONE  │
│ESP32-C5 │ │ESP32-C5 │ │ESP32-C5 │
│  black  │ │  black  │ │         │
│         │ │         │ │         │
│RGB LED  │ │RGB LED  │ │RGB LED  │
│GPS(UART)│ │GPS(UART)│ │RAM buf  │
│SD card  │ │SD card  │ │no SD/GPS│
│         │ │         │ │         │
│2.4G WiFi│ │2.4G WiFi│ │2.4G WiFi│
│5GHz WiFi│ │5GHz WiFi│ │5GHz WiFi│
│BLE      │ │BLE      │ │BLE      │
└─────────┘ └─────────┘ └─────────┘
```

---

## Hardware

| Role | Board | Colour | Notes |
|---|---|---|---|
| Nest | CYD JC2432W328C | Yellow | Built-in ILI9341 display, SD slot, touch |
| Worker | Seeed XIAO ESP32-C5 | Black | Dual-band (2.4 + 5 GHz) + BLE, RISC-V |

**All worker testing was done on the Seeed Studio XIAO Expansion Board v1.2** — the official carrier board for XIAO modules that provides a built-in OLED, SD card slot, Grove connectors, and clean breakouts for all XIAO pins without needing perfboard or loose wiring. This is the pre-perfboard stage of the build. If you are replicating this project, the XIAO Expansion Board removes a whole class of wiring problems and is strongly recommended for getting started.

The XIAO Expansion Board v1.2 exposes the following connections used by W.A.S.P.:

| Function | Expansion Board | XIAO pin | GPIO |
|---|---|---|---|
| GPS TX (module → C5) | GPS header | D7 | GPIO12 |
| GPS RX (C5 → module) | GPS header | D6 | GPIO11 |
| SD CS | SD header | D2 | GPIO25 |
| SD SCK | SD header | D8 | GPIO8 |
| SD MISO | SD header | D9 | GPIO9 |
| SD MOSI | SD header | D10 | GPIO10 |
| RGB LED data / R | D0 breakout | D0 | GPIO3 |
| RGB LED G | D4 breakout | D4 | GPIO23 |
| RGB LED B | D5 breakout | D5 | GPIO24 |

> **Note on the RGB LED:** Two types are supported, chosen via `ledType` in `worker.cfg`:
> - **`ws2812`** (default) — addressable WS2812B or SK6812 on D0 (GPIO3). SK6812 mini-e is recommended (3.3V tolerant). WS2812B works but prefers 5V logic — at short cable runs the C5's 3.3V output usually drives it.
> - **`rgb4pin`** — standard 4-pin common-cathode RGB LED. R=D0 (GPIO3), G=D4 (GPIO23), B=D5 (GPIO24). Pull each channel through a ~47–100Ω resistor to the GPIO pin; cathode to GND. D4 and D5 were freed when the OLED was removed.
> Pull the LED's power from the 3V3 pin for battery efficiency.

**Peripherals per worker:**
- RGB LED — status flashes (GPS state, scan active, sync result). Type (`ws2812` or `rgb4pin`), brightness, and on/off controlled per-worker via `worker.cfg`. Replaces SSD1306 OLED — the Nest display handles all rich status; an OLED on a field unit nobody can read wastes power.
- UART GPS module — independent geo-tagging
- SPI micro-SD module — local log storage when out of nest range

**Nest peripherals:**
- SD card (built-in CYD slot) — aggregated log storage
- Onboard RGB LED (active LOW) — status flashes. Red: GPIO 4, Green: GPIO 16, Blue: GPIO 17. GPIO 4 is shared with `TFT_RST` in `User_Setup.h`; safe to use as an output after `tft.init()` completes.

> **Display driver:** The JC2432W328C runs an **ILI9341** panel — confirmed by working firmware. Some sources list ST7789; that applies to a different CYD variant. Configure `TFT_eSPI`'s `User_Setup.h` with `#define ILI9341_DRIVER`.

> **Touch driver:** The JC2432W328C uses a **CST820 capacitive** controller on I²C (SDA=33, SCL=32, RST=25, addr=0x15) — *not* the resistive XPT2046/SPI controller used by older CYDs. The firmware drives it with a minimal `Wire`-based driver (`nest_touch.cpp`) — no extra library install. The CST820 INT pin (GPIO 21) collides with the TFT backlight on this board, so touch is polled, not interrupt-driven.

---

## LED Status Flash Codes

### Worker (D0/GPIO3 for WS2812B; D0+D4+D5 for 4-pin common-cathode)

| State | Colour | Pattern |
|---|---|---|
| Boot / power-on | White | 3× quick flash (50 ms each) |
| GPS acquiring | Amber `#FAA307` | Slow pulse — 800 ms on / 800 ms off |
| GPS fix acquired | Cyan `#00FFFF` | 2× flash |
| Scan cycle start | Yellow `#FFFF00` | 1× flash (100 ms) |
| Connecting to Nest AP | Blue `#4488FF` | Fast blink (200 ms) |
| Sync success | Green `#00FF00` | 2× flash |
| Sync fail / nest unreachable | Red `#FF0000` | 3× fast flash |
| Chunked upload failed (file → .defer) | Orange `#FF6600` | 4× fast flash |
| Low heap warning | Red `#FF0000` | 1× slow pulse (400 ms) |

LED type, brightness, and all flash patterns are set per-worker in `/worker.cfg` on the worker SD card. See `worker.cfg.example`.

### Nest (onboard RGB LED — active LOW, GPIOs 4 / 16 / 17)

| State | Colour | Pattern |
|---|---|---|
| Boot / power-on | White (R+G+B) | 3× quick flash (50 ms each) |
| Worker heartbeat received | Green | 1× brief flash (50 ms) |
| File chunk / sync received | Blue | 1× flash (80 ms) per chunk |

> GPIO 4 (red channel) is shared with `TFT_RST` in `TFT_eSPI`'s `User_Setup.h`. It is reclaimed as a plain output immediately after `tft.init()` completes — the reset pulse only fires once at startup.

---

## Build Stages

| Stage | Description | Status |
|---|---|---|
| 1 | ESP-NOW ping-pong — prove cross-chip comms (Nest ↔ Worker) | ✅ Complete |
| 2 | Worker standalone scan — 2.4 + 5 GHz WiFi + BLE to serial | ✅ Complete |
| 3 | Worker with GPS — geo-tagged scans to serial | ✅ Complete |
| 4 | Worker with SD — write WiGLE-format log locally | ✅ Complete |
| 5 | Real-time streaming — worker streams scan data to nest via ESP-NOW | ✅ Complete |
| 6 | File sync — worker connects to nest AP and transfers logs | ✅ Complete |
| 7 | Nest display — worker list, scan counts, file browser on CYD touch | ✅ Complete |
| 8 | Unified worker firmware — auto-detects Worker vs Drone mode at boot | ✅ Complete |
| 9 | Hardened file sync — 8 KB log cap, RAM pre-buffer, path validation, TCP upload | ✅ Complete |
| 10 | Chunked upload — split large files into 8 KB chunks for reliable transfer of any size | ✅ Complete |
| 11 | RGB LED status — replace OLED with single addressable LED; brightness + on/off in worker.cfg | ✅ Complete |
| 12 | Home upload — Nest connects to home WiFi and uploads CSVs to WiGLE + WDGWars; worker `ledType` config (ws2812 / rgb4pin) | ✅ Complete |
| 13 | LED config via SD — all flash patterns (colour, count, timing) tuneable in wasp.cfg / worker.cfg without reflash | ✅ Complete |
| 14 | Fast sync + upload fixes — single-connect chunked upload confirmed; WiGLE column order fix; ESP-NOW restore after failed nest connect | ✅ Complete |
| 15 | Modular refactor — worker split into 9 modules; nest split into 8 modules. Future changes touch only the relevant file | ✅ Complete |
| 16 | Nest touch UI — capacitive CST820 (I²C) driver, stack-based menu, fade transitions, file browser, worker detail, settings. Invalidation-driven rendering eliminates flicker on detail screens | ✅ Complete |

---

## Getting Started

### Arduino IDE Setup

**Board package:** `esp32 by Espressif Systems` v3.0.0 or newer.
Install via `Tools > Board > Board Manager`.

| Device | Board selection |
|---|---|
| Nest (CYD) | `ESP32 Dev Module` |
| Worker (C5) | `XIAO_ESP32C5` |

> **Important for Worker — three settings required before flashing:**
> - `Tools > USB CDC On Boot > Enabled` — the C5 uses native USB, without this Serial output will not appear
> - `Tools > Partition Scheme > Huge APP (3MB No OTA/1MB SPIFFS)` — the default partition scheme only allocates ~1.25MB for the app; NimBLE needs ~1.35MB, so this is required (4MB flash is fine)
> - `Tools > PSRAM > Disabled` — the XIAO ESP32-C5 has no physical PSRAM; enabling it causes a boot crash (`Failed to allocate dummy cacheline for PSRAM memory barrier!`)

### Required Libraries

| Library | Install via |
|---|---|
| NimBLE-Arduino by h2zero | Tools > Manage Libraries |
| TinyGPS++ by Mikal Hart | Tools > Manage Libraries |
| Adafruit NeoPixel by Adafruit | Tools > Manage Libraries |
| TFT_eSPI by Bodmer | Tools > Manage Libraries |

> **TFT_eSPI requires manual configuration** — after installing, edit `Arduino/libraries/TFT_eSPI/User_Setup.h`. Comment out the existing driver and pin definitions, then add:
> ```cpp
> #define ILI9341_DRIVER
> #define TFT_MISO  12
> #define TFT_MOSI  13
> #define TFT_SCLK  14
> #define TFT_CS    15
> #define TFT_DC     2
> #define TFT_RST    4
> #define LOAD_GLCD
> #define LOAD_FONT2
> #define LOAD_FONT4
> #define SPI_FREQUENCY       40000000
> #define SPI_READ_FREQUENCY  20000000
> ```

### Flashing

Flash `stage15/nest/nest.ino` to the CYD and `stage15/worker/worker.ino` to the C5.

### Config Files

Both devices use a simple `key=value` config file on their own SD card. Lines starting with `#` are comments; no spaces around `=`.

**Nest — `/wasp.cfg`** (see `wasp.cfg.example`)

| Key | Default | Description |
|---|---|---|
| `homeSsid` | — | Home Wi-Fi SSID for WiGLE / WDGWars uploads |
| `homePsk` | — | Home Wi-Fi password |
| `apSsid` | `WASP-Nest` | Nest AP name that workers connect to for file sync |
| `apPsk` | `waspswarm` | Nest AP password |
| `wigleBasicToken` | — | WiGLE 'Encoded for use' API token |
| `wdgwarsApiKey` | — | WDGWars API key (64 hex chars) |
| `nestLedBoot` | `FFFFFF,3,50,50` | LED event: `colour(hex),flashes,onMs,offMs` |
| `nestLedHeartbeat` | `00FF00,1,50,50` | Flash on worker heartbeat received |
| `nestLedChunk` | `0000FF,1,80,80` | Flash on file chunk received |
| `nestLedUploadAct` | `FFFF00,1,80,80` | Flash while home upload in progress |
| `nestLedUploadOK` | `00FF00,2,80,80` | Flash on successful home upload |
| `nestLedUploadFail` | `FF0000,3,80,80` | Flash on failed home upload |

**Worker — `/worker.cfg`** (see `worker.cfg.example`)

| Key | Default | Description |
|---|---|---|
| `ledEnabled` | `true` | `false` or `0` to disable all LED output |
| `ledBrightness` | `40` | 0–255 brightness (40 ≈ 15%) |
| `ledType` | `ws2812` | `ws2812` = addressable WS2812B/SK6812; `rgb4pin` = common-cathode 4-pin RGB |
| `ledPin` | `3` | WS2812 data pin (or rgb4pin R pin) |
| `ledPinG` | `23` | rgb4pin Green pin |
| `ledPinB` | `24` | rgb4pin Blue pin |
| `ledBoot` | `FFFFFF,3,50,50` | LED event: `colour(hex),flashes,onMs,offMs` |
| `ledGPSAcquire` | `FAA307,1,800,800` | Slow amber pulse while acquiring GPS |
| `ledGPSFound` | `00FFFF,2,100,100` | Cyan flash when GPS UART detected |
| `ledGPSFix` | `00FFFF,2,100,100` | Cyan flash on first position fix |
| `ledScanCycle` | `FFFF00,1,100,100` | Yellow flash at start of each scan cycle |
| `ledConnecting` | `4488FF,1,200,200` | Blue blink while connecting to nest |
| `ledSyncOK` | `00FF00,2,100,100` | Green flash on successful sync |
| `ledSyncFail` | `FF0000,3,80,80` | Red flash on sync failure |
| `ledTooBig` | `FF6600,4,80,80` | Orange flash when file deferred (too large) |
| `ledLowHeap` | `FF0000,1,400,400` | Red slow pulse on low heap warning |
| `ledDronePulse` | `8800FF,1,200,200` | Purple pulse — drone mode heartbeat |
| `ledHeartbeat` | `FF69B4,1,50,50` | Pink flash on ESP-NOW heartbeat sent |
| `nestSsid` | `WASP-Nest` | Nest AP SSID to connect to for sync |
| `nestPsk` | `waspswarm` | Nest AP password |
| `nestIp` | `192.168.4.1` | Nest IP address |
| `syncEvery` | `25` | Sync to nest every N scan cycles |
| `heartbeatIntervalMs` | `5000` | ms between ESP-NOW heartbeats |
| `wifiChanMs` | `80` | ms spent per WiFi channel during scan |
| `bleScanMs` | `3000` | ms per BLE scan pass |
| `cycleDelayMs` | `2000` | ms pause at end of each scan cycle |
| `maxLogBytes` | `8192` | Rotate log file above this size (bytes) |
| `lowHeapThreshold` | `30000` | Heap level (bytes) that triggers LED warning |
| `gpsBaud` | `9600` | GPS UART baud rate |
| `gpsRxPin` | `12` | UART1 RX GPIO (GPS TX → here) |
| `gpsTxPin` | `11` | UART1 TX GPIO (GPS RX ← here) |

Config is read once at boot. If the file is absent, compiled-in defaults apply.
Each worker can carry its own `worker.cfg` so units can be tuned independently without reflashing.

---

## Repository Layout

```
/
├── stage15/                       ← active firmware (flash this)
│   ├── nest/
│   │   ├── nest.ino               ← CYD: setup(), loop(), FreeRTOS task spawn
│   │   ├── nest_types.h           ← shared structs, constants, pin defines
│   │   ├── nest_config.h/cpp      ← loadConfig(), wasp.cfg parsing
│   │   ├── nest_led.h/cpp         ← onboard RGB LED abstraction
│   │   ├── nest_registry.h/cpp    ← worker registry, ESP-NOW state
│   │   ├── nest_espnow.h/cpp      ← ESP-NOW receive callback
│   │   ├── nest_upload.h/cpp      ← TCP raw + HTTP upload handlers
│   │   ├── nest_home.h/cpp        ← home WiFi connect + WiGLE/WDGWars upload
│   │   ├── nest_display.h/cpp     ← TFT display rendering
│   │   ├── nest_touch.h/cpp       ← CST820 capacitive touch driver (I²C)
│   │   └── nest_ui.h/cpp          ← screen stack, fade transitions, touch dispatch
│   └── worker/
│       ├── worker.ino             ← setup(), loop(), mode detection
│       ├── worker_types.h         ← shared structs, constants, pin defines
│       ├── worker_config.h/cpp    ← loadWorkerConfig(), worker.cfg parsing
│       ├── worker_led.h/cpp       ← WS2812 + rgb4pin LED abstraction
│       ├── worker_gps.h/cpp       ← GPS detect, clock sync, timestamps
│       ├── worker_storage.h/cpp   ← SD log open/rotate/flush, CSV writers
│       ├── worker_drone.h/cpp     ← RAM circular buffer, drone CSV builder
│       ├── worker_scan.h/cpp      ← WiFi + BLE scanning
│       ├── worker_espnow.h/cpp    ← ESP-NOW init, heartbeat, summary send
│       └── worker_sync.h/cpp      ← connectToNest, syncFiles, uploadChunked
```

---

## Credits

W.A.S.P. is built from scratch as a learning exercise, developed with
[Claude Code](https://claude.ai/code) by Anthropic.

- WiGLE: [wigle.net](https://wigle.net)
- WDGWars: [wdgwars.pl](https://wdgwars.pl)

**Inspiration:**
- [JustCallMeKoko](https://github.com/justcallmekoko) — ESP32 wardriving pioneer whose work in the space inspired this project
- [SpicedHam](https://github.com/SpicedHam) — wardriving community inspiration
