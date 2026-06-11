# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

P.A.R. is a website for a custom 37×18 pixel LED matrix display. Users draw pixel art (or upload/crop photos), send them to a queue, and an Arduino device polls the queue and displays each image. The Mac Mini records the camera feed for each print and uploads the recording to YouTube; the website's Latest tab embeds the most recent upload and the gallery modal embeds each print's recording.

## Repo

GitHub repo name has a **trailing dot**: `Z-Coder672/P.A.R.` Git URL is `https://github.com/Z-Coder672/P.A.R..git` (literal `.` then `.git`). `gh` CLI is not installed on this machine — use raw `git` + `curl` for repo checks.

## Running locally

```bash
php -S localhost:8000 router.php
```

`router.php` is a PHP dev-server router: it handles PHP files directly, serves static files as-is, and falls back to `index.html` for client-side routes (`/latest`, `/upload`, `/gallery`, `/about`).

In production, Apache + `mod_rewrite` in `.htaccess` handles the same routing.

## Architecture

### Frontend (SPA)
`index.html` + `script.js` — a single-page app with four tabs (Latest, Upload, Gallery, About) routed via `window.history.pushState`. No build step, no framework; pure vanilla JS. The Latest tab embeds the most recent upload by reading `latest-video.php` (server-stored, written by `stream-video-id.php`). No YT Data API call from the Latest tab.

### PHP backend endpoints
| File | Method | Purpose |
|---|---|---|
| `enqueue.php` | POST | Accepts `{item: base64, name: string}`, appends to `queue.txt`. Max 20 items (429 `queue_full`); duplicate bitmaps rejected (409 `duplicate_queue_item`); name truncated to 100 chars. |
| `next.php` | POST | Pops first item from `queue.txt`, creates `gallery/<N>/pending.json`, sets `X-Gallery-Id` response header, writes `snapshot-pending.flag` containing `<N>` and `stream-pending.flag` containing `{"id":N,"name":"..."}`, returns base64 bitmap (called by Arduino). **Must be POST**, not GET — Cloudflare/intermediates may silently retry GETs to origin on connect-fail or 5xx, and each retry would pop another queue item the Arduino never sees, leaving orphan `gallery/<N>/pending.json` entries. |
| `complete.php` | GET | Renames `gallery/<N>/pending.json` → `info.json` once Arduino confirms display (`?id=N`) |
| `gallery.php` | GET | Returns JSON list of `gallery/<N>/` entries — each has `id`, `pending`, `bitmap`, `name`, `image` (URL to `image.jpg`/`.png` with `?v=<mtime>` cache-bust, or `null`), and `video_id` (11-char YT id of the uploaded recording for this print, or `null`) |
| `video-status.php` | GET | `?id=<videoId>` → `{exists, live, state}` via YT Data API `videos.list`. Used by the gallery modal and the Latest tab to decide whether to embed: skip when the video doesn't exist (deleted) or is still `live` (legacy broadcast rows). |
| `snapshot-request.php` | POST | Legacy ad-hoc trigger — touches `snapshot-pending.flag` empty (auth: `X-Snapshot-Secret`). The normal flow uses `next.php` to write the flag with a gallery id; this endpoint is only useful for IDless captures. |
| `snapshot-next.php` | POST | Atomically pops `snapshot-pending.flag`, returns `{ok, entry, id}` where `id` is the gallery id from the flag's contents (or `null` for legacy/empty flags). Auth required. |
| `snapshot-clear.php` | POST | Unconditionally removes `snapshot-pending.flag` (auth required) |
| `stream-start.php` | POST | Atomically pops `stream-pending.flag`, returns `{ok, id, name}`. Flags older than 10 min are deleted and treated as empty so a queued-but-never-picked-up print can't trigger a much-later recording. Auth: `SNAPSHOT_SECRET`. |
| `stream-end-set.php` | POST | `{id}` → writes `stream-end.flag` containing `{"id":N}`. Called by the Arduino after its 10-min post-display linger to tell the Mac Mini to stop recording. Stale-flag prophylactic: an existing flag older than 10 min is removed first. Auth: `SNAPSHOT_SECRET`. |
| `stream-end.php` | POST | Atomically pops `stream-end.flag`, returns `{ok, id, entry}`. Flags older than 10 min are deleted and treated as empty (`204`). Auth: `SNAPSHOT_SECRET`. |
| `stream-video-id.php` | POST | `{id, video_id}` → merges `video_id` (11-char `[A-Za-z0-9_-]`) into `gallery/<id>/pending.json` or `info.json`, AND atomically rewrites `latest-video.json` so the Latest tab can find the newest upload without scanning the gallery or hitting the YT Data API. Called by YT-Streamer right after `videos.insert` completes. Auth: `SNAPSHOT_SECRET`. |
| `latest-video.php` | GET | Returns `{video_id, name, id}` from `latest-video.json`, or `204` when no upload has happened yet. No YT Data API call — the answer is whatever YT-Streamer last reported. Public (the Latest tab fetches this on every page load). |

### Bitmap format
37×18 = 666 pixels packed as bits into **84 bytes** (the last byte holds 6 padding bits in its low half), transmitted as base64 (112 chars). Bit 1 = cyan (#02b2d9, "on"), bit 0 = black ("off"). The bit order within each byte is MSB-first.

### Data storage
- `queue.txt` — one JSON entry per line: `{"item":"<base64>","name":"<name>"}`
- `gallery/<N>/pending.json` — `{"name":"...","bitmap":"<base64>","video_id":"<11-char>"?}` (present while item is awaiting confirmation; `video_id` added asynchronously by `stream-video-id.php` after YT-Streamer finishes uploading the recording)
- `gallery/<N>/info.json` — same schema as pending.json; renamed from pending.json by `complete.php` once confirmed
- `gallery/<N>/image.jpg` (or `.png`) — real photo of the LED matrix displaying entry `N`, captured by YT-Streamer and SFTP'd in
- `snapshot-pending.flag` — exists ⇒ a snapshot is owed; file contents = the gallery id the snapshot belongs to (empty for legacy/ad-hoc captures)
- `stream-pending.flag` — exists ⇒ a per-print recording should be started; file contents = JSON `{"id":N,"name":"..."}`. Written by `next.php`, popped by `stream-start.php`, expires after 10 min.
- `stream-end.flag` — exists ⇒ an in-flight recording should be stopped; file contents = JSON `{"id":N}`. Written by `stream-end-set.php` (called by Arduino), popped by `stream-end.php` (called by YT-Streamer), expires after 10 min.
- `latest-video.json` — `{"id":N,"video_id":"...","name":"...","ts":<unix>}` of the most recent upload YT-Streamer reported. Rewritten atomically by `stream-video-id.php` on every upload; read by `latest-video.php` for the Latest tab. The Latest tab reads only this file — it never asks the YT Data API for the latest video id.
- `locks/` — file-based concurrency slots (`enqueue.N.lock`, `next.N.lock`); up to 5 concurrent requests per endpoint

### Snapshot ↔ gallery flow
End-to-end: `next.php` pops a queue item → creates `gallery/<N>/` + `pending.json` → writes `snapshot-pending.flag` with content `<N>` → returns bitmap with `X-Gallery-Id: <N>` header. Arduino displays for 1s, then `GET /complete.php?id=<N>` (renames `pending.json` → `info.json`). YT-Streamer's snapshot poller hits `snapshot-next.php` every 5s, gets `{id: <N>}`, grabs a frame from the local USB webcam via ffmpeg avfoundation — or, while a print is recording, copies the recorder's live JPEG sidecar (`/tmp/recordings/latest_frame.jpg`), since a USB cam allows only one opener — SFTPs it to `gallery/<N>/image.jpg` (creating that subdir over SFTP if missing). `gallery.php` exposes the photo via `image` URL; the modal renders the cyan bitmap + the real photo side-by-side, falling back to "No P.A.R. image available." when no photo exists yet.

**Timing caveat:** Arduino display duration is 1s but the snapshot poller interval is 5s + ffmpeg grab takes ~1s. With a busy queue, the captured photo can be of a *later* entry than the gallery id it gets attached to. Workarounds (not implemented): bump display duration; have Arduino wait until `image.jpg` for that id appears.

### Per-print recording flow
Every print gets its own YouTube upload titled `"<name>" printing - P.A.R.` (no timestamp). End-to-end: `next.php` writes `stream-pending.flag` with `{id,name}` → YT-Streamer's record orchestrator hits `stream-start.php` every `STREAM_POLL_INTERVAL`s, gets `{id,name}`, spawns ffmpeg recording the local USB webcam (avfoundation, **video-only — never audio**) → `/tmp/recordings/<id>_<ts>.mp4` (fragmented mp4, 1.5h hard cap via `-t` on the input; a second output writes the ~1fps `latest_frame.jpg` sidecar) → polls `stream-end.php` every interval; after the Arduino's 10-min post-display linger it POSTs `stream-end-set.php` which writes `stream-end.flag` with `{id}` → YT-Streamer sees the stop signal, sends `q\n` to ffmpeg's stdin for a clean moov flush, then hands the mp4 to a background uploader thread (so the *next* print can start recording immediately) → uploader calls `videos.insert` with `resumable=True`, gets the 11-char video id back, POSTs `{id, video_id}` to `stream-video-id.php` (merges into `gallery/<id>/{pending,info}.json`), and unlinks the local mp4. The gallery modal and the Latest tab both read `video_id` from `gallery.php`, ask `video-status.php` if it exists / isn't still processing, and embed an `<iframe>` of the recording. If the upload fails the mp4 stays in `/tmp/recordings/` for manual recovery.

### Environment (`.env`)
```
YT_DATA_KEY = <YouTube Data API v3 key>
SNAPSHOT_SECRET = <shared secret for snapshot endpoints>
```

Format uses ` = ` with spaces, parsed line-by-line in PHP (not `parse_ini_file()`). Keep that format when adding new keys.

## YT-Streamer (Python)

`YT-Streamer/YT_streamer.py` — a Mac Mini daemon that records per-print video and uploads to YouTube. Three concurrent subsystems:

1. **Record orchestrator** (`record_orchestrator`): polls `stream-start.php` every `STREAM_POLL_INTERVAL` seconds for a `(gallery_id, name)`. On a hit, verifies the camera, spawns ffmpeg recording the local USB webcam (avfoundation, **video-only — `-an` is hard-coded, audio is never captured**) → `/tmp/recordings/<id>_<ts>.mp4` with fragmented-mp4 muxing (`+empty_moov+default_base_moof+frag_keyframe+faststart`) and a 1.5h `-t` cap, **plus a second output writing `/tmp/recordings/latest_frame.jpg` at ~1fps** (the snapshot poller's no-contention source). Then polls `stream-end.php` until the matching stop signal arrives or the cap fires. Graceful stop: `q\n` on stdin → SIGTERM → SIGKILL. The finished mp4 is handed to a *background* uploader thread (so the next print can start recording immediately while a slow upload is still in flight); the uploader calls `videos.insert` with `resumable=True, chunksize=8MiB`, POSTs the resulting 11-char id to `stream-video-id.php`, then unlinks the mp4. Failed uploads stay on disk for manual recovery.
2. **Snapshot poller** (`poll_snapshot_queue`): polls `snapshot-next.php` every 5 s, grabs a webcam frame with ffmpeg avfoundation (or copies `latest_frame.jpg` while a recording holds the cam), SFTPs to `gallery/<id>/image.jpg`.
3. **Moderation poller** (`poll_mod_queue`): polls the mod queue, runs each submission through two Claude SDK calls (image + name).

YouTube auth uses OAuth 2.0 with the `youtube` scope (covers `videos.insert`). Client secrets and refresh token live in an encrypted DMG vault (`YT_streamer_vault.dmg`) protected by a Keychain-stored passphrase — see `_vault_*` and `get_youtube_service`. Auth is lazy: the vault is only mounted when the first recording arrives.

Configured via environment variables: `CAMERA_NAME` (default `Brio 100` — matched case-insensitively against the *start* of each avfoundation video-device name), `CAMERA_FRAMERATE` (default 30), `CAMERA_POLL_INTERVAL` (keeper re-scan cadence, default 15), `SNAPSHOT_SECRET`, `SNAPSHOT_REQUEST_URL`, `STREAM_START_URL`, `STREAM_END_URL`, `STREAM_VIDEO_ID_URL`, `STREAM_POLL_INTERVAL` (default 10), `SFTP_HOST`, `SFTP_USER`, `SFTP_REMOTE_DIR` (chrooted; empty string is valid), `SFTP_PORT` (default 21), `SFTP_PASS_FILE` (default `SFTP-pass.txt`, read from the vault), plus the moderation block (`MOD_QUEUE_URL`, `MOD_ACTION_URL`, `MOD_SECRET`, SMTP creds, etc.).

**Camera is a local USB webcam via ffmpeg avfoundation, NOT RTSP** (refactored away; the RTSP version is archived at `YT-Streamer/archive/`). Gotchas, do not re-litigate:
- **No audio, ever** — the user explicitly never wants audio broadcast. `build_record_cmd` hard-codes `-an`; the input is the bare video index. Don't add audio capture.
- **A USB webcam allows only ONE opener at a time** (unlike RTSP's concurrent readers). The recording owns the device, so the snapshot path must NOT open it during a recording — it copies the recorder's `latest_frame.jpg` sidecar instead (`_read_live_frame`, PIL-validated against torn reads). The orchestrator unlinks the sidecar on stop so stale frames aren't served.
- **Device discovery** = `ffmpeg -f avfoundation -list_devices true -i ""` (writes the list to **stderr** and exits non-zero — both expected). `_refresh_camera` parses it for the video device whose name starts with `CAMERA_NAME` and caches its index; the keeper re-runs this every `CAMERA_POLL_INTERVAL`s whenever not recording (no "warm knocks" — that RTSP-era path is gone). Enumerating doesn't open the cam, so the privacy LED stays off and it never contends with a recording.
- **Test with `./venv/bin/python`** (the venv has `requests`/`PIL`/etc.; system `python3` does not). Importing `YT_streamer` runs the required-env check, so the `.env` must be populated.

**Snapshot upload is FTPS, not SFTP, even though the env var names start with `SFTP_`.** The env-var prefix is historical — the actual transport is `ftplib.FTP_TLS` (explicit TLS on port 21). Reasons, all hit during prior debugging:

- The Site5 *addon* FTP account `yt-streamer@par.zimmzimm.com` is FTP/FTPS only — SSH/SFTP on :22 only accepts the main cPanel user (`zcoder`), so paramiko-as-`yt-streamer` always gets `Authentication failed` regardless of password. cPanel → FTP Accounts → *Configure FTP Client* confirms the documented client config is FTPS on :21, not SFTP.
- `SFTP_HOST` must be the Site5 origin (`shared187.accountservergroup.com`), NOT `par.zimmzimm.com` or `ftp.zimmzimm.com`. Both customer-facing hostnames resolve to Cloudflare (104.21.x / 172.67.x), which proxies 80/443 but does NOT tunnel 21 or 22 — connections to those ports silently time out after ~60s (`Errno 60 Operation timed out`). The HTTP endpoints in `.env` (`SNAPSHOT_REQUEST_URL` etc.) still use `par.zimmzimm.com` because those go over 443. The origin hostname is the same one already used for `SMTP_HOST`.
- TLS verification is disabled (`ssl.CERT_NONE`) because the shared host presents a wildcard cert that doesn't match `shared187.accountservergroup.com`. TLS still encrypts the password and data channels — only authenticity is sacrificed.
- The FTP account is chrooted to the gallery directory, so `SFTP_REMOTE_DIR` is `""` and remote paths are bare `<id>/image.jpg`. Don't reintroduce the full `/home1/zcoder/...` path — the account can't reach above its chroot anyway.
- The password lives in the encrypted DMG vault as `SFTP-pass.txt`, **not** in `.env`. It's read lazily on the first snapshot upload via `_vault_read_sftp_password()` (same vault-mount-then-unmount pattern as the YouTube client secret) and cached in `_sftp_password_cache` for the rest of the process. cPanel → FTP Accounts → Change Password is the place to rotate; update vault file to match.
- Per `upload_snapshot()`: `mkd <id>` is best-effort — `error_perm 550` (already exists) is swallowed, anything else re-raised. `STOR <id>/image.jpg` is the actual upload; success → local mp4/jpg unlinked.

```bash
cd YT-Streamer
source venv/bin/activate
python YT_streamer.py
```

## Arduino

`Arduino Code/PARMain/PARMain.ino` — polls `POST /next.php` over HTTPS, decodes the base64 bitmap, drives the display, then polls again. Waits 10 seconds between polls when the queue is empty; waits 10 minutes after a successful display before polling again, and at the end of that linger POSTs `/stream-end-set.php` with the gallery id so the Mac Mini stops recording (`sendStreamEnd()`). WiFi credentials live in `env.h` (not committed). **Naming:** on-disk dir is `PARMain`.

`Arduino Code/PARMain/FLOW.md` — concise per-step walkthrough of boot, main loop, color classification, and GRBL streaming for quick reference. **Drifts independently from the .ino** — when you change `flipDisc`, the scan offsets, servo timings, or the post-display delay, update FLOW.md in the same edit. Trust the .ino over FLOW.md when they disagree.

`Arduino Code/SerialBridge/` — USB↔Serial1 passthrough; flash to the Nano RP2040 to send raw G-code to the GRBL Mega from a PC serial monitor.

`Color Sensor ML/ColorSensorStream/` — streams raw R/G/B/C frequency readings every ~15ms; useful for sensor debugging without running the full sketch.

### Hardware
- **Main MCU:** Arduino Nano RP2040 Connect (WiFiNINA for HTTPS, Servo on D9, TCS3200 color sensor S0–S3 + OUT on D4–D8).
- **Motion controller:** Arduino Mega running a slightly modified [grbl-Mega](https://github.com/gnea/grbl-Mega) — `cpu_map` patched for the CNC Shield V3 pinout. Source lives at `~/Documents/Arduino/libraries/grbl-Mega` (additional working dir). Talks to the main MCU over `Serial1` @ 115200 using GRBL's character-counting streaming protocol.
- **Coordinate system:** CNC homes to full negatives, so the work area lives in negative coords. `X_TRAVEL = 777.695`, `Y_TRAVEL = 399.695`; `initGrid()` origins to `(-X_TRAVEL + 25.0, -Y_TRAVEL + 0.0)`. Cell pitch is **20.045 mm in X, 23.40 mm in Y**; bitmap `y=0` is the top row, but physical Y increases upward, so Y is mirrored (`(GRID_H-1) - y`) when computing coords. **`Y_TRAVEL` (firmware grid geometry) is deliberately NOT equal to GRBL `$131` (Y soft-limit max travel, now `402`).** They used to match at `399.695`, which put the bottom row's flip target *exactly* on the `-$131` soft-limit edge — and since `399.695 × 40 steps/mm = 15987.8` rounds to `-399.70`, that edge intermittently rounded one microstep past the limit and threw `ALARM:2`. Raising `$131` to `402` gives ~2.3 mm of headroom so the bottom row sits inside the envelope. Don't "re-sync" `Y_TRAVEL` to `$131` — the board hasn't moved, so changing `Y_TRAVEL` would shift the whole pattern. **Separately, the `+Y` (Y=0) edge:** `scanGrid` adds `SCAN_OFFSET_Y` (+4.0) to center the sensor, so the top bitmap row's scan target was `+2.105` — past `Y=0`, a hard `ALARM:2` that no `$131` value fixes (GRBL caps the work area at 0 regardless). `clampScanY()`/`SCAN_Y_MAX` now pin every scan Y just inside the envelope (top row reads ~2 mm low, still on the disc). The flip head itself never goes positive (top-row flip Y = `-1.895`). Also: `drainResponses` now logs the in-flight G-code on `ALARM` (`GRBL ALARM: <code> on '<cmd>'`) so the next plog dump names the exact offending move.
- **Flip motion:** `flipDisc(x,y,catchByNextMove)` does a two-stage 180° rotation. Stage 1: servo to 90° (`SERVO_US_ENGAGE`) rotates the squisk 90°, then back to 0° (`SERVO_US_REST`). Stage 2: slide X +16.8 mm so the arm clears the disc column, drop the arm to 50° (`SERVO_US_RELEASE`), and slide X −16.8 mm — the 50° arm catches the half-rotated squisk during the return slide and pushes it through the final 90°. The X excursion is capped per-flip so it never commands past `X=0`. Servo settle: `SERVO_90_DEG_SETTLE_MS = 300` ms for the 90° moves, `SERVO_50_DEG_SETTLE_MS = 100` ms for the 50° moves; `writeServoUs(us, settle_ms)` takes settle as a second arg.
- **Second-catch pass (`#define FLIP_SECOND_CATCH`, default OFF):** an optional step 3 after Stage 2 — drop the arm a further ~10° (`SERVO_US_RELEASE2`, ~28°) and sweep +X again to push back any disc the first catch left over/under-rotated. Gated behind a commented-out `//#define FLIP_SECOND_CATCH` near `FLIP_OFFSET_X`; **uncomment to re-enable**. When OFF the arm is parked at REST after Stage 2 and `catchByNextMove` is ignored (the `(void)catchByNextMove;` in the `#else`). When ON, `catchByNextMove=true` leaves the arm down at `RELEASE2` so the caller's next +X move performs the sweep (no extra G-code); `false` emits an explicit +X stroke and re-parks at REST. The same `#define` + `#ifdef`/`#else` block is mirrored in `FlipCheckerboardTest.ino` — keep both in sync. `FlipCornersTest.ino` has no second-catch pass (its `flipDisc` is the simpler 2-arg form).
- **FlipCheckerboardTest sync:** `Arduino Code/FlipCheckerboardTest/FlipCheckerboardTest.ino` mirrors motion constants (grid origin/pitch, servo pulse widths, settle times, flip offsets), the `FLIP_SECOND_CATCH` toggle, and shared motion helpers (`moveToYSafe`, `releaseSweep`) from `PARMain.ino`. Keep both in sync when changing any of these.

### Display flow
Each job in `loop()` runs: `scanGrid()` (re-home + re-read every cell to reseed `gridState[]` after the 10-min idle) → `displayBitmap(bitmap)` → **check pass** (`scanGrid()` + `displayBitmap(bitmap)` again) → `onDisplayComplete()` → `delay(10 min)`. The check pass re-scans the physical board after the first draw — reseeding `gridState[]` so any disc that didn't flip cleanly or was misclassified is caught — then re-runs `displayBitmap`, whose diff-against-`gridState` logic re-flips only the cells still wrong. One pass, not a loop-until-clean. Boot also runs `$H` then a full `scanGrid()` to seed `gridState[]`.

Cell scanning uses the TCS3200 (offset `(-23.0, +4.0)` from the flip head), classifying blue (`#40ccdb`) vs black via `classifyDisc()` → `classifier_is_blue()` (a tiny ternary transformer; weights in `classifier.h`/`model_weights.h`). The classifier is fed a **5-frame running average of 2 ms-paced RGBC reads** (`tcsReadRGBC` averages 5 frames; matches the train-time distribution).

`releaseSweep()` runs **once after `displayBitmap()`** so half-rotated discs settle before the snapshot photo is taken. With the servo parked at `SERVO_US_REST`, it does a serpentine top-to-bottom traverse — no Y-wiggle, no servo movement during the sweep — and routes inter-row Y travel through `moveToYSafe`. Same function in `FlipCheckerboardTest.ino` (called after the checkerboard finishes); keep them in sync.

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
  --out "../Arduino Code/PARMain/model_weights.h" \
  --out "../Arduino Code/ColorSensorTest/model_weights.h" \
  --out "../Arduino Code/ValidateColorModel/model_weights.h"
# 4. Verify
python verify_export.py
```

**Data quality check before retraining:** if `color_data.json` is healthy, `blue` median B/C ≈ 55–70 and `black` median B/C ≈ 15–25. If both classes have similar medians (or worse, are inverted), the labels are wrong — training will plateau at ~77% with even a float model. Check with:
```bash
python -c "import json,numpy as np; d=json.load(open('color_data.json')); b=np.array(d['blue']); k=np.array(d['black']); print('blue B/C',np.median(b[:,2]/(b[:,3]+1))); print('black B/C',np.median(k[:,2]/(k[:,3]+1)))"
```

**export_header.py defaults** write only 3 paths (ColorClassifier, PARMain, ColorSensorTest) — always pass all four `--out` args explicitly to also cover ValidateColorModel.

### Sketch conventions
- **Pure-Y motion only at X soft-limits.** Any vertical travel outside `flipDisc` must happen with X pinned at `0` or `-X_TRAVEL`. Use `moveToYSafe(x, y)` (emits `G0 X<limit>` → `G0 Y<targetY>` → `G0 X<targetX>`) for phase entry and any cross-row transition. Row sweeps (`scanGrid`, `displayBitmap`, `verifyAndFix`, `releaseSweep`, the `FlipCheckerboardTest` flip loop) are serpentine — end-of-row X equals start-of-next-row X, so the inter-row leg through `moveToYSafe` lands a pure-Y move at the limit.
- **G0 with one axis omitted holds that axis** in GRBL — `G0 Y<n>` is a pure-Y move at the current X. That's what lets `moveToYSafe` work without tracking gantry position.
- **Recovery from wedged comms:** `sendGcode`/`waitForIdle` carry a 60 s no-progress watchdog that calls `NVIC_SystemReset()` (RP2040 mbed-core CMSIS) — match that pattern for any new long-blocking loop. `drainResponses` no longer hard-halts on GRBL faults: `ALARM` triggers `grblAlarmRecover()` (Ctrl-X soft reset → `$H` → reassert modals → clear queue → force re-scan next job), and `error:N` is retried up to 10× with 3 s spacing per same-command run before falling back to MCU reset. `errorRetryCount` resets on any clean `ok`. The command queue stores text per slot (`cmdTexts[QUEUE_SIZE][MAX_CMD_LEN]`) so retries can re-send.
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
- **`.tab-content.active` is flex-row by default** — sibling children inside an active tab sit side-by-side. Add `#tabid.tab-content.active { flex-direction: column; }` if you need stacked content (latest and about already do this).
- **`loadLatestRecording()` clears `.latest-container.innerHTML`** on every load. Static content for the latest tab (e.g. taglines, links) must be a sibling of `.latest-container`, not a child, or it will be wiped.
- **In-page links to other tabs** should use `class="nav-link" data-tab="X"` so the existing `navLinks.forEach` click handler in `script.js` routes them via `pushState` instead of triggering a full page load. They must be present at script load time (the handler captures `navLinks` once).
- **Don't move gallery-entry creation to `enqueue.php`.** Tried this once to make the bitmap appear in the gallery as soon as it was uploaded; user pushed back — they want the bitmap visible only once the Arduino actually picks the item up. Keep entry creation in `next.php`.
