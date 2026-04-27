# Stage 10 — Chunked Upload + Website Colour Scheme

Adds chunked TCP upload so files of any size can be transferred reliably,
and updates the Nest display to match the stokemctoke.com colour palette.

---

## What's new

### Chunked upload (worker + nest)

The ESP32-C5's TCP send path stalls on single payloads above ~9.5 KB.
Stage 9 capped new log files at 8 KB (safely under that ceiling) but left
pre-existing oversized files stuck as `.toobig` with no recovery path.

Stage 10 replaces the dead-end with a chunked protocol:

```
UPLOAD_CHUNK <MAC> <FILENAME> <CHUNK_INDEX> <TOTAL_CHUNKS> <CHUNK_SIZE>\n
```

- Worker splits any file larger than `CHUNK_SIZE` (8 KB) into sequential
  chunks, each uploaded as a separate TCP connection.
- Each connection reads the chunk from SD **before** connecting to WiFi,
  preserving the SD+WiFi DMA coexistence fix from Stage 9.
- Nest appends each received chunk to the same file; chunk 0 truncates
  first so retried transfers start clean.
- On full success the worker renames to `.done` as before.
- At the start of every sync, the worker renames any `.toobig` files back
  to `.csv` so they enter the chunked path automatically.

### Colour scheme

Nest display updated to match stokemctoke.com:

| Role | Hex | RGB565 |
|---|---|---|
| Background | `#000111` | `0x0002` |
| Header | `#FAA307` amber | `0xFD00` |
| Header text | `#F4F6F8` near-white | `0xF7BF` |
| Active worker | `#FFFF00` yellow | `0xFFE0` |
| Stale worker | `#FAA307` amber | `0xFD00` |
| Offline worker | `#2B2B2B` dark grey | `0x2945` |
| Labels / dividers | `#9CA3AF` slate | `0x9D15` |
| GPS fix | `#00FFFF` cyan | `0x07FF` |
| GPS absent | `#9CA3AF` grey | `0x9D15` |

---

## Flash targets

| Board | File |
|---|---|
| Nest (CYD) | `nest/nest.ino` |
| Worker / Drone (XIAO ESP32-C5) | `worker/worker.ino` |

Arduino IDE settings for the Worker are unchanged from Stage 9.
