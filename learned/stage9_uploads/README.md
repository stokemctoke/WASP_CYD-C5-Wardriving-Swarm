# Stage 9 — WiGLE + WDGWars Upload

**Goal:** Nest reads credentials from SD card at boot and uploads aggregated scan files to WiGLE and WDGWars automatically when home WiFi is available — no recompile needed to change credentials.

## What it does

On boot the nest reads `/wasp.cfg` from its SD card (see `wasp.cfg.example` in the repo root):

```
homeSsid=YourHomeWiFi
homePsk=YourPassword
apSsid=WASP-Nest
apPsk=waspswarm
wigleBasicToken=<encoded token from wigle.net>
wdgwarsApiKey=<64-char hex key from wdgwars.pl>
```

AP credentials come from config — no hardcoded values. Missing keys fall back to safe defaults (`WASP-Nest` / `waspswarm`). Unknown keys are logged and skipped.

Upload flow (to be completed):
1. Nest connects to home WiFi using `homeSsid` / `homePsk`
2. Iterates `.csv.done` files under `/logs/` on SD
3. POSTs each to WiGLE (`api.wigle.net/api/v2/file/upload` — Basic auth with encoded token)
4. POSTs each to WDGWars (`wdgwars.pl/api/upload-csv` — `X-API-Key` header)
5. Renames successfully uploaded files to `.uploaded`

## WDGWars API

Method 1 (CSV upload) — recommended for firmware:
- `POST https://wdgwars.pl/api/upload-csv`
- Header: `X-API-Key: <64-char hex key>`
- Body: `multipart/form-data`, field `file` = WiGLE CSV

WiGLE CSV format is accepted directly — no conversion needed.

## Status

- [x] `wasp.cfg` parser — reads all credentials at boot
- [ ] Home WiFi STA connection
- [ ] WiGLE upload
- [ ] WDGWars upload

## Files

| File | Board |
|---|---|
| `nest/nest.ino` | CYD (ESP32 Dev Module) |
