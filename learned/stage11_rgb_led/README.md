# Stage 11 — RGB LED Status Indicator

Replaces the SSD1306 OLED with a single addressable RGB LED on the worker.
The Nest already handles all rich display work; an OLED on a field unit
nobody can read wastes power and I2C bus space.

---

## What's new

### Single addressable RGB LED (worker only)

A WS2812/SK6812 LED wired to `D0` (GPIO3) on the XIAO Expansion Board
replaces the OLED for field status. The pin is a single `#define LED_PIN`
at the top of `worker.ino` — change it there for perfboard layouts.

**Flash vocabulary:**

| State | Colour | Pattern |
|---|---|---|
| Boot / power-on confirm | White `#FFFFFF` | 3× quick (50ms each) |
| GPS acquiring | Amber `#FAA307` | Slow pulse, 800ms cycle |
| GPS fix acquired | Cyan `#00FFFF` | 2× flash |
| Scan cycle start | Yellow `#FFFF00` | 1× flash (100ms) |
| Connecting to Nest AP | Blue `#4488FF` | Fast blink (200ms) |
| Sync success | Green `#00FF00` | 2× flash |
| Sync fail / nest unreachable | Red `#FF0000` | 3× fast flash |
| Chunked upload failed (→ .defer) | Orange `#FF6600` | 4× fast flash |
| Low heap warning | Red `#FF0000` | 1× slow pulse (400ms) |

Colours are chosen to match the WASP colour palette already on the Nest
display, so the visual language stays consistent across devices.

### Worker config file (`worker.cfg`)

Create `/worker.cfg` on the worker's own SD card to tune LED behaviour
per-worker without reflashing:

```
# WASP worker config
ledEnabled=true
ledBrightness=40
```

| Key | Default | Range | Effect |
|---|---|---|---|
| `ledEnabled` | `true` | `true`/`false` or `1`/`0` | Disables all LED output |
| `ledBrightness` | `40` | 0–255 | NeoPixel brightness level |

If `worker.cfg` is absent the compiled defaults apply. Config is read
once at boot from SD, before the GPS detection loop, so `detectGPS()`
already runs at the correct brightness.

### LED hardware note

Use an **SK6812 mini-e** or similar 3.3V-tolerant device — runs directly
off the C5's 3.3V data line with no level shifter. WS2812B also works at
short cable runs despite preferring 5V logic. Pull VCC from the 3V3 pin
to stay within the battery-efficient power envelope.

---

## Flash targets

| Board | File |
|---|---|
| Nest (CYD) | `nest/nest.ino` (unchanged from Stage 10) |
| Worker / Drone (XIAO ESP32-C5) | `worker/worker.ino` |

### Additional library required for Worker

| Library | Install via |
|---|---|
| Adafruit NeoPixel by Adafruit | Tools > Manage Libraries |

Arduino IDE settings for the Worker are unchanged from Stage 10.
