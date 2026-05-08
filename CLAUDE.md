# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

P.A.R. is a website for a custom 37×18 pixel LED matrix display. Users draw pixel art (or upload/crop photos), send them to a queue, and an Arduino device polls the queue and displays each image. A YouTube livestream shows the device live.

## Running locally

```bash
php -S localhost:8000 router.php
```

`router.php` is a PHP dev-server router: it handles PHP files directly, serves static files as-is, and falls back to `index.html` for client-side routes (`/livestream`, `/upload`, `/gallery`).

In production, Apache + `mod_rewrite` in `.htaccess` handles the same routing.

## Architecture

### Frontend (SPA)
`index.html` + `script.js` — a single-page app with three tabs (Livestream, Upload, Gallery) routed via `window.history.pushState`. No build step, no framework; pure vanilla JS.

### PHP backend endpoints
| File | Method | Purpose |
|---|---|---|
| `enqueue.php` | POST | Accepts `{item: base64, name: string}`, appends to `queue.txt`. Max 20 items (429 `queue_full`); duplicate bitmaps rejected (409 `duplicate_queue_item`); name truncated to 100 chars. |
| `next.php` | GET | Pops first item from `queue.txt`, creates `gallery/<N>/pending.json`, sets `X-Gallery-Id` response header, writes `snapshot-pending.flag` containing `<N>`, returns base64 bitmap (called by Arduino) |
| `complete.php` | GET | Renames `gallery/<N>/pending.json` → `info.json` once Arduino confirms display (`?id=N`) |
| `gallery.php` | GET | Returns JSON list of `gallery/<N>/` entries — each has `id`, `pending`, `bitmap`, `name`, and `image` (URL to `image.jpg`/`.png` with `?v=<mtime>` cache-bust, or `null`) |
| `livestream-api.php` | GET | Queries YouTube Data API for live stream from the P.A.R. channel |
| `snapshot-request.php` | POST | Legacy ad-hoc trigger — touches `snapshot-pending.flag` empty (auth: `X-Snapshot-Secret`). The normal flow uses `next.php` to write the flag with a gallery id; this endpoint is only useful for IDless captures. |
| `snapshot-next.php` | POST | Atomically pops `snapshot-pending.flag`, returns `{ok, entry, id}` where `id` is the gallery id from the flag's contents (or `null` for legacy/empty flags). Auth required. |
| `snapshot-clear.php` | POST | Unconditionally removes `snapshot-pending.flag` (auth required) |

### Bitmap format
37×18 = 666 pixels packed as bits into **84 bytes** (the last byte holds 6 padding bits in its low half), transmitted as base64 (112 chars). Bit 1 = cyan (#02b2d9, "on"), bit 0 = black ("off"). The bit order within each byte is MSB-first.

### Data storage
- `queue.txt` — one JSON entry per line: `{"item":"<base64>","name":"<name>"}`
- `gallery/<N>/pending.json` — `{"name":"...","bitmap":"<base64>"}` (present while item is awaiting confirmation)
- `gallery/<N>/info.json` — same schema as pending.json; renamed from pending.json by `complete.php` once confirmed
- `gallery/<N>/image.jpg` (or `.png`) — real photo of the LED matrix displaying entry `N`, captured by YT-Streamer and SFTP'd in
- `snapshot-pending.flag` — exists ⇒ a snapshot is owed; file contents = the gallery id the snapshot belongs to (empty for legacy/ad-hoc captures)
- `locks/` — file-based concurrency slots (`enqueue.N.lock`, `next.N.lock`); up to 5 concurrent requests per endpoint

### Snapshot ↔ gallery flow
End-to-end: `next.php` pops a queue item → creates `gallery/<N>/` + `pending.json` → writes `snapshot-pending.flag` with content `<N>` → returns bitmap with `X-Gallery-Id: <N>` header. Arduino displays for 1s, then `GET /complete.php?id=<N>` (renames `pending.json` → `info.json`). YT-Streamer's snapshot poller hits `snapshot-next.php` every 5s, gets `{id: <N>}`, grabs an RTSP frame via ffmpeg, SFTPs it to `gallery/<N>/image.jpg` (creating that subdir over SFTP if missing). `gallery.php` exposes the photo via `image` URL; the modal renders the cyan bitmap + the real photo side-by-side, falling back to "No P.A.R. image available." when no photo exists yet.

**Timing caveat:** Arduino display duration is 1s but the snapshot poller interval is 5s + ffmpeg grab takes ~1s. With a busy queue, the captured photo can be of a *later* entry than the gallery id it gets attached to. Workarounds (not implemented): bump display duration; have Arduino wait until `image.jpg` for that id appears.

### Environment (`.env`)
```
YT_DATA_KEY = <YouTube Data API v3 key>
SNAPSHOT_SECRET = <shared secret for snapshot endpoints>
```

Format uses ` = ` with spaces, parsed line-by-line in PHP (not `parse_ini_file()`). Keep that format when adding new keys.

## YT-Streamer (Python)

`YT-Streamer/YT_streamer.py` — a 24/7 process that:
1. Streams RTSP from a Eufy camera to YouTube Live via ffmpeg (main thread, auto-reconnects)
2. Polls `snapshot-next.php` every 5 seconds for pending snapshot requests (background thread), grabs a frame with ffmpeg, and uploads it to the server via SFTP

Entirely configured via environment variables (`RTSP_URL`, `YT_STREAM_KEY`, `SNAPSHOT_SECRET`, `SNAPSHOT_REQUEST_URL`, `SFTP_HOST`, `SFTP_USER`, `SFTP_REMOTE_DIR`, `SFTP_PORT` (default 22), `SFTP_KEY_PATH` (default `~/.ssh/id_rsa`), `SFTP_PASSWORD` (optional; key auth used if unset)).

```bash
cd YT-Streamer
source venv/bin/activate
python YT_streamer.py
```

## Arduino

`Arduino Code/P.A.R.Main/P.A.R.Main.ino` — polls `GET /next.php` over HTTPS, decodes the base64 bitmap, drives the display, then polls again. Waits 10 seconds between polls when the queue is empty; waits 10 minutes after a successful display + verify before polling again. WiFi credentials live in `env.h` (not committed).

`Arduino Code/P.A.R.Main/FLOW.md` — concise per-step walkthrough of boot, main loop, color classification, and GRBL streaming for quick reference. **Drifts independently from the .ino** — when you change `flipDisc`, the scan offsets, servo timings, or the post-display delay, update FLOW.md in the same edit. Trust the .ino over FLOW.md when they disagree.

`Arduino Code/SerialBridge/` — USB↔Serial1 passthrough; flash to the Nano RP2040 to send raw G-code to the GRBL Mega from a PC serial monitor.

`Color Sensor ML/ColorSensorStream/` — streams raw R/G/B/C frequency readings every ~15ms; useful for sensor debugging without running the full sketch.

### Hardware
- **Main MCU:** Arduino Nano RP2040 Connect (WiFiNINA for HTTPS, Servo on D9, TCS3200 color sensor S0–S3 + OUT on D4–D8).
- **Motion controller:** Arduino Mega running a slightly modified [grbl-Mega](https://github.com/gnea/grbl-Mega) — `cpu_map` patched for the CNC Shield V3 pinout. Source lives at `~/Documents/Arduino/libraries/grbl-Mega` (additional working dir). Talks to the main MCU over `Serial1` @ 115200 using GRBL's character-counting streaming protocol.
- **Coordinate system:** CNC homes to full negatives, so the work area lives in negative coords. `X_TRAVEL = 777.695`, `Y_TRAVEL = 399.695`; `initGrid()` origins to `(-X_TRAVEL + 25.0, -Y_TRAVEL + 0.0)`. Cell pitch is **20 mm in X, 22 mm in Y**; bitmap `y=0` is the top row, but physical Y increases upward, so Y is mirrored (`(GRID_H-1) - y`) when computing coords.
- **Flip motion:** `flipDisc(x,y)` does a two-stage 180° rotation. Stage 1: servo to 80° (`SERVO_US_ENGAGE`) rotates the squisk 90°, then back to 0° (`SERVO_US_REST`). Stage 2: arc the gantry around the squisk (Y ±20 mm, X +16.8 mm, Y ∓20 mm) so the arm clears the disc, drop the arm to 30° (`SERVO_US_RELEASE`), and slide X −16.8 mm — the 30° arm catches the half-rotated squisk during the slide and pushes it through the final 90°. The Y excursion direction inverts on the bottom row (`gy == GRID_H-1`, at the −Y soft limit); the X excursion is capped per-flip so it never commands past `X=0`. Servo settle: `SERVO_80_DEG_SETTLE_MS = 300` ms for the 80° moves, `SERVO_30_DEG_SETTLE_MS = 100` ms for the 30° moves; `writeServoUs(us, settle_ms)` takes settle as a second arg.
- **FlipCheckerboardTest sync:** `Arduino Code/FlipCheckerboardTest/FlipCheckerboardTest.ino` mirrors motion constants (grid origin/pitch, servo pulse widths, settle times, flip offsets) and shared motion helpers (`moveToYSafe`, `releaseSweep`) from `P.A.R.Main.ino`. Keep both in sync when changing any of these.

### Display flow (post-display verify)
After `displayBitmap`, `loop()` runs `verifyAndFix(bitmap)` in a loop (max 10 passes): scan cells with the TCS3200 (offset `(-23.0, +4.0)` from the flip head), classify blue (`#40ccdb`) vs black via `classifyDisc()` → `classifier_is_blue()` (a tiny ternary transformer; weights in `classifier.h`/`model_weights.h`), re-flip mismatches. **The scan pass iterates `x = 0..GRID_W-2` — the rightmost column is skipped, and the existing `SCAN_OFFSET_X` is set up so the leftmost falls outside the sensor sweep too.** The fix sub-pass still iterates the full grid (the rightmost just never has any wrong bits set). **Only call `complete.php` when the verify pass returns 0 mismatches** — if the cap is hit, leave the gallery entry pending. Boot also runs `$H` then a full `scanGrid()` to seed `gridState[]` (same skip-rightmost rule). The classifier is fed a **5-frame running average of 2 ms-paced RGBC reads** (`tcsReadRGBC` averages 5 frames; matches the train-time distribution).

`releaseSweep()` runs **once after `displayBitmap()` and before the verify-fix loop** (so half-rotated discs settle before the sensor reads them). With the servo parked at `SERVO_US_REST`, it does a serpentine top-to-bottom traverse — no Y-wiggle, no servo movement during the sweep — and routes inter-row Y travel through `moveToYSafe`. Same function in `FlipCheckerboardTest.ino` (called after the checkerboard finishes); keep them in sync.

Sanity-check the live classifier with `Arduino Code/ColorSensorTest/ColorSensorTest.ino` — it prints raw RGBC, `logit`, and `blue`/`black` guess once the 5-frame ring is full. To actually retrain (rather than tweak a threshold), collect new data via `Color Sensor ML/collect.py` (manual `b`/`k` keys) or `Arduino Code/CollectColorTrainingData/` (auto-labeled sweep over a known-pattern board) → `train.py` → `export_header.py` to regenerate `model_weights.h`.

### Color Sensor ML pipeline
`Color Sensor ML/` is a self-contained PyTorch pipeline:
- `collect.py` (manual hotkeys) / `CollectColorTrainingData.ino` (auto-labeled rig sweep) → append RGBC samples to `color_data.json` (schema: `{"blue": [[r,g,b,c], ...], "black": [...]}`).
- `collect_auto.py` — automated serial reader; reads labeled `b,r,g,b,c` / `k,r,g,b,c` lines printed by `CollectColorTrainingData.ino` and appends to `color_data.json` (usage: `python collect_auto.py [--port /dev/cu.usbmodem101]`).
- `eval_running_avg.py` — sanity-check accuracy at the 5-frame averaging window the firmware uses.
- `train.py` → `model.pt` (float) → `export_header.py` → `model_weights.h` (int8, deployed to all four sketch folders).
- `evolve.py` — alternative training path (evolutionary search) for tiny architectures.
- `verify_export.py` — checks the exported int8 weights still match the float model.

**Current model:** big ternary, `d_model=16, d_ff=32`, trained on 5-step averaged data. ~99.85% test accuracy on averaged inputs.

**Full retrain sequence** (run from `Color Sensor ML/` with venv active):
```bash
# 1. Generate 5-step averaged data (always fresh from color_data.json)
python -c "
import json, numpy as np
def ra(a,w):
    cs=np.cumsum(np.vstack([np.zeros((1,4)),a.astype(np.float64)]),0)
    return ((cs[w:]-cs[:-w])/w).astype(np.float32)
d=json.load(open('color_data.json'))
json.dump({'blue':ra(np.array(d['blue']),5).tolist(),'black':ra(np.array(d['black']),5).tolist()},open('/tmp/color_data_avg5.json','w'))
"
# 2. Train
python train.py --data /tmp/color_data_avg5.json --d-model 16 --d-ff 32 --epochs 100
# 3. Export to all four sketch folders
python export_header.py \
  --out ColorClassifier/model_weights.h \
  --out "../Arduino Code/P.A.R.Main/model_weights.h" \
  --out "../Arduino Code/ColorSensorTest/model_weights.h" \
  --out "../Arduino Code/ValidateColorModel/model_weights.h"
# 4. Verify
python verify_export.py
```

**Data quality check before retraining:** if `color_data.json` is healthy, `blue` median B/C ≈ 55–70 and `black` median B/C ≈ 15–25. If both classes have similar medians (or worse, are inverted), the labels are wrong — training will plateau at ~77% with even a float model. Check with:
```bash
python -c "import json,numpy as np; d=json.load(open('color_data.json')); b=np.array(d['blue']); k=np.array(d['black']); print('blue B/C',np.median(b[:,2]/(b[:,3]+1))); print('black B/C',np.median(k[:,2]/(k[:,3]+1)))"
```

**export_header.py defaults** write only 3 paths (ColorClassifier, P.A.R.Main, ColorSensorTest) — always pass all four `--out` args explicitly to also cover ValidateColorModel.

### Sketch conventions
- **Pure-Y motion only at X soft-limits.** Any vertical travel outside `flipDisc` must happen with X pinned at `0` or `-X_TRAVEL`. Use `moveToYSafe(x, y)` (emits `G0 X<limit>` → `G0 Y<targetY>` → `G0 X<targetX>`) for phase entry and any cross-row transition. Row sweeps (`scanGrid`, `displayBitmap`, `verifyAndFix`, `releaseSweep`, the `FlipCheckerboardTest` flip loop) are serpentine — end-of-row X equals start-of-next-row X, so the inter-row leg through `moveToYSafe` lands a pure-Y move at the limit.
- **G0 with one axis omitted holds that axis** in GRBL — `G0 Y<n>` is a pure-Y move at the current X. That's what lets `moveToYSafe` work without tracking gantry position.
- **Recovery from wedged comms:** `sendGcode`/`waitForIdle` carry a 60 s no-progress watchdog that calls `NVIC_SystemReset()` (RP2040 mbed-core CMSIS) — match that pattern for any new long-blocking loop. The legacy `while (true);` on `error`/`ALARM` in `drainResponses` is the one outlier and stays as a deliberate hard-halt for safety-relevant faults.
- **No Arduino `String` for globals.** Heap fragments over weeks of uptime. Stack-locals inside one HTTP call are fine; module-level state goes in `char[N]` (see `pendingGalleryId`).
- **Server-derived strings used in URLs/headers must be validated** before interpolation (see `isDigitsOnlyId`) — defense-in-depth against header injection if the server is ever compromised.

### GRBL gotcha (do not re-litigate)
GRBL acks (`ok`) when a line is **parsed into the planner**, not when motion finishes. To wait for motion to actually complete (e.g. before firing the servo), send `G4 P0` and then `waitForIdle()` — the dwell forces a planner sync inside GRBL, so its `ok` is the real motion-done signal. That's what `waitForMotion()` does.

**Boot order:** when homing is required, GRBL boots into alarm state and rejects G-code with `error:9` ("locked out during alarm"). Always send `$H` **first**, then `G21`/`G90` after homing completes. Don't send any modal G-code before `$H`.

### Color classifier gotcha (do not re-litigate)
Don't classify discs with squared-Euclidean distance over raw RGBC. At 20% TCS3200 scaling, the B-filter pulse can hit ~125 kHz; `b*b` overflows 32-bit `long` and you get garbage negative distances that always pick the same target. Stick with channel ratios (currently B/C) or anything else that stays small.

### HTTPS gotchas (do not re-litigate)

The poll path uses raw `WiFiSSLClient` with a hand-written HTTP request, not `ArduinoHttpClient`. Reasons, all hit during prior debugging:

- The server is behind Cloudflare, which always responds with `Transfer-Encoding: chunked` + `Connection: keep-alive` (no `Content-Length`) over HTTP/1.1. `ArduinoHttpClient`'s chunked decoder + WiFiNINA TLS combo would intermittently hang on the populated-body case (status `-3` = timeout) while the trivial `"NONE"` body worked. Server itself responds in ~250ms — it's a client-library issue, not a server issue.
- `client.stop()` must be called **after** `client.responseBody()`, not before — otherwise the body read blocks on a dead socket. Also call `stop()` between requests; reusing a keep-alive `HttpClient` across GET→POST is fragile on WiFiNINA.
- The `beginRequest()` / `get()` / `sendHeader("Connection","close")` / `endRequest()` pattern returns `HTTP_ERROR_API` (-1) in the version of `ArduinoHttpClient` we have — `startRequest()` only accepts `eIdle`, not `eRequestStarted`. Don't try this approach again; use raw `WiFiSSLClient` instead.
- The raw approach: open a fresh `WiFiSSLClient` per call, write the request manually with `Connection: close`, parse status/headers/body inline, decode chunked if present, then `stop()`. See `fetchNext()` in the .ino.
- Always `client.setHttpResponseTimeout(...)` (or a manual timeout in the raw path) so a stuck call surfaces as an error instead of a forever-hang.
- `ArduinoHttpClient` is still imported and a global `HttpClient client` is defined; `client.setHttpResponseTimeout(15000)` is called in `setup()`. All actual HTTP operations (`fetchNext`, `onDisplayComplete`) bypass it and use raw `WiFiSSLClient` — the global client is vestigial from the migration.

## Frontend gotchas

- **Pixel grid is DOM-as-state.** `.pixel.active` is the source of truth — `captureGridState()` reads it, `restoreGridFromState()` writes it. Don't add a parallel state array.
- **Call `addToHistory()` after any user-initiated pixel mutation** (draw, fill, clear, photo-import). It both persists to `localStorage` and updates the undo buffer; skipping it desyncs both.
- **Modal convention:** `.upload-modal.hidden` toggles visibility, backdrop click dismisses, action buttons use `.queue-modal-confirm` / `.queue-modal-cancel` (or a red destructive variant like `.clear-modal-confirm`).
- **Font Awesome 6.7 is loaded site-wide** via the CDN link in `index.html`. Use `<i class="fa-solid fa-..."></i>` inline; no setup needed.
- **Gallery cards capture `item` in a click-handler closure** at render time (`script.js` `loadGallery`). If the entry transitions from pending → completed server-side, the cached `item.pending` is still `true` until cards re-render, which would otherwise make a completed entry open the modal in the "In progress" state. Fix is two-pronged: `openGalleryItem` re-fetches `gallery.php` on click and resolves the latest entry by id; plus `startGalleryPolling` re-runs `loadGallery({ silent: true })` every 5s while the gallery tab is active so the cards themselves stay fresh.
- **Modal renders bitmap + name unconditionally**, then layers status (pending / snapshot photo / "No image available") on top. Don't reintroduce branches that hide the bitmap during pending — the bitmap is in `pending.json` from the moment `next.php` fires, so it should always show.
- **`gallery.php` rename TOCTOU race:** `complete.php` does `rename(pending.json, info.json)`. There's a small window where `gallery.php` sees `pending.json` exists, then `file_get_contents` returns `false` because the file just got renamed. The endpoint silently `continue`s past such entries, so a single fetch can briefly miss an item mid-transition. The 5s polling masks this in practice.
- **Adding a new SPA tab** takes three edits: nav link `<a data-tab="X">` in `index.html`, `<section id="X" class="tab-content">` in `index.html`, and an `if (pathname === '/X') return 'X';` branch in `getActiveTabFromUrl()`. No server changes — both `router.php` and `.htaccess` fall back to `index.html` for unknown paths.
- **`.tab-content.active` is flex-row by default** — sibling children inside an active tab sit side-by-side. Add `#tabid.tab-content.active { flex-direction: column; }` if you need stacked content (livestream and about already do this).
- **`loadPARLivestream()` clears `.livestream-container.innerHTML`** on every load. Static content for the livestream tab (e.g. taglines, links) must be a sibling of `.livestream-container`, not a child, or it will be wiped.
- **In-page links to other tabs** should use `class="nav-link" data-tab="X"` so the existing `navLinks.forEach` click handler in `script.js` routes them via `pushState` instead of triggering a full page load. They must be present at script load time (the handler captures `navLinks` once).
- **Don't move gallery-entry creation to `enqueue.php`.** Tried this once to make the bitmap appear in the gallery as soon as it was uploaded; user pushed back — they want the bitmap visible only once the Arduino actually picks the item up. Keep entry creation in `next.php`.
