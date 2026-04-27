# Stage 12 — Home Upload (WiGLE + WDGWars)

Closes the full data pipeline: when the Nest is back in range of home WiFi
it automatically uploads collected worker CSVs to WiGLE and WDGWars.

---

## What's new

### Automatic home upload (Nest only)

Every 5 minutes the Nest checks whether home WiFi is configured and whether
there are `.csv` files in `/logs/MACADDR/` waiting to go up. If so:

1. ESP-NOW and the worker AP are temporarily suspended.
2. Nest connects to home WiFi as a pure STA.
3. Every `.csv` under `/logs/` is uploaded to WiGLE and/or WDGWars via
   HTTPS multipart POST, reading each file into RAM first (same SD+WiFi
   DMA fix used in the worker sync path).
4. Successfully uploaded files are renamed `.done`.
5. AP + ESP-NOW are restored — workers reconnect on their next heartbeat.

### WiGLE upload

- Endpoint: `https://api.wigle.net/api/v2/file/upload`
- Auth: `Authorization: Basic <wigleBasicToken>` from `wasp.cfg`
- The token is the **"Encoded for use"** value from wigle.net → Account → API Token

### WDGWars upload

- Endpoint: `https://wdgwars.pl/api/v1/upload` *(verify from your wdgwars.pl profile)*
- Auth: `X-Api-Key: <wdgwarsApiKey>` from `wasp.cfg` *(verify header name from profile)*
- The API key is the 64-hex-char key from wdgwars.pl → Profile → API Keys

> **Before your first wardrive:** confirm the WDGWars endpoint and header name
> from your profile page and update `stage12_home_upload/nest/nest.ino` if they differ.

### Display changes

| Element | Change |
|---|---|
| Status bar | `H` indicator added (right side) — cyan = last upload OK, amber = fail, grey = configured but not yet attempted |
| Footer | Cycles every second: Sync → WiGLE result → WDGWars result → Sync |

### LED

| State | Colour | Pattern |
|---|---|---|
| Upload in progress | Cyan solid | On during connect + upload |
| Upload success | Green | 2× flash |
| Upload fail / home unreachable | Red | 3× flash |

### wasp.cfg additions

```
# Home Wi-Fi — Nest connects here to upload to WiGLE and WDGWars
homeSsid=YourHomeWiFiName
homePsk=YourHomeWiFiPassword

# WiGLE 'Encoded for use' token — wigle.net → Account → API Token
wigleBasicToken=YourWiGLEEncodedToken

# WDGWars API key — wdgwars.pl → Profile → API Keys (64 hex chars)
wdgwarsApiKey=0000000000000000000000000000000000000000000000000000000000000000
```

---

## Flash targets

| Board | File |
|---|---|
| Nest (CYD) | `nest/nest.ino` |
| Worker / Drone (XIAO ESP32-C5) | `worker/worker.ino` (unchanged from Stage 11) |

Arduino IDE settings are unchanged from Stage 11.
