# CYD JC2432W328C — Component Info Pack

**Variant:** JC2432W328C (Capacitive Touch, I²C)  
**SoC:** ESP32 (standard, dual-core Xtensa 32-bit @ 240 MHz)  
**Confirmed by:** W.A.S.P. Stage 16 integration

---

## Display — ILI9341

| Property | Value |
|----------|-------|
| **Panel** | 2.4" ILI9341, 320×240 RGB |
| **Interface** | SPI (HSPI) |
| **Color Depth** | 16-bit (RGB565) |
| **Backlight** | GPIO21 (PWM via LEDC, 5 kHz, 8-bit) |
| **Data Pin (MOSI)** | GPIO13 |
| **Clock (SCLK)** | GPIO14 |
| **Chip Select (CS)** | GPIO15 |
| **Data/Command (DC)** | GPIO2 |
| **Reset (RST)** | GPIO4 |
| **MISO** | GPIO12 (not used for display writes, but may be shared) |

**Notes:**
- TFT_eSPI library is required; User_Setup.h must be configured with above pins
- Display rotation: `setRotation(0)` = portrait, USB-C bottom, antenna top
- Backlight control: `ledcAttach(GPIO21, 5000, 8)` + `ledcWrite(GPIO21, duty)` where duty ∈ [0, 255]
- SPI clock: 40 MHz write, 20 MHz read (typical; configure in TFT_eSPI User_Setup.h)

---

## Touch Controller — CST820 (Capacitive, I²C)

| Property | Value |
|----------|-------|
| **Chip** | Hynitron CST820 Capacitive Touch |
| **Interface** | I²C @ 400 kHz |
| **I²C Address** | 0x15 (7-bit) |
| **SDA Pin** | GPIO33 |
| **SCL Pin** | GPIO32 |
| **Reset Pin (active low)** | GPIO25 |
| **IRQ Pin** | GPIO36 (optional; not used in W.A.S.P. polling mode) |

**Protocol:**
- After power-up or RST pulse, wait ≥50 ms before first I²C read
- Polling registers 0x01–0x06 yields:
  - **0x01**: Gesture type (0x00 = no gesture; 0x01–0x0F = swipe/tap codes)
  - **0x02**: Number of fingers pressed (typically 0 or 1 for stylus/capacitive pad)
  - **0x03–0x04**: Finger 1 X coordinate (big-endian, ∈ [0, 239])
  - **0x05–0x06**: Finger 1 Y coordinate (big-endian, ∈ [0, 319])
- No calibration required; coordinates map directly to screen pixels
- Typical polling cycle: ≤1 ms per read at 400 kHz I²C

**W.A.S.P. Implementation (nest_touch.h/cpp):**
- `touchBegin()`: Initializes Wire, performs RST pulse, waits 50 ms
- `touchRead(int* px, int* py)`: Reads registers, extracts coordinates; returns true if finger down
- `touchDiag()`: 1 Hz diagnostic output showing gesture, finger count, raw X/Y

**Differences vs. Resistive XPT2046:**
- Capacitive = always-on, no pressure sensing; responds to skin/conductive stylus
- No calibration needed (ILI9341 and CST820 pixel spaces are 1:1)
- No interrupt support in W.A.S.P. (polling is simpler and sufficient for UI)
- ~1/10th the code compared to XPT2046 SPI driver

---

## SD Card Slot

| Property | Value |
|----------|-------|
| **Interface** | SPI (VSPI) |
| **Clock (SCLK)** | GPIO18 |
| **Data Out (MOSI)** | GPIO23 |
| **Data In (MISO)** | GPIO19 |
| **Chip Select (CS)** | GPIO5 |

**Notes:**
- Separate SPI bus from display; no contention
- Typical clock: ≤25 MHz (use default Arduino SD library speeds)
- W.A.S.P. uses this for logging (Worker) and file upload (Nest)
- Card detection: probe for `/` directory existence; no CD pin wired

---

## Power & Reset

| Component | GPIO | Notes |
|-----------|------|-------|
| **USB 5V Input** | — | Micro-USB port; internal AMS1117-3.3 LDO |
| **EN (Reset)** | Hardware | Pull high to enable; cap to GND provides soft debounce |
| **Boot** | GPIO0 | Should be pulled high; low forces bootloader mode |

**Power Budget (typical, 240 MHz):**
- ESP32 core: ~80–120 mA
- Display (backlight @ 255): ~100–150 mA
- SD card: ~20–50 mA (peak during write)
- WiFi/BLE (active): +60–100 mA
- **Total (all subsystems active):** ≤400 mA (worst case)
- USB host supply: typically 500 mA; sufficient for development

---

## GPIO Summary

| GPIO | Function | Direction | Notes |
|------|----------|-----------|-------|
| 2 | TFT_DC | Out | Display data/command |
| 4 | TFT_RST | Out | Display reset (active low pulse during init) |
| 5 | SD_CS | Out | SD card chip select |
| 12 | TFT_MISO | In | SPI display MISO (read data) |
| 13 | TFT_MOSI | Out | SPI display MOSI (write data) |
| 14 | TFT_SCLK | Out | SPI display clock |
| 15 | TFT_CS | Out | SPI display chip select |
| 18 | SD_SCLK | Out | SPI SD clock |
| 19 | SD_MISO | In | SPI SD read data |
| 21 | TFT_BACKLIGHT | Out | PWM backlight (LEDC channel) |
| 23 | SD_MOSI | Out | SPI SD write data |
| 25 | TOUCH_RST | Out | CST820 reset (active low) |
| 32 | TOUCH_SCL | Out | I²C touch clock |
| 33 | TOUCH_SDA | In/Out | I²C touch data |
| 36 | TOUCH_IRQ | In | CST820 interrupt (not used; polling instead) |
| GND | — | — | Shared between all subsystems |
| 3.3V | — | — | Shared between all subsystems |

**Free GPIOs:** 0, 1, 3, 6–11, 16, 17, 22, 26–31, 34–35, 37–39  
(Some of these may be strapped or reserved; consult ESP32 datasheet if adding external hardware.)

---

## Clock Sources & Oscillators

| Component | Type | Speed | Notes |
|-----------|------|-------|-------|
| **Main XTAL** | Ceramic resonator | 40 MHz | Onboard; drives PLL for CPU clock |
| **RTCIO (RTC clock)** | Internal | 150 kHz | Used by RTC + deep-sleep timer |
| **Internal osc** | — | 8 MHz / 8 = 1 MHz | Fallback if XTAL fails |

**SPI Clock Configuration (W.A.S.P.):**
- Display: 40 MHz write, 20 MHz read (typical; adjust in TFT_eSPI User_Setup.h if needed)
- SD card: 25 MHz (default Arduino SD library)
- I²C (touch): 400 kHz (Wire.begin(SDA, SCL, 400000))

---

## Thermal & Reliability

| Parameter | Typical | Notes |
|-----------|---------|-------|
| **Operating Temp** | 0–40°C | Backlight and SD card degrade outside this range |
| **Storage Temp** | −40–85°C | Components rated higher; board not tested below 0°C |
| **Relative Humidity** | 10–90% | Avoid condensation; no conformal coating |
| **Solder Joint Life** | — | Standard FR4 + lead-free solder; ~10 year shelf life if dry |

**Common Failure Modes:**
1. **Backlight flicker:** Capacitor ESR drift or poor USB power; use a 2A+ supply
2. **Touch unresponsive:** I²C bus shorted or CST820 stuck in reset; check GPIO25 logic, SDA/SCL lines
3. **Display ghosting:** SPI contention (display and SD on same bus); ensure CS lines are separate
4. **SD card timeout:** Voltage sag under heavy load; add bulk caps near AMS1117 input if needed
5. **Bootloop:** BOOT (GPIO0) floating low; check for shorted button or loose connector

---

## Variants

**JC2432W328C** (Capacitive, I²C) — **Your Hardware**
- CST820 touch, I²C addr 0x15
- No calibration needed
- Simpler driver code (in-tree ~30 lines)
- Preferred for W.A.S.P. (confirmed stable)

**JC2432W328R** (Resistive, SPI)
- XPT2046 touch, SPI interface
- Requires calibration per board
- More complex driver (library often needed)
- Older variant; rare in 2026
- **Not compatible** with W.A.S.P. Stage 16+ code

---

## References

- **CST820 Datasheet:** Hynitron datasheet (available from supplier or reverse-engineered from protocol)
- **ILI9341 Datasheet:** Ilitek ILI9341 TFT driver (standard; widely documented)
- **ESP32 Datasheet:** Espressif ESP32-WROOM (dual-core, 240 MHz, 320 KB SRAM)
- **TFT_eSPI Library:** https://github.com/Bodmer/TFT_eSPI (Arduino Library Manager)
- **W.A.S.P. Codebase:** `/stage15/nest/` (latest known good configuration)

---

## Quick Setup Checklist

- [ ] **Arduino IDE**: Install ESP32 board support (≥2.0.0, ≤3.x compatible)
- [ ] **TFT_eSPI**: Install via Library Manager; configure User_Setup.h with pins listed above
- [ ] **First Flash**: Use USB cable; hold BOOT if bootloop occurs; monitor at 115200 baud
- [ ] **Touch Test**: Run nest.ino; check serial for `[TDIAG]` and `[TOUCH]` messages
- [ ] **SD Card**: Insert formatted FAT32 card; logs appear in `/logs/` directory
- [ ] **WiFi**: Edit wasp.cfg on SD card; set homeSsid, homePsk for home network
- [ ] **Hardware Review**: Verify no bridged solder, all connectors seated, no pinched wires

---

## Revision History

| Date | Event | Notes |
|------|-------|-------|
| 2026-05-03 | Initial Pack | Confirmed JC2432W328C CST820 capacitive, Stage 16 stable |

