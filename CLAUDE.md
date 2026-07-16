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
| `enqueue.php` | POST | Accepts `{item: base64, name: string, email?: string}`, appends to `mod_queue.txt`. Max 20 items (429 `queue_full`); duplicate bitmaps rejected (409 `duplicate_queue_item`); name truncated to 100 chars. Optional `email` (validated) is **never** written to the queue — it's encrypted off-webroot keyed by the entry id (see Email notifications); the entry carries only a boolean `notify`. Rate limited (see Rate limiting) — 429 `rate_limited`. |
| `next.php` | POST | Pops first item from `queue.txt`, creates `gallery/<N>/pending.json`, sets `X-Gallery-Id` response header, writes `stream-pending.flag` containing `{"id":N,"name":"..."}`, returns base64 bitmap (called by Arduino). Does **not** arm `snapshot-pending.flag` — the Arduino does that via `snapshot-request.php` after its check (fix) pass, so the photo captures the corrected board rather than a mid-draw state. **Must be POST**, not GET — Cloudflare/intermediates may silently retry GETs to origin on connect-fail or 5xx, and each retry would pop another queue item the Arduino never sees, leaving orphan `gallery/<N>/pending.json` entries. |
| `complete.php` | GET | **Redundant fallback.** Renames `gallery/<N>/pending.json` → `info.json` (`?id=N`). `snapshot-request.php` now performs this same rename as part of the single "print done" event, so by the time the Arduino's `complete.php` call lands the entry is usually already finalized and this no-ops. Kept because the Arduino still calls it (and expects 2xx) and it covers the legacy IDless snapshot path; the system no longer **depends** on it succeeding (so a mid-job device reboot that drops it can't strand the entry as pending). |
| `gallery.php` | GET | Returns JSON list of `gallery/<N>/` entries — each has `id`, `pending`, `bitmap`, `name`, `image` (URL to `image.jpg`/`.png` with `?v=<mtime>` cache-bust, or `null`), and `video_id` (11-char YT id of the uploaded recording for this print, or `null`). `pending` is **derived**, not just "has `pending.json`": it's `true` only when the entry is still `pending.json` **and** has no photo. A photo only appears post-display, so `image` present forces `pending=false` — a finished print with a real photo never reads as "In progress" even if its `pending.json → info.json` rename was lost. |
| `video-status.php` | GET | `?id=<videoId>` → `{exists, live, state}` via YT Data API `videos.list`. Used by the gallery modal and the Latest tab to decide whether to embed: skip when the video doesn't exist (deleted) or is still `live` (legacy broadcast rows). |
| `snapshot-request.php` | POST | The single server-side "print done" event. Arms `snapshot-pending.flag` (auth: `X-Snapshot-Secret`) **and** finalizes the gallery entry by renaming `gallery/<N>/pending.json` → `info.json` (same logic as `complete.php`; idempotent, no-op if no pending entry). Accepts optional `id=<N>` form param (digits-only), written as the flag contents and used for the rename; absent/invalid id writes an empty flag and skips the rename (legacy IDless capture). The Arduino POSTs it with the gallery id right after its check (fix) pass finishes — driving the photo, the recording stop (via the flag), and completion from one call. |
| `snapshot-next.php` | POST | Atomically pops `snapshot-pending.flag`, returns `{ok, entry, id}` where `id` is the gallery id from the flag's contents (or `null` for legacy/empty flags). Auth required. |
| `snapshot-clear.php` | POST | Unconditionally removes `snapshot-pending.flag` (auth required) |
| `stream-start.php` | POST | Atomically pops `stream-pending.flag`, returns `{ok, id, name}`. Flags older than 10 min are deleted and treated as empty so a queued-but-never-picked-up print can't trigger a much-later recording. Auth: `SNAPSHOT_SECRET`. |
| `stream-end-set.php` | POST | **Vestigial** — the Arduino no longer calls this. Formerly wrote `stream-end.flag` to stop a recording; recordings now stop on the snapshot signal (see Per-print recording flow). Left in place (no server change); auth: `SNAPSHOT_SECRET`. |
| `stream-end.php` | POST | **Vestigial** — YT-Streamer no longer polls this. Formerly popped `stream-end.flag`. Left in place; auth: `SNAPSHOT_SECRET`. |
| `stream-video-id.php` | POST | `{id, video_id}` → merges `video_id` (11-char `[A-Za-z0-9_-]`) into `gallery/<id>/pending.json` or `info.json`, AND atomically rewrites `latest-video.json` so the Latest tab can find the newest upload without scanning the gallery or hitting the YT Data API. Called by YT-Streamer right after `videos.insert` completes. Auth: `SNAPSHOT_SECRET`. |
| `latest-video.php` | GET | Returns `{video_id, name, id}` from `latest-video.json`, or `204` when no upload has happened yet. No YT Data API call — the answer is whatever YT-Streamer last reported. Public (the Latest tab fetches this on every page load). |

### Bitmap format
37×18 = 666 pixels packed as bits into **84 bytes** (the last byte holds 6 padding bits in its low half), transmitted as base64 (112 chars). Bit 1 = cyan (#02b2d9, "on"), bit 0 = black ("off"). The bit order within each byte is MSB-first.

### Data storage
- `queue.txt` — one JSON entry per line: `{"item":"<base64>","name":"<name>"}`
- `gallery/<N>/pending.json` — `{"name":"...","bitmap":"<base64>","video_id":"<11-char>"?}` (present while item is awaiting confirmation; `video_id` added asynchronously by `stream-video-id.php` after YT-Streamer finishes uploading the recording)
- `gallery/<N>/info.json` — same schema as pending.json; renamed from pending.json by `snapshot-request.php` (the primary path) or `complete.php` (the redundant fallback), whichever fires first, once display is done
- `gallery/<N>/image.jpg` (or `.png`) — real photo of the LED matrix displaying entry `N`, captured by YT-Streamer and SFTP'd in
- `snapshot-pending.flag` — exists ⇒ a snapshot is owed; file contents = the gallery id the snapshot belongs to (empty for legacy/ad-hoc captures). Written by `snapshot-request.php` (Arduino, post-check-pass), popped by `snapshot-next.php`.
- `stream-pending.flag` — exists ⇒ a per-print recording should be started; file contents = JSON `{"id":N,"name":"..."}`. Written by `next.php`, popped by `stream-start.php`, expires after 10 min.
- `stream-end.flag` — **vestigial**, no longer written or read. Recordings now stop on the snapshot signal (see Per-print recording flow). The `stream-end-set.php`/`stream-end.php` endpoints remain in place but unused.
- `latest-video.json` — `{"id":N,"video_id":"...","name":"...","ts":<unix>}` of the most recent upload YT-Streamer reported. Rewritten atomically by `stream-video-id.php` on every upload; read by `latest-video.php` for the Latest tab. The Latest tab reads only this file — it never asks the YT Data API for the latest video id.
- `locks/` — file-based concurrency slots (`enqueue.N.lock`, `next.N.lock`); up to 5 concurrent requests per endpoint
- **`PRIVATE_DIR/` (ABOVE the document root, default `../par-private`, `.env`-overridable)** — holds everything a web client must never reach. Created 0700 on first use with its own `Require all denied` .htaccess as defense-in-depth. Contains `emails/` (encrypted submitter addresses; see Email notifications) and `ratelimit/` (token-bucket state). Never inside the webroot; not committed.

### Snapshot ↔ gallery flow
End-to-end: `next.php` pops a queue item → creates `gallery/<N>/` + `pending.json` → returns bitmap with `X-Gallery-Id: <N>` header. Arduino scans, draws, runs its check (fix) pass, then POSTs `/snapshot-request.php` with `id=<N>` — which writes `snapshot-pending.flag` with content `<N>` **and** renames `pending.json` → `info.json` (finalizing the entry). The Arduino then also `GET`s `/complete.php?id=<N>`, but that's now a redundant fallback — the entry is already `info.json`, so it no-ops. Folding completion into the snapshot request means one post-display call (not two) has to land for the entry to finalize and the recording to stop, so a dropped/never-retried second call can't leave a print stuck pending. YT-Streamer's snapshot poller hits `snapshot-next.php` every 5s, gets `{id: <N>}`, grabs a frame from the local USB webcam via ffmpeg avfoundation — or, while a print is recording, copies the recorder's live JPEG sidecar (`/tmp/recordings/latest_frame.jpg`), since a USB cam allows only one opener — SFTPs it to `gallery/<N>/image.jpg` (creating that subdir over SFTP if missing). `gallery.php` exposes the photo via `image` URL; the modal renders the cyan bitmap + the real photo side-by-side, falling back to "No P.A.R. image available." when no photo exists yet.

**Timing:** the flag is armed only after the check pass, and the board then sits untouched through the 10-min post-display linger — so the poller's 5s interval + ~1s grab comfortably captures the correct, corrected board. (The old flow armed the flag in `next.php` at pop time, which let the poller photograph a mid-draw board.)

### Per-print recording flow
Every print gets its own YouTube upload titled `"<name>" printing - P.A.R.` (no timestamp). End-to-end: `next.php` writes `stream-pending.flag` with `{id,name}` → YT-Streamer's record orchestrator hits `stream-start.php` every `STREAM_POLL_INTERVAL`s, gets `{id,name}`, starts a **native AVFoundation capture session** (`AVFRecorder`, PyObjC — **NOT ffmpeg**, **video-only — never audio**) recording the local USB webcam → `/tmp/recordings/<id>_<ts>.mov` (1.5h hard cap via a deadline check; a throttled `AVCaptureVideoDataOutput` writes the ~1fps `latest_frame.jpg` sidecar). ffmpeg can only request the cam's *uncompressed* formats, which over USB 2.0 cap 1080p at ~5fps; AVCaptureSession selects the MJPEG-backed `420v` 1080p30 device format (what Photo Booth uses) for a true 1080p30. **The recording stops on the snapshot signal — the unified "print done" trigger.** When the snapshot poller captures this print's snapshot (display-done), it records the gallery id + a timestamp; the record orchestrator sees that its in-flight id matches and stops. The snapshot flag is armed once per print — by the Arduino's `snapshot-request.php` after its check (fix) pass, minutes into the recording (`next.php` no longer arms it; see Snapshot ↔ gallery flow). As a safety floor against a spuriously early signal, the stop is only honored if the snapshot signal landed at least `MIN_RECORD_SECONDS` (60s) into the recording. A snapshot popped with `id=null` is tied to the in-flight recording's id. **Consequence:** the recording ends at display-done, not display-done + the Arduino's 10-min linger (that static window is no longer recorded). The 1.5h cap remains the backstop if a snapshot is never captured. On stop: `AVCaptureMovieFileOutput.stopRecording()` finalizes the moov, then YT-Streamer hands the mov to a background uploader thread (so the *next* print can start recording immediately) → uploader calls `videos.insert` with `resumable=True`, gets the 11-char video id back, POSTs `{id, video_id}` to `stream-video-id.php` (merges into `gallery/<id>/{pending,info}.json`), and unlinks the local mov. The gallery modal and the Latest tab both read `video_id` from `gallery.php`, ask `video-status.php` if it exists / isn't still processing, and embed an `<iframe>` of the recording. If the upload fails the mov stays in `/tmp/recordings/` for manual recovery.

The old stop path (Arduino POSTs `stream-end-set.php` after its 10-min linger → `stream-end.flag` → YT-Streamer polls `stream-end.php`) is **retired**: it could orphan a recording until the 1.5h cap if the Arduino crashed between display-done and the stream-end POST. The endpoints/flag remain in place (no server change) but are unused; `sendStreamEnd()` was removed from the sketch.

### Environment (`.env`)
```
YT_DATA_KEY = <YouTube Data API v3 key>
SNAPSHOT_SECRET = <shared secret for snapshot endpoints>
SITE_BASE_URL = https://par.zimmzimm.com   (used to build the gallery link in notification emails)
NOTIFY_FROM = no-reply@par.zimmzimm.com    (From/envelope-sender of notification emails)
# PRIVATE_DIR = /home1/zcoder/par-private   (optional; absolute path to the off-webroot store. Defaults to a sibling of the docroot.)
```

Format uses ` = ` with spaces, parsed line-by-line in PHP (not `parse_ini_file()`). Keep that format when adding new keys. `.env` is gitignored, so **these keys must be added to the production `.env` by hand** — deploying code alone won't create them.

**The email-encryption key is deliberately NOT in `.env`.** It lives in a file **above the document root** at `PRIVATE_DIR/email.key` (base64 of a 32-byte libsodium secretbox key), read by `par_email_key()` in `lib/private_store.php` — never from the webroot, so a webroot-only exposure (a misconfigured server serving `.env`, a `/​.env` scanner) can't reach it. Generate + install (no shell needed on the server — upload to `~/par-private/email.key` via cPanel File Manager, which can reach above `public_html`):
```
php -r 'echo base64_encode(sodium_crypto_secretbox_keygen());' > email.key   # then upload to PRIVATE_DIR/email.key (chmod 600)
```
Required for notifications: without a valid `email.key` enqueue still succeeds but no address is stored and no email is sent (fail-safe). Rotating the key orphans any already-stored (unbound/undelivered) addresses.

### Email notifications (submitter "your piece is done" emails)
Opt-in: the upload name modal has an optional email field. The address is treated as sensitive throughout — it is **never** written to any web-served file (`queue.txt`, `mod_queue.txt`, `gallery/<N>/*.json`) and never returned by any endpoint. Instead it is **encrypted at rest (libsodium `crypto_secretbox`) and stored under `PRIVATE_DIR/emails/`, above the document root**, keyed by an opaque id. Only that id travels through the queue files. The encryption **key itself is also off-webroot** at `PRIVATE_DIR/email.key` (never in `.env`; see Environment) — so both the ciphertext and the key are outside the served tree.

Lifecycle (helpers in `lib/private_store.php`):
1. `enqueue.php` → `par_store_submission_email($subId, …)` writes `emails/sub-<subId>.enc`; the `mod_queue.txt` entry gets `notify:true` (no address).
2. `mod-action.php` approve → carries `sub_id` into the promoted `queue.txt` entry; reject → `par_delete_submission_email()` discards the stored address.
3. `next.php` (Arduino pickup) → `par_bind_email_to_gallery($subId, $N)` renames `sub-<subId>.enc → gid-<N>.enc`.
4. `snapshot-request.php` (the "print done" event) → `par_notify_gallery_complete($N)`: **atomically claims** `gid-<N>.enc → .sending` (so device retries and the redundant `complete.php` call can't double-send), decrypts, sends via PHP `mail()`, then marks `.sent` (or restores `.enc` on send failure so a later retry can resend). `complete.php` calls the same idempotent hook as a fallback.

The notification email is HTML, links to `SITE_BASE_URL/gallery`, and pins its envelope sender (`-f`) for SPF alignment. All header-bearing values are CR/LF-stripped and the recipient is re-validated at send time (header-injection defense). Delivery uses `mail()` (local Exim on Site5) — no SMTP creds needed in the web `.env`; the address never leaves the web server. **Depends on the host's `mail()` working**; if Site5 mail is unreliable, swap `par_send_completion_email()` to authenticated SMTP.

### Rate limiting (`lib/ratelimit.php`)
Goal: keep request/CPU load low enough that Site5 shared hosting never flags the account. Each protected endpoint gets **two file-based token buckets** (state in `PRIVATE_DIR/ratelimit/`): a best-effort **per-IP** bucket (keyed on `CF-Connecting-IP`, since the origin only sees a few Cloudflare edge IPs as `REMOTE_ADDR`) *and* a **global** bucket that caps total traffic to that endpoint regardless of source IP. The global bucket is the real ban protection — it holds even if the per-IP key is spoofed by hitting the origin directly. Breach → `429` + `Retry-After`; **fails open** on any FS error (availability over strictness). Applied to `enqueue` (strict: ~5/min per IP, ~60/min global), and the polled public reads `gallery`/`queue`/`latest-video`/`video-status` (generous, tuned so the frontend's 5s gallery poll and per-load latest fetch never trip; `video-status` limits also shield the YT Data API quota). **Not** applied to `next.php` (Arduino) or the `SNAPSHOT_SECRET`-authed endpoints, so the device/daemon can't be locked out. `lib/`, `par-private/`, and all raw data files (`*.enc`, `*.flag`, `mod_queue.txt`, `pending/info.json`, …) are denied in `.htaccess`; `lib/*.php` also self-guard against direct requests. Verified end-to-end against a local Apache+php-fpm honoring `.htaccess` (php's built-in server ignores `.htaccess`, so red-team against real Apache).

## YT-Streamer (Python)

`YT-Streamer/YT_streamer.py` — a Mac Mini daemon that records per-print video and uploads to YouTube. Three concurrent subsystems:

1. **Record orchestrator** (`record_orchestrator`): polls `stream-start.php` every `STREAM_POLL_INTERVAL` seconds for a `(gallery_id, name)`. On a hit, verifies the camera (ffmpeg enumeration), then starts an `AVFRecorder` — a **native AVCaptureSession** (PyObjC), **video-only — only a video input is added, audio is never captured** → `/tmp/recordings/<id>_<ts>.mov` via `AVCaptureMovieFileOutput` (1.5h cap via a deadline check), **plus a throttled `AVCaptureVideoDataOutput` writing `/tmp/recordings/latest_frame.jpg` at ~1fps** (the snapshot poller's no-contention source). The recorder selects the device format of `CAMERA_VIDEO_SIZE` (default `1920x1080`) whose max fps ≥ `CAMERA_FRAMERATE`, **preferring the `420v`/NV12 (MJPEG-backed) format** — the only one that sustains 1080p30 over USB 2.0. The H.264 average bitrate is pinned to `VIDEO_BITRATE` (default `2500k`) via `setOutputSettings:forConnection:` (AVFoundation's ~24 Mbps default would make a 90-min file ~16 GB). Then waits for the **snapshot-stop signal** (set by the snapshot poller when this print's snapshot is captured, gated by `MIN_RECORD_SECONDS` to ignore the job-start flag arm — see Per-print recording flow) or the 1.5h cap, whichever comes first. Stop = `stopRecording()` (finalizes the moov) then `stopRunning()`. The finished mov is handed to a *background* uploader thread (so the next print can start recording immediately while a slow upload is still in flight); the uploader calls `videos.insert` with `resumable=True, chunksize=8MiB`, POSTs the resulting 11-char id to `stream-video-id.php`, then unlinks the mov. Failed uploads stay on disk for manual recovery.
2. **Snapshot poller** (`poll_snapshot_queue`): polls `snapshot-next.php` every 5 s, grabs a webcam frame with ffmpeg avfoundation when idle (or copies the recorder's `latest_frame.jpg` sidecar while a recording holds the cam), SFTPs to `gallery/<id>/image.jpg`.
3. **Moderation poller** (`poll_mod_queue`): polls the mod queue, runs each submission through two Claude SDK calls (image + name).

YouTube auth uses OAuth 2.0 with the `youtube` scope (covers `videos.insert`). Client secrets and refresh token live in an encrypted DMG vault (`YT_streamer_vault.dmg`) protected by a Keychain-stored passphrase — see `_vault_*` and `get_youtube_service`. Auth is lazy: the vault is only mounted when the first recording arrives.

Configured via environment variables: `CAMERA_NAME` (default `Brio 100` — matched case-insensitively against the *start* of each video-device name, for both ffmpeg enumeration and the AVFoundation recorder's `localizedName` lookup), `CAMERA_FRAMERATE` (default 30), `CAMERA_VIDEO_SIZE` (default `1920x1080` — the recorder's target device-format size), `VIDEO_BITRATE` (default `2500k` — pinned H.264 average bitrate), `CAMERA_POLL_INTERVAL` (keeper re-scan cadence, default 15), `SNAPSHOT_SECRET`, `SNAPSHOT_REQUEST_URL`, `STREAM_START_URL`, `STREAM_END_URL`, `STREAM_VIDEO_ID_URL`, `STREAM_POLL_INTERVAL` (default 10), `SFTP_HOST`, `SFTP_USER`, `SFTP_REMOTE_DIR` (chrooted; empty string is valid), `SFTP_PORT` (default 21), `SFTP_PASS_FILE` (default `SFTP-pass.txt`, read from the vault), plus the moderation block (`MOD_QUEUE_URL`, `MOD_ACTION_URL`, `MOD_SECRET`, SMTP creds, etc.).

**Camera is a local USB webcam, NOT RTSP** (refactored away; the RTSP version is archived at `YT-Streamer/archive/`). **Recording uses a native AVFoundation `AVCaptureSession` (PyObjC), NOT ffmpeg.** ffmpeg is still used for device *enumeration* (keeper) and for *one-shot* snapshot stills when idle. Gotchas, do not re-litigate:
- **ffmpeg cannot record 1080p30 from this cam — that's why recording is native AVFoundation.** ffmpeg's avfoundation input only requests the cam's *uncompressed* device formats (`uyvy422`/`nv12`). Uncompressed 1080p over the Brio's **USB 2.0** link is bandwidth-capped to **~5fps** (only ≤640×480 sustains 30fps uncompressed); the result was a choppy recording that the 30fps container hid by padding with duplicate frames. The Brio exposes a second 1080p device format — `420v` (NV12, MJPEG-backed, OS-decoded) — that does a true 30fps, but ffmpeg's avfoundation demuxer can't select a specific `AVCaptureDeviceFormat` (and `-capture_raw_data` doesn't help). `AVCaptureSession` *can* pick it (that's what Photo Booth does), so `AVFRecorder._pick_format` chooses the wanted-size format with max fps ≥ target, **preferring `420v`**. Verify a recording's *real* fps with `ffmpeg -i f.mov -vf mpdecimate -f null -` (unique-frame count), NOT the container's `avg_frame_rate` (which is the padded value). The ~24fps you may see in dim room light is auto-exposure, not the bug — the bright LED scene sits at 30.
- **No audio, ever** — the user explicitly never wants audio broadcast. `AVFRecorder` adds only a video `AVCaptureDeviceInput`; there is no audio input. Don't add one.
- **The frame duration must be the format's exact rational.** A naive `CMTimeMake(1, 30)` is rejected (`setActiveVideoMinFrameDuration: Not supported`) because the device advertises `1/30.00003`. `AVFRecorder` pins both min/max active frame duration to the chosen range's own `minFrameDuration()`. Don't hand-construct `1/fps`.
- **The moov is finalized on `stopRecording()`, not crash-safe.** Unlike the old fragmented-mp4 ffmpeg path (which survived SIGKILL), an outright crash mid-record loses the `.mov`. The normal graceful-stop path (snapshot signal / cap) always finalizes cleanly; uploads happen per-print so at most one recording is at risk.
- **The main thread MUST run a CFRunLoop while a recording can be stopped from a worker thread.** `AVCaptureSession.stopRunning()` / `stopRecording()` tear the graph down by delivering `graphWillStop` to the **main thread** via `performSelector:onThread:<main> waitUntilDone:YES`. The record orchestrator calls `stop()` from a *worker* thread, so that perform is enqueued on the main run loop — if the main thread is parked (`time.sleep`), it's never serviced and `stop()` **deadlocks forever while the capture keeps writing** (this caused a single recording to run ~13h / 15GB; both the snapshot-stop and the 1.5h cap were powerless because they share the wedged orchestrator thread). `__main__` therefore services `CFRunLoopRunInMode` in 0.25s slices instead of sleeping, with explicit SIGINT/SIGTERM handlers (a CFRunLoop doesn't raise `KeyboardInterrupt`) that set a stop flag + `CFRunLoopStop`. Don't revert the main thread to a bare sleep loop. Stopping from the main thread itself (the shutdown handler) works even without the loop, because a same-thread `waitUntilDone:YES` perform runs inline. Backstop: `record_watchdog` force-`os._exit`s if any recording outlives `RECORD_MAX_SECONDS + RECORD_WATCHDOG_GRACE`, so a future stop-hang can't run away again.
- **Gate `stop()`'s `stopRecording()` on AVFRecorder's own `_recording_started` flag, not `movie_out.isRecording()`** — `isRecording()` was observed returning `False` on a live recording, which skipped the finalize and left the writer running.
- **Bitrate must be pinned.** `AVCaptureMovieFileOutput`'s default is ~24 Mbps at 1080p → a 90-min file ≈ 16 GB. `AVFRecorder` caps it to `VIDEO_BITRATE` (default `2500k`) via `setOutputSettings:forConnection:` on the video connection (which only exists *after* the output is added to the session).
- **The sidecar is a second `AVCaptureVideoDataOutput` on the same session** (BGRA frames → CoreImage JPEG, throttled to ~1fps in the delegate), running on a dispatch queue (needs `pyobjc-framework-libdispatch`). Adding it alongside the movie output does NOT reduce the recording fps. A USB webcam still allows only ONE *process* opener, so the snapshot path must NOT open the cam during a recording — it copies this sidecar instead (`_read_live_frame`, PIL-validated against torn reads). The orchestrator unlinks the sidecar on stop so stale frames aren't served.
- **Device discovery (enumeration) is still ffmpeg** = `ffmpeg -f avfoundation -list_devices true -i ""` (writes the list to **stderr** and exits non-zero — both expected). `_refresh_camera` parses it for the video device whose name starts with `CAMERA_NAME`; the keeper re-runs this every `CAMERA_POLL_INTERVAL`s whenever not recording. Enumerating doesn't open the cam, so the privacy LED stays off and it never contends with a recording. (The recorder separately re-selects the device by `localizedName` via `AVCaptureDeviceDiscoverySession` — the cached ffmpeg index isn't used for recording.)
- **PyObjC is a hard dep for recording** (`pyobjc-framework-{AVFoundation,CoreMedia,Quartz,libdispatch}`). The import is guarded: if it's missing the rest of the daemon still runs and only recording fails with a clear error. Install via `./venv/bin/python -m pip install -r requirements.txt` — **not** `./venv/bin/pip`, whose shebang points at the venv's original (pre-relocation) home and installs into the wrong site-packages.
- **Test with `./venv/bin/python`** (the venv has `requests`/`PIL`/`pyobjc`/etc.; system `python3` does not). Importing `YT_streamer` runs the required-env check, so the `.env` must be populated. To exercise the recorder directly: `from YT_streamer import AVFRecorder; r=AVFRecorder(Path('/tmp/t.mov'),'Brio',Path('/tmp/t.jpg')); r.start(); time.sleep(8); r.stop()`.

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

`Arduino Code/PARMain/PARMain.ino` — polls `POST /next.php` over HTTPS, decodes the base64 bitmap, drives the display, then polls again. Waits 10 seconds between polls when the queue is empty; waits 10 minutes after a successful display before polling again (paces polling / lets the board settle). It no longer signals the Mac Mini to stop recording — that's driven by the snapshot request (`sendSnapshotRequest()`), the single "print done" signal; `sendStreamEnd()` was removed. WiFi credentials live in `env.h` (not committed). **Naming:** on-disk dir is `PARMain`.

`Arduino Code/PARMain/FLOW.md` — concise per-step walkthrough of boot, main loop, color classification, and GRBL streaming for quick reference. **Drifts independently from the .ino** — when you change `flipDisc`, the scan offsets, servo timings, or the post-display delay, update FLOW.md in the same edit. Trust the .ino over FLOW.md when they disagree.

`Arduino Code/SerialBridge/` — USB↔Serial1 passthrough; flash to the Nano RP2040 to send raw G-code to the GRBL Mega from a PC serial monitor.

`Arduino Code/Servo*` — servo/motion bring-up & diagnosis sketches, all driving the production D9→ServoNano path: `ServoCenter` (hold servo at REST, never touches GRBL — safe park during reassembly), `ServoCycle` (sustained servo cycling, zero carriage motion), `ServoRepro` (full-board checkerboard job with robust+logged GRBL handling — closest stand-in for a real job), `ServoYFlex`/`ServoWiggleTest` (carriage-position stress; park-at-corner + cycle to localize an intermittent open). Diagnosed once this way: an intermittent open in the **main servo's moving-carriage cable at full +X (right) extension** — main dropped flips on the right side while the witness stayed fine.

`Color Sensor ML/ColorSensorStream/` — streams raw R/G/B/C frequency readings every ~15ms; useful for sensor debugging without running the full sketch.

### Hardware
- **Main MCU:** Arduino Nano RP2040 Connect (WiFiNINA for HTTPS, servo UART TX on D9, TCS3200 color sensor S0–S3 + OUT on D4–D8).
- **Servo drive:** the SG90 flip servo is offloaded to a dedicated 5V Arduino Nano (`ServoNano.ino`; enumerates as a CH340 `usbserial-*` port). RP2040 **D9 bit-bangs a 9600-baud software UART** of the µs value → ServoNano RX → `Servo` lib drives the SG90 (mbed UART/PwmOut on D9 crashed / didn't move it; bit-bang at 9600 tolerates WiFiNINA/Serial1 ISR jitter). ServoNano **echoes each received line + `[ok <us>]`/`[bad …]`** over its USB serial — a clean signal-integrity probe. A second **witness servo** is teed on the ServoNano output (short, fixed cable) as a diagnostic: if the main servo misbehaves while the witness stays fine, the fault is on the main's *own* branch (its long moving-carriage cable/connector), not the shared signal/output/5V rail.
- **Boards / ports / camera:** identify boards with `arduino-cli board list` — port names **change on replug** (RP2040 + Mega are `usbmodem*`, ServoNano is `usbserial-*`); FQBN `arduino:mbed_nano:nanorp2040connect`. Free a port before flashing/reading with `pkill -f serial-monitor` (Arduino IDE) and any `cat`/logger holding it. For ad-hoc ffmpeg captures of the rig camera, select the Brio **by name** (`-f avfoundation -i "Brio 100"`) — the numeric index is unstable (the iPhone Continuity camera shifts it); and add `-movflags +frag_keyframe+empty_moov` to any recording you might kill (a plain mp4 killed mid-write has no moov atom → corrupt).
- **Motion controller:** Arduino Mega running a slightly modified [grbl-Mega](https://github.com/gnea/grbl-Mega) — `cpu_map` patched for the CNC Shield V3 pinout. Source lives at `~/Documents/Arduino/libraries/grbl-Mega` (additional working dir). Talks to the main MCU over `Serial1` @ 115200 using GRBL's character-counting streaming protocol.
- **Coordinate system:** CNC homes to full negatives, so the work area lives in negative coords. `X_TRAVEL = 777.695`, `Y_TRAVEL = 402.0`; `initGrid()` origins to `(-X_TRAVEL + 25.0, -Y_TRAVEL + 0.0)`. Cell pitch is **20.045 mm in X, 23.40 mm in Y**; bitmap `y=0` is the top row, but physical Y increases upward, so Y is mirrored (`(GRID_H-1) - y`) when computing coords. **`Y_TRAVEL` MUST equal GRBL `$131` (currently `402`).** Homing drives to the −Y switch and GRBL pins that corner at `−$131` — the switch is the physical coordinate anchor, so the bottom row (grid Y `= −Y_TRAVEL`) stays physically put only when the two match; changing `$131` without `Y_TRAVEL` (or vice versa) shifts the whole pattern by the difference. The `$131` bump from `399.695` → `402` was deliberate: it raised `Y=0` 2.305 mm above the top of the grid, which fixed the top-row *scan* target — `scanGrid` adds `SCAN_OFFSET_Y` (+4.0) to center the sensor, and under the old value the top-row scan landed at `+2.105` (past `Y=0` → deterministic `ALARM:2` under `$20=1`; GRBL caps the work area at 0 on the positive side). Now it lands at `−0.2`, and `clampScanY()`/`SCAN_Y_MAX` in `PARMain.ino` remain as a safety net so future offset/pitch tweaks can't push a scan target past 0. Also: `drainResponses` logs the in-flight G-code on `ALARM` (`GRBL ALARM: <code> on '<cmd>'`) so a plog dump names the exact offending move.
- **Flip motion:** `flipDisc(x,y,catchByNextMove)` does a two-stage 180° rotation. Stage 1: servo to 90° (`SERVO_US_ENGAGE`) rotates the squisk 90°, then back to 0° (`SERVO_US_REST`). Stage 2: slide X +16.8 mm so the arm clears the disc column, drop the arm to ~46° (`SERVO_US_RELEASE`), and slide X −16.8 mm — the ~46° arm catches the half-rotated squisk during the return slide and pushes it through the final 90°. The X excursion is capped per-flip so it never commands past `X=0`. Servo settle: `SERVO_90_DEG_SETTLE_MS = 300` ms for the 90° moves, `SERVO_50_DEG_SETTLE_MS = 100` ms for the 50° moves; `writeServoUs(us, settle_ms)` takes settle as a second arg.
- **Second-catch pass (`#define FLIP_SECOND_CATCH`, default OFF):** an optional step 3 after Stage 2 — drop the arm a further ~10° (`SERVO_US_RELEASE2`, ~36°) and sweep +X again to push back any disc the first catch left over/under-rotated. Gated behind a commented-out `//#define FLIP_SECOND_CATCH` near `FLIP_OFFSET_X`; **uncomment to re-enable**. When OFF the arm is parked at REST after Stage 2 and `catchByNextMove` is ignored (the `(void)catchByNextMove;` in the `#else`). When ON, `catchByNextMove=true` leaves the arm down at `RELEASE2` so the caller's next +X move performs the sweep (no extra G-code); `false` emits an explicit +X stroke and re-parks at REST. The same `#define` + `#ifdef`/`#else` block is mirrored in `FlipCheckerboardTest.ino` — keep both in sync. `FlipCornersTest.ino` has no second-catch pass (its `flipDisc` is the simpler 2-arg form).
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
- **Carriage moves only with the servo at REST** (except inside `flipDisc`). A naive diagonal `moveTo` across the populated board (instead of `moveToYSafe`'s pure-X/pure-Y edge legs) drags the arm through discs and **snapped the flip arm once** — always route cross-field travel through the X-limit edge legs.
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
- **`gallery.php` rename TOCTOU race:** `snapshot-request.php` (and the redundant `complete.php`) does `rename(pending.json, info.json)`. There's a small window where `gallery.php` sees `pending.json` exists, then `file_get_contents` returns `false` because the file just got renamed. The endpoint silently `continue`s past such entries, so a single fetch can briefly miss an item mid-transition. The 5s polling masks this in practice.
- **Gallery deep-links via `#<id>` hash.** A URL hash of a bare gallery id (e.g. `/gallery#129`, or even a bare `/#129` — normalized to `/gallery#129` via `replaceState`) auto-switches to the Gallery tab and opens that entry's modal on load and on `hashchange` (`galleryIdFromHash()` + `openGalleryItemById()` in `script.js`). Opening any entry's modal also writes `/gallery#<id>` (so the URL is shareable/refresh-safe); closing it strips the hash. The completion email's "View it in the gallery" link uses this (`par_send_completion_email($to,$name,$galleryId)` in `lib/private_store.php` appends `#<id>`). Hash regex is digits-only; gallery ids are integer folder names.
- **Adding a new SPA tab** takes three edits: nav link `<a data-tab="X">` in `index.html`, `<section id="X" class="tab-content">` in `index.html`, and an `if (pathname === '/X') return 'X';` branch in `getActiveTabFromUrl()`. No server changes — both `router.php` and `.htaccess` fall back to `index.html` for unknown paths.
- **`.tab-content.active` is flex-row by default** — sibling children inside an active tab sit side-by-side. Add `#tabid.tab-content.active { flex-direction: column; }` if you need stacked content (latest and about already do this).
- **`loadLatestRecording()` clears `.latest-container.innerHTML`** on every load. Static content for the latest tab (e.g. taglines, links) must be a sibling of `.latest-container`, not a child, or it will be wiped.
- **In-page links to other tabs** should use `class="nav-link" data-tab="X"` so the existing `navLinks.forEach` click handler in `script.js` routes them via `pushState` instead of triggering a full page load. They must be present at script load time (the handler captures `navLinks` once).
- **Don't move gallery-entry creation to `enqueue.php`.** Tried this once to make the bitmap appear in the gallery as soon as it was uploaded; user pushed back — they want the bitmap visible only once the Arduino actually picks the item up. Keep entry creation in `next.php`.
