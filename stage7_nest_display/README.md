# Stage 7 — Nest Display

**Goal:** Give the nest a live visual dashboard on the CYD's built-in ILI9341 touchscreen so swarm status is readable at a glance without a serial monitor.

## What it does

The CYD display runs in portrait mode (240 × 320, USB-C at bottom, antenna at top). It shows up to 5 worker rows, each updated every second:

- Coloured status dot: green = Worker active, cyan = Drone active, yellow = stale (>10 s), grey = offline (>30 s)
- **W** / **D** type label next to the dot
- MAC address and RSSI link quality (colour-coded green → yellow → orange → red)
- GPS fix status + WiFi/BLE scan counts + cycle number
- Footer: last file sync event

```
┌─────────────────────────────┐  ← amber header "W.A.S.P. NEST  ch:1"
│ Workers in range: 2         │  ← status bar
├─────────────────────────────┤
│ ● W  38:44:BE:BA:0F:30  -34 │  ← green dot, Worker MAC, RSSI
│   FIX  W:12(8+4) B:1 #19   │  ← GPS + scan data
├─────────────────────────────┤
│ ● D  38:44:BE:BA:13:78  -26 │  ← cyan dot, Drone MAC
│   NO FIX  W:15(11+4) B:6 #23│
└─────────────────────────────┘
│ Last sync: 3844BEBA0F30 4821B│  ← footer
```

## What was learned

- `setRotation(0)` gives portrait with USB-C at bottom and antenna at top on the CYD JC2432W328C.
- The display and SD card must use separate SPI buses (HSPI for display, VSPI for SD) — they cannot share.
- Refreshing only changed rows rather than the full screen eliminates flicker.

## Files

| File            | Board                  |
| --------------- | ---------------------- |
| `nest/nest.ino` | CYD (ESP32 Dev Module) |
