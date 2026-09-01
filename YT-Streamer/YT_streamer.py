#!/usr/bin/env python3
"""
Per-print Recorder + Snapshot/Moderation Pollers.

Records a locally-attached USB webcam (a Logitech "Brio 100", matched by name
prefix) to a local mov while a print is active via a NATIVE AVFoundation
capture session (PyObjC), then uploads to YouTube via the Data API and attaches
the resulting video id to the gallery entry.

Recording uses AVCaptureSession — NOT ffmpeg — because ffmpeg's avfoundation
input can only request the camera's UNCOMPRESSED device formats, and uncompressed
1080p over USB 2.0 is bandwidth-capped to ~5fps. AVCaptureSession can select the
MJPEG-backed 1080p30 device format (the same one Photo Booth uses), so it gets a
true 1080p30. ffmpeg is still used for device enumeration and for one-shot
snapshot stills when no recording is in flight.

A recording starts when stream-start.php hands us a (gallery_id, name) and
stops when either stream-end.php fires (Arduino's signal after its 10-min
post-display linger) or the RECORD_MAX_SECONDS hard cap elapses.

Also runs the snapshot poller (Site5 -> SFTP photo) and the moderation poller
on background threads; those are independent of the recorder.
"""

import subprocess
import time
import logging
import sys
import threading
import json
import re
import shutil
import tempfile
import requests
import ftplib
import ssl
import os
import signal
import smtplib
import base64
import io
import concurrent.futures
import anyio
from PIL import Image, ImageDraw, ImageFont
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText
from email.mime.image import MIMEImage
from datetime import datetime
from pathlib import Path
from dotenv import load_dotenv

from claude_agent_sdk import (
    query as claude_query,
    ClaudeSDKClient,
    ClaudeAgentOptions,
    AssistantMessage,
    ResultMessage,
    TextBlock,
)

from google.oauth2.credentials import Credentials
from google_auth_oauthlib.flow import InstalledAppFlow
from google.auth.transport.requests import Request
import httplib2
from google.auth.exceptions import RefreshError, TransportError
from googleapiclient.discovery import build
from googleapiclient.errors import HttpError
from googleapiclient.http import MediaFileUpload

load_dotenv(Path(__file__).parent / ".env")

# ── CONFIG ─────────────────────────────────────────────────────────────────────
# Locally-attached USB webcam. CAMERA_NAME is matched case-insensitively against
# the START of each video-device name, so "brio 100" matches "Brio 100", "Brio
# 100 (1234)", etc. — for BOTH ffmpeg enumeration (keeper/snapshot) and the
# AVFoundation recorder's localizedName lookup. Recording is ALWAYS video-only:
# the AVCaptureSession adds only a video input, no audio.
CAMERA_NAME            = os.getenv("CAMERA_NAME", "Brio 100")
CAMERA_FRAMERATE       = os.getenv("CAMERA_FRAMERATE", "30")
# Recording resolution. The AVFoundation recorder selects the device format of
# exactly this size whose max frame rate is >= CAMERA_FRAMERATE, preferring the
# '420v' (NV12, MJPEG-backed) format — the only one that sustains 30fps at 1080p
# over USB 2.0. The Brio 100 tops out at 1920x1080@30.
CAMERA_VIDEO_SIZE      = os.getenv("CAMERA_VIDEO_SIZE", "1920x1080")
try:
    CAMERA_REC_W, CAMERA_REC_H = (int(x) for x in CAMERA_VIDEO_SIZE.lower().split("x"))
except Exception:
    print(f"WARNING: CAMERA_VIDEO_SIZE={CAMERA_VIDEO_SIZE!r} invalid; using 1920x1080")
    CAMERA_REC_W, CAMERA_REC_H = 1920, 1080
# Keeper re-enumerates the device list this often (when not recording).
CAMERA_POLL_INTERVAL   = int(os.getenv("CAMERA_POLL_INTERVAL", "15"))
SNAPSHOT_SECRET        = os.getenv("SNAPSHOT_SECRET")

SNAPSHOT_REQUEST_URL   = os.getenv("SNAPSHOT_REQUEST_URL")
SNAPSHOT_POLL_INTERVAL = 5
SNAPSHOT_LOCAL_DIR     = Path("/tmp/snapshots")

STREAM_START_URL       = os.getenv("STREAM_START_URL")
STREAM_END_URL         = os.getenv("STREAM_END_URL")
STREAM_VIDEO_ID_URL    = os.getenv("STREAM_VIDEO_ID_URL")
STREAM_POLL_INTERVAL   = int(os.getenv("STREAM_POLL_INTERVAL", "10"))

# Public gallery listing, used as an INDEPENDENT recording-stop trigger: while a
# recording is in flight the orchestrator polls this and stops the moment its
# gallery id reads "not pending" (display done). This does NOT go through the
# snapshot flag / snapshot poller, so it also covers the case where the Arduino's
# snapshot-request.php (the flag arm) is lost but the entry still finalizes via
# complete.php — the exact failure that let one recording run to the cap. No new
# .env key needed: derive it from an existing endpoint URL (same origin), with an
# optional GALLERY_URL override.
def _sibling_url(base: str | None, leaf: str) -> str | None:
    if not base or "/" not in base:
        return None
    return base.rsplit("/", 1)[0] + "/" + leaf
GALLERY_URL            = os.getenv("GALLERY_URL") or _sibling_url(
    SNAPSHOT_REQUEST_URL or STREAM_START_URL, "gallery.php")
# How often (seconds) to poll the gallery for completion during a recording.
# Slower than the main loop tick to stay gentle on the shared host's rate limits;
# a few extra seconds of recording tail past display-done is immaterial.
GALLERY_COMPLETE_POLL_INTERVAL = int(os.getenv("GALLERY_COMPLETE_POLL_INTERVAL", "20"))

# ── UPLOAD RETRY ───────────────────────────────────────────────────────────────
# A videos.insert is a multi-GB transfer over a residential link, so a transient
# socket failure mid-upload is normal, not exceptional. Before these knobs
# existed a single blip abandoned the recording permanently: the catch-all in
# upload_recording returned None and nothing ever retried, despite the upload
# being resumable precisely so it could continue. On 2026-08-22 that stranded
# five prints (#68-#71, #74 — 4.5 GB) in one afternoon; three of them died
# instantly on connection setup having transferred nothing, so a retry seconds
# later would have succeeded.
#
# Two layers, because they cover different failures:
#   - UPLOAD_CHUNK_RETRIES is handed to next_chunk(), whose own backoff retries
#     a single failed chunk against the SAME resumable session. This is the
#     cheap, correct path — the server keeps the bytes already received.
#   - UPLOAD_MAX_ATTEMPTS restarts the whole upload with a fresh session when a
#     chunk exhausts its retries. Costlier, so it is the outer fallback.
UPLOAD_MAX_ATTEMPTS    = int(os.getenv("UPLOAD_MAX_ATTEMPTS", "5"))
UPLOAD_CHUNK_RETRIES   = int(os.getenv("UPLOAD_CHUNK_RETRIES", "5"))
# Backoff between whole-upload attempts: BASE * 2**(attempt-1), capped. Generous
# because the failures this recovers from (link down, DHCP renew, ISP blip) tend
# to last tens of seconds, not milliseconds.
UPLOAD_RETRY_BASE_DELAY = float(os.getenv("UPLOAD_RETRY_BASE_DELAY", "10"))
UPLOAD_RETRY_MAX_DELAY  = float(os.getenv("UPLOAD_RETRY_MAX_DELAY", "300"))

# Local working dir for in-flight .mov recordings. Uploads delete on success;
# failed uploads are left here for manual recovery.
RECORDING_DIR          = Path("/tmp/recordings")
# While a recording is in flight, the recording ffmpeg also writes a ~1fps JPEG
# here (a second output of the same capture). The snapshot poller copies THIS
# instead of opening the camera, because a USB webcam allows only one opener at
# a time and the recording owns it.
LATEST_FRAME_PATH      = RECORDING_DIR / "latest_frame.jpg"
# Hard cap on a single recording (safety net if the stop signal is lost). This is
# a BACKSTOP, not a target — it must sit comfortably above the longest real print,
# because a print that outruns it gets its recording truncated mid-artwork. Raised
# 60 -> 90 min on 2026-08-22 after prints #71-#74 ran 60-64 min and were all cut
# off before finishing; at the pinned VIDEO_BITRATE a 90-min file is ~1.7 GB.
RECORD_MAX_SECONDS     = int(os.getenv("RECORD_MAX_MINUTES", "90")) * 60
# Out-of-band watchdog ceiling. The cap above is enforced inside the orchestrator
# loop, which shares its thread with the (native, blocking) stop() call — if stop
# wedges, the cap can never fire. This grace is how far past the cap a recording
# may run before the watchdog force-exits the process as a last resort, so a
# single hung stop can never produce a multi-hour runaway recording again.
RECORD_WATCHDOG_GRACE  = 5 * 60
# A recording must run at least this long before the snapshot signal is allowed
# to stop it. The snapshot flag is armed TWICE per print — once by next.php at
# job start (content = gallery id) and once by the Arduino's snapshot-request at
# display-done (empty → id=null). The job-start arm fires within a few seconds of
# the recording starting (or before it), so requiring the snapshot signal to land
# at least this far into the recording filters it out; the real display takes
# minutes, so the post-display snapshot always clears this floor.
MIN_RECORD_SECONDS     = 60
# After start(), a healthy AVCaptureSession must actually deliver frames (movie
# file growing on disk and/or sidecar buffers arriving) within this window. A
# session that starts but streams nothing — the classic "camera not authorized in
# a detached/launchd context" failure, where startRunning() succeeds but macOS
# routes no frames and posts no synchronous error — is otherwise invisible: the
# didFinishRecording delegate never fires, is_running() stays True, and the
# orchestrator writes an empty file for the full cap. This gate catches that
# in seconds so the print can be logged/aborted instead of silently lost.
FRAME_LIVENESS_TIMEOUT   = 12.0
# Movie-file byte floor that proves real frames landed (used only when the sidecar
# VideoDataOutput was rejected, so there's no per-buffer counter to trust). A
# black/failed session leaves the file missing or ~0 bytes; a real 2500k H.264
# stream blows past this within a second or two.
FRAME_LIVENESS_MIN_BYTES = 64 * 1024
# Human-readable AVCaptureDevice.authorizationStatusForMediaType_ values, logged
# at recorder start so the daemon's ACTUAL camera-TCC state (as its own process
# sees it) is visible in the log — the difference between an authorized
# Terminal-launched run and a NotDetermined/Denied launchd-detached one.
_CAMERA_AUTH_NAMES = {0: "NotDetermined", 1: "Restricted", 2: "Denied", 3: "Authorized"}

# ── TIMELAPSE INTRO ───────────────────────────────────────────────────────────
#
# Every upload is prefixed with a fixed-SPEED timelapse of the whole print, so a
# viewer sees the finished piece assemble before the real-time footage starts.
# The label in the bottom-right names the speed for each half: "Timelapse
# (<mult>x speed)" over the intro, "Real-time (1x speed)" after.
#
# The RATE is fixed, not the duration: the intro runs `duration / TIMELAPSE_SPEED`
# seconds, so a longer print gets a proportionally longer intro and every video
# reads at the same pace. (It was a fixed 20s at a computed multiplier first;
# that made the multiplier a different number on every upload.)
#
# The labels are rendered to PNGs with Pillow and composited with ffmpeg's
# `overlay`, NOT `drawtext`: the Homebrew ffmpeg on this machine is built
# without libfreetype, so drawtext does not exist in it (`ffmpeg -filters |
# grep drawtext` is empty). Don't "simplify" this back to drawtext without
# checking that first — it fails at run time with "No such filter".
TIMELAPSE_ENABLED       = os.getenv("TIMELAPSE_ENABLED", "1") not in ("0", "false", "no")
TIMELAPSE_SPEED         = float(os.getenv("TIMELAPSE_SPEED", "125"))
# An intro shorter than this is a blink, not a timelapse, so such recordings are
# uploaded unmodified. At 125x this floors the source at ~4 minutes.
TIMELAPSE_MIN_INTRO_S   = float(os.getenv("TIMELAPSE_MIN_INTRO_SECONDS", "2"))
# Label geometry, both expressed as a fraction of frame height so the overlay
# scales with CAMERA_VIDEO_SIZE. ~1/24 of 1080p is a 45px cap height — "medium".
TIMELAPSE_FONT_DIV      = float(os.getenv("TIMELAPSE_FONT_DIV", "24"))
TIMELAPSE_MARGIN_DIV    = float(os.getenv("TIMELAPSE_MARGIN_DIV", "36"))
TIMELAPSE_TEXT_RGB      = (0, 0, 0)          # black, per spec
TIMELAPSE_FONT_CANDIDATES = (
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/HelveticaNeue.ttc",
    "/Library/Fonts/Arial.ttf",
)
# The composed file is a full re-encode of (20s + the whole print), so it uses
# the hardware encoder by default; libx264 is the fallback if VideoToolbox is
# unavailable. Bitrate sits above VIDEO_BITRATE because this is a second
# generation of H.264 over an already-compressed source.
# Decode only keyframes for the timelapse pass when the file's keyframes are
# dense enough to fill the intro (measured per recording, never assumed). This is
# where nearly all the compose time is saved; set to 0 to force full decode.
TIMELAPSE_KEYFRAME_DECODE = os.getenv("TIMELAPSE_KEYFRAME_DECODE", "1") not in ("0", "false", "no")
TIMELAPSE_ENCODER       = os.getenv("TIMELAPSE_ENCODER", "h264_videotoolbox")
TIMELAPSE_BITRATE       = os.getenv("TIMELAPSE_BITRATE", "4000k")
# Hard ceiling on the compose subprocess. A 90-min 1080p re-encode runs in
# minutes on VideoToolbox; anything near this is wedged, and a wedged compose
# must not hold the uploader thread (and therefore the recording) forever.
TIMELAPSE_TIMEOUT       = float(os.getenv("TIMELAPSE_TIMEOUT", "3600"))

# FTPS (port 21, explicit TLS). The Site5 addon FTP account is FTP-only —
# SSH/SFTP on :22 is reserved for the main cPanel user, so paramiko can't
# auth there. Host must be the Site5 origin (shared187...) because the
# customer-facing hostnames are Cloudflare-proxied and CF doesn't tunnel :21.
SFTP_HOST       = os.getenv("SFTP_HOST")
SFTP_PORT       = int(os.getenv("SFTP_PORT", "21"))
SFTP_USER       = os.getenv("SFTP_USER")
# Password lives in the encrypted DMG vault, not .env. Filename inside the
# vault is configurable so we don't bake the convention into code.
SFTP_PASS_FILE  = os.getenv("SFTP_PASS_FILE", "SFTP-pass.txt")
# Empty string is valid: FTP accounts are chrooted, so paths are relative to
# the account's home (the gallery dir itself).
SFTP_REMOTE_DIR = os.getenv("SFTP_REMOTE_DIR", "")

# ── MODERATION ─────────────────────────────────────────────────────────────────
MOD_QUEUE_URL      = os.getenv("MOD_QUEUE_URL")
MOD_ACTION_URL     = os.getenv("MOD_ACTION_URL")
MOD_SECRET         = os.getenv("MOD_SECRET") or os.getenv("SNAPSHOT_SECRET")
MOD_POLL_INTERVAL  = int(os.getenv("MOD_POLL_INTERVAL", "30"))
MOD_AUTO_THRESHOLD = float(os.getenv("MOD_AUTO_THRESHOLD", "0.7"))
MOD_CHECK_TIMEOUT  = int(os.getenv("MOD_CHECK_TIMEOUT", "30"))   # per-attempt seconds
MOD_CHECK_RETRIES  = int(os.getenv("MOD_CHECK_RETRIES", "5"))    # extra attempts on timeout
_MOD_REASONING_RAW = os.getenv("MOD_REASONING_EFFORT", "medium").lower()
_VALID_EFFORTS = ("low", "medium", "high", "xhigh", "max")
if _MOD_REASONING_RAW not in _VALID_EFFORTS:
    print(f"WARNING: MOD_REASONING_EFFORT={_MOD_REASONING_RAW!r} invalid; "
          f"must be one of {_VALID_EFFORTS}. Falling back to 'medium'.")
    _MOD_REASONING_RAW = "medium"
# Narrow to the SDK's Literal type so static checkers are happy.
from typing import Literal, cast
MOD_REASONING: Literal["low", "medium", "high", "xhigh", "max"] = cast(
    Literal["low", "medium", "high", "xhigh", "max"], _MOD_REASONING_RAW
)
MOD_IMAGE_MODEL    = os.getenv("MOD_IMAGE_MODEL", "claude-sonnet-4-6")
MOD_NAME_MODEL     = os.getenv("MOD_NAME_MODEL",  "claude-haiku-4-5-20251001")
MOD_ARTIST_MODEL   = os.getenv("MOD_ARTIST_MODEL", "claude-haiku-4-5-20251001")
NOTIFY_EMAIL       = os.getenv("NOTIFY_EMAIL")
SMTP_HOST          = os.getenv("SMTP_HOST")
SMTP_USER          = os.getenv("SMTP_USER")
SMTP_PASS          = os.getenv("SMTP_PASS")
SMTP_PORT          = int(os.getenv("SMTP_PORT", "465"))

# YouTube Data API OAuth (write scope for broadcast management)
YT_SCOPES              = ["https://www.googleapis.com/auth/youtube"]
YT_VAULT_DMG           = Path(__file__).parent / os.getenv("YT_VAULT_DMG", "YT_streamer_vault.dmg")
YT_VAULT_KEYCHAIN_KEY  = os.getenv("YT_VAULT_KEYCHAIN_KEY", "")
YT_VAULT_SECRET_FILE   = os.getenv("YT_VAULT_SECRET_FILE", "")
YT_VAULT_TOKEN_FILE    = "yt_token.json"
# ── PLAYLIST (a playlist on a DIFFERENT channel than the uploads) ──────────────
# videos.insert always lands on the channel the UPLOAD token was consented for,
# and playlistItems.insert can only write a playlist owned by the channel ITS
# token was consented for. One OAuth token is bound to exactly one channel, so
# uploading to channel A while filing into a playlist on channel B needs TWO
# separately-consented tokens. Both live in the same encrypted vault, under
# different filenames. Set them up with:  ./run_auth_setup.sh
#
# If YT_PLAYLIST_ID is empty the whole playlist step is skipped (uploads are
# unaffected). If the playlist token file is absent but YT_PLAYLIST_ID is set,
# the upload client is used as a fallback — correct only when the playlist
# happens to live on the upload channel.
YT_PLAYLIST_ID               = os.getenv("YT_PLAYLIST_ID", "").strip()
YT_VAULT_PLAYLIST_TOKEN_FILE = os.getenv("YT_VAULT_PLAYLIST_TOKEN_FILE",
                                         "yt_token_playlist.json")
# A playlist insert is a single cheap call (50 quota units) with no resumable
# session to protect, so a couple of quick retries is all it warrants.
VIDEO_ID_MAX_ATTEMPTS        = int(os.getenv("VIDEO_ID_MAX_ATTEMPTS", "5"))
VIDEO_ID_RETRY_BASE_DELAY    = float(os.getenv("VIDEO_ID_RETRY_BASE_DELAY", "5"))
PLAYLIST_MAX_ATTEMPTS        = int(os.getenv("PLAYLIST_MAX_ATTEMPTS", "3"))
PLAYLIST_RETRY_BASE_DELAY    = float(os.getenv("PLAYLIST_RETRY_BASE_DELAY", "5"))
# Background OAuth-token keepalive cadence (hours). The refresher runs even with
# no start signals so a dead/expired refresh token surfaces in the log early
# instead of wedging the next recording. Refresh-only — never interactive.
YT_TOKEN_REFRESH_INTERVAL = float(os.getenv("YT_TOKEN_REFRESH_HOURS", "6")) * 3600.0
# Public site URL used in the video description. Must carry the scheme —
# YouTube only auto-links descriptions when the URL starts with http(s)://.
SITE_BASE_URL = os.getenv("SITE_BASE_URL", "https://par.zimmzimm.com").rstrip("/")

_required = {
    "SNAPSHOT_SECRET": SNAPSHOT_SECRET,
    "SNAPSHOT_REQUEST_URL": SNAPSHOT_REQUEST_URL,
    "SFTP_HOST": SFTP_HOST,
    "SFTP_USER": SFTP_USER,
    # SFTP_REMOTE_DIR is intentionally not required — empty == chrooted home.
    "YT_VAULT_KEYCHAIN_KEY": YT_VAULT_KEYCHAIN_KEY,
    "YT_VAULT_SECRET_FILE": YT_VAULT_SECRET_FILE,
}
_missing = [k for k, v in _required.items() if not v]
if _missing:
    print("ERROR: Missing required environment variables:", ", ".join(_missing))
    sys.exit(1)

# Target H.264 average bitrate for the recording. AVCaptureMovieFileOutput's
# default is ~24 Mbps at 1080p (a 60-min cap would be ~11 GB) — far too large
# for /tmp and the YouTube upload, so we pin a sane average via the movie
# output's compression settings. 2.5 Mbps is plenty for the low-detail LED
# matrix subject (60-min cap ≈ 1.1 GB).
VIDEO_BITRATE        = os.getenv("VIDEO_BITRATE", "2500k")
VIDEO_BITRATE_BPS    = int(VIDEO_BITRATE.rstrip("kK")) * 1000
CAMERA_RETRY_DELAY   = 30   # seconds between camera-availability re-checks
# Total window to keep retrying the camera after a start signal before giving
# up on a print. A cam that comes up any time inside this window gets recorded.
CAMERA_WAIT_SECONDS  = 10 * 60

# Cached avfoundation video-device index from the last successful enumeration
# (e.g. "1"). Audio is never captured. None = not currently present. The keeper
# refreshes this every CAMERA_POLL_INTERVAL seconds.
_camera_lock = threading.Lock()
_camera_spec: str | None = None
# Set while a recording is in flight — pauses the camera keeper so it doesn't
# re-enumerate devices while the AVCaptureSession holds the cam.
_recording_active = threading.Event()
# Tracks the currently in-flight recording (the AVFRecorder + its mov path) so a
# Ctrl+C handler can stop it cleanly and delete the partial file. Guarded by
# _inflight_lock because the orchestrator thread writes it and the signal handler
# (main thread) reads it.
_inflight_lock = threading.Lock()
_inflight_rec: "AVFRecorder | None" = None
_inflight_path: Path | None = None
# Gallery id of the in-flight recording (None when idle). Read by the snapshot
# poller so a snapshot popped with id=null (the Arduino's post-display touch) can
# still be tied to the recording it belongs to. Guarded by _inflight_lock.
_inflight_id: int | None = None
# time.monotonic() stamp when the in-flight recording started (None when idle).
# Read by the watchdog to enforce a hard wall-clock ceiling independent of the
# orchestrator loop, so a wedged stop() can't record forever. Guarded by
# _inflight_lock.
_inflight_started: float | None = None
# Snapshot-stop signal: the snapshot poller sets these after it grabs the
# snapshot for an in-flight print; record_orchestrator polls them to know the
# print is done and the recording should stop. _snapshot_stop_ts is a
# time.monotonic() stamp so the orchestrator can require the signal to have
# landed well after the recording began (see MIN_RECORD_SECONDS).
_snapshot_stop_lock = threading.Lock()
_snapshot_stop_id: int | None = None
_snapshot_stop_ts: float = 0.0
# ───────────────────────────────────────────────────────────────────────────────

# Anchor the log file to this script's directory, not the launch cwd — the
# daemon is often started from the project root (or via launchd), and a bare
# relative "stream.log" then lands wherever cwd happens to be, leaving the
# expected YT-Streamer/stream.log empty.
_LOG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stream.log")
logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s [%(levelname)s] %(message)s",
    handlers=[
        logging.StreamHandler(sys.stdout),
        logging.FileHandler(_LOG_PATH),
    ]
)
log = logging.getLogger(__name__)


# ── NATIVE AVFOUNDATION RECORDER ─────────────────────────────────────────────
# Recording goes through AVCaptureSession (PyObjC), not ffmpeg, so we can select
# the camera's MJPEG-backed 1080p30 device format (see module docstring). The
# import is guarded so the rest of the daemon (mod/snapshot pollers) still loads
# if PyObjC is somehow missing — only recording then fails, with a clear error.
try:
    import objc
    import AVFoundation as _AVF
    import CoreMedia as _CM
    import Quartz as _Quartz
    import libdispatch as _libdispatch
    from Foundation import NSObject, NSURL, NSNotificationCenter
    _AVF_IMPORT_ERROR: Exception | None = None
except Exception as _avf_e:                                    # pragma: no cover
    _AVF_IMPORT_ERROR = _avf_e
    NSObject = object                                          # type: ignore


def _fourcc(n: int) -> str:
    return bytes([(n >> 24) & 0xFF, (n >> 16) & 0xFF,
                  (n >> 8) & 0xFF, n & 0xFF]).decode("ascii", "replace")


if _AVF_IMPORT_ERROR is None:
    class _RecordDelegate(NSObject):
        """AVCaptureMovieFileOutput finish callback — signals the recorder that
        the moov atom has been flushed and the file is finalized."""
        def initWithRecorder_(self, rec):
            self = objc.super(_RecordDelegate, self).init()
            if self is None:
                return None
            self._rec = rec
            return self

        def captureOutput_didFinishRecordingToOutputFileAtURL_fromConnections_error_(
                self, out, url, conns, error):
            self._rec._on_finish(error)

        # AVCaptureSession reports post-startRunning() failures ONLY through these
        # notifications — never synchronously from startRunning()/startRecording.
        # Without observing them, a session that dies (camera unauthorized,
        # yanked, or grabbed by another process) keeps is_running() True forever.
        def sessionRuntimeError_(self, note):
            err = note.userInfo().get(_AVF.AVCaptureSessionErrorKey) \
                if note.userInfo() is not None else None
            self._rec._on_session_failure("runtime-error", err)

        def sessionWasInterrupted_(self, note):
            reason = note.userInfo().get(_AVF.AVCaptureSessionInterruptionReasonKey) \
                if note.userInfo() is not None else None
            self._rec._on_session_failure(f"interrupted(reason={reason})", None)

    class _SidecarDelegate(NSObject):
        """VideoDataOutput callback — writes LATEST_FRAME_PATH ~1fps so the
        snapshot poller has a no-contention source while a recording owns the cam.
        Throttled in-callback; most invocations just compare a timestamp and
        return. Frames arrive BGRA; CoreImage renders the JPEG."""
        def initWithPath_(self, path):
            self = objc.super(_SidecarDelegate, self).init()
            if self is None:
                return None
            self._path = str(path)
            self._tmp = self._path + ".tmp"
            self._last = 0.0
            self._frames = 0  # every buffer, pre-throttle — the frame-liveness signal
            self._ctx = _Quartz.CIContext.context()
            self._cs = _Quartz.CGColorSpaceCreateDeviceRGB()
            return self

        def captureOutput_didOutputSampleBuffer_fromConnection_(self, out, sbuf, conn):
            self._frames += 1
            now = time.monotonic()
            if now - self._last < 1.0:
                return
            self._last = now
            try:
                img = _CM.CMSampleBufferGetImageBuffer(sbuf)
                if img is None:
                    return
                ci = _Quartz.CIImage.imageWithCVImageBuffer_(img)
                data = self._ctx.JPEGRepresentationOfImage_colorSpace_options_(ci, self._cs, {})
                if data is not None and data.writeToFile_atomically_(self._tmp, True):
                    os.replace(self._tmp, self._path)
            except Exception as e:                              # pragma: no cover
                log.debug(f"[sidecar] frame write failed: {e!r}")


class AVFRecorder:
    """Native AVFoundation per-print recorder. Selects the camera's MJPEG-backed
    1080p30 device format (the one ffmpeg's avfoundation input can't reach) and
    records H.264 to `out_path` via AVCaptureMovieFileOutput, while a throttled
    VideoDataOutput writes a ~1fps JPEG sidecar (`sidecar_path`) for the snapshot
    poller. Audio is never captured — only a video input is added.

    Unlike the old fragmented-mp4 ffmpeg path, the moov atom is finalized on
    stop(); a hard crash mid-record loses the file, but the normal graceful-stop
    path (snapshot signal or cap) always finalizes cleanly."""

    def __init__(self, out_path: Path, name_prefix: str, sidecar_path: Path,
                 want_w: int = CAMERA_REC_W, want_h: int = CAMERA_REC_H,
                 fps: int = int(CAMERA_FRAMERATE)):
        self.out_path = Path(out_path)
        self.sidecar_path = Path(sidecar_path)
        self.name_prefix = name_prefix.strip().lower()
        self.want_w, self.want_h, self.fps = want_w, want_h, fps
        self._session = None
        self._movie_out = None
        self._delegate = None
        self._data_delegate = None
        self._queue = None
        self._error = None
        self._finished = threading.Event()
        self._recording_started = False
        self._observing = False  # True once session notifications are registered
        self.device_name: str | None = None
        self.chosen_subtype: str | None = None
        self.auth_status: int | None = None  # camera TCC status seen at start()

    # — public API —————————————————————————————————————————————————————————————
    def start(self) -> None:
        """Configure the session and begin recording. Raises on fatal setup
        error (no PyObjC, no matching device, no suitable format, etc.)."""
        if _AVF_IMPORT_ERROR is not None:
            raise RuntimeError(f"PyObjC AVFoundation unavailable: {_AVF_IMPORT_ERROR!r}")

        # Camera TCC status as THIS process sees it. Reads the grant; does not open
        # the device. Anything other than Authorized(3) means the capture graph
        # will start but deliver no frames (and can't prompt from a daemon), which
        # is the silent-empty-recording failure — surface it loudly up front.
        self.auth_status = _AVF.AVCaptureDevice.authorizationStatusForMediaType_(
            _AVF.AVMediaTypeVideo)
        _auth_name = _CAMERA_AUTH_NAMES.get(self.auth_status, "?")
        if self.auth_status == 3:
            log.info(f"[record] camera authorization = {self.auth_status} ({_auth_name})")
        else:
            log.error(f"[record] camera authorization = {self.auth_status} ({_auth_name}) "
                      "— capture will start but stream NO frames; grant Camera "
                      "access to the process that launches this daemon")

        dt = _AVF.AVCaptureDeviceDiscoverySession.discoverySessionWithDeviceTypes_mediaType_position_(
            [_AVF.AVCaptureDeviceTypeExternal,
             _AVF.AVCaptureDeviceTypeBuiltInWideAngleCamera,
             _AVF.AVCaptureDeviceTypeContinuityCamera],
            _AVF.AVMediaTypeVideo, _AVF.AVCaptureDevicePositionUnspecified)
        devs = list(dt.devices())
        dev = next((d for d in devs
                    if d.localizedName().lower().startswith(self.name_prefix)), None)
        if dev is None:
            raise RuntimeError(
                f"no AVFoundation camera matching {self.name_prefix!r} "
                f"(present: {[d.localizedName() for d in devs]})")
        self.device_name = dev.localizedName()

        fmt = self._pick_format(dev)
        if fmt is None:
            raise RuntimeError(
                f"{self.device_name!r} has no {self.want_w}x{self.want_h}"
                f"@>={self.fps}fps format")

        ok = dev.lockForConfiguration_(None)
        if not (ok[0] if isinstance(ok, tuple) else ok):
            raise RuntimeError("lockForConfiguration failed")
        try:
            dev.setActiveFormat_(fmt)
            # Pin CFR at the format's fastest supported frame duration. Use the
            # range's exact rational (the device rejects a naive 1/fps CMTime).
            r = sorted(fmt.videoSupportedFrameRateRanges(),
                       key=lambda x: -x.maxFrameRate())[0]
            dev.setActiveVideoMinFrameDuration_(r.minFrameDuration())
            dev.setActiveVideoMaxFrameDuration_(r.minFrameDuration())
        finally:
            dev.unlockForConfiguration()

        session = _AVF.AVCaptureSession.alloc().init()
        session.beginConfiguration()
        inp, err = _AVF.AVCaptureDeviceInput.deviceInputWithDevice_error_(dev, None)
        if inp is None or not session.canAddInput_(inp):
            raise RuntimeError(f"cannot add camera input: {err}")
        session.addInput_(inp)

        movie = _AVF.AVCaptureMovieFileOutput.alloc().init()
        if not session.canAddOutput_(movie):
            raise RuntimeError("cannot add movie output")
        session.addOutput_(movie)

        # Sidecar: low-overhead VideoDataOutput, BGRA, throttled to ~1fps. Adding
        # it alongside the movie output does NOT reduce the recording's fps.
        # Best-effort: if the OS won't allow the second output, record anyway.
        try:
            data = _AVF.AVCaptureVideoDataOutput.alloc().init()
            data.setAlwaysDiscardsLateVideoFrames_(True)
            # kCVPixelBufferPixelFormatTypeKey == "PixelFormatType"; 'BGRA' == 0x42475241
            data.setVideoSettings_({"PixelFormatType": 0x42475241})
            self._queue = _libdispatch.dispatch_queue_create(b"par.avf.sidecar", None)
            self._data_delegate = _SidecarDelegate.alloc().initWithPath_(str(self.sidecar_path))
            data.setSampleBufferDelegate_queue_(self._data_delegate, self._queue)
            if session.canAddOutput_(data):
                session.addOutput_(data)
            else:
                self._data_delegate = None
                log.warning("[record] sidecar VideoDataOutput rejected; "
                            "recording without live frame")
        except Exception as e:
            self._data_delegate = None
            log.warning(f"[record] sidecar setup failed ({e!r}); "
                        "recording without live frame")

        session.commitConfiguration()
        self._session = session
        self._movie_out = movie

        # Cap the H.264 average bitrate — AVFoundation's default (~24 Mbps at
        # 1080p) would make a 60-min recording ~11 GB. The video connection
        # exists only after the output is added to the session.
        try:
            conn = movie.connectionWithMediaType_(_AVF.AVMediaTypeVideo)
            if conn is not None:
                movie.setOutputSettings_forConnection_({
                    _AVF.AVVideoCodecKey: _AVF.AVVideoCodecTypeH264,
                    _AVF.AVVideoWidthKey: self.want_w,
                    _AVF.AVVideoHeightKey: self.want_h,
                    _AVF.AVVideoCompressionPropertiesKey: {
                        _AVF.AVVideoAverageBitRateKey: VIDEO_BITRATE_BPS,
                        _AVF.AVVideoMaxKeyFrameIntervalKey: self.fps,
                    },
                }, conn)
        except Exception as e:
            log.warning(f"[record] could not set bitrate ({e!r}); "
                        "using AVFoundation default")

        # A stale sidecar from a prior recording must not be served before the
        # first fresh frame lands.
        self.sidecar_path.unlink(missing_ok=True)

        # Register the delegate + session-failure observers BEFORE startRunning so a
        # runtime error raised during startup is caught (not just mid-recording).
        self._delegate = _RecordDelegate.alloc().initWithRecorder_(self)
        nc = NSNotificationCenter.defaultCenter()
        nc.addObserver_selector_name_object_(
            self._delegate, b"sessionRuntimeError:",
            _AVF.AVCaptureSessionRuntimeErrorNotification, session)
        nc.addObserver_selector_name_object_(
            self._delegate, b"sessionWasInterrupted:",
            _AVF.AVCaptureSessionWasInterruptedNotification, session)
        self._observing = True

        session.startRunning()
        self.out_path.unlink(missing_ok=True)
        movie.startRecordingToOutputFileURL_recordingDelegate_(
            NSURL.fileURLWithPath_(str(self.out_path)), self._delegate)
        self._recording_started = True

    def stop(self, timeout: float = 15.0) -> None:
        """Stop recording, flushing the moov atom, then tear down the session.
        Idempotent and exception-safe (called from the orchestrator finally
        block and the Ctrl+C handler).

        DEADLOCK NOTE: both stopRecording()'s finalize and stopRunning()'s graph
        teardown deliver `graphWillStop` to the MAIN thread via
        performSelector:onThread:<main> waitUntilDone:YES. That perform only runs
        if the main thread is servicing a CFRunLoop. If the main thread is parked
        (e.g. time.sleep), this call blocks FOREVER while the capture graph keeps
        writing frames — the 13-hour-recording bug. The main thread therefore MUST
        run a CFRunLoop whenever a recording can be stopped from a worker thread
        (see __main__). Gate stopRecording() on our own start flag, NOT
        movie_out.isRecording(): the latter has been observed returning False on a
        live recording, which skipped the finalize and left the writer running."""
        try:
            mo = self._movie_out
            if mo is not None and self._recording_started and not self._finished.is_set():
                mo.stopRecording()
                # Wait for the finish delegate (moov flush) before stopping the
                # session, else the file can be left without a moov.
                if not self._finished.wait(timeout):
                    log.warning("[record] movie finish callback timed out; "
                                "file may be incomplete")
        except Exception as e:
            log.warning(f"[record] stop error: {e!r}")
        finally:
            try:
                if self._session is not None and self._session.isRunning():
                    self._session.stopRunning()
            except Exception as e:
                log.warning(f"[record] session stop error: {e!r}")
            # Drop the notification observers so a torn-down session can't deliver
            # a late runtime-error to a stale delegate.
            if self._observing:
                try:
                    NSNotificationCenter.defaultCenter().removeObserver_(self._delegate)
                except Exception as e:
                    log.warning(f"[record] observer removal error: {e!r}")
                self._observing = False

    def is_running(self) -> bool:
        # Liveness = "started and not yet finished". Do NOT use
        # movie_out.isRecording(): startRecordingToOutputFileURL: is async, so
        # isRecording() stays False for a short window right after start() — the
        # orchestrator's first loop iteration would read that as capture-ended
        # at 0s. _finished is set only by the didFinishRecording delegate
        # (_on_finish), i.e. when recording truly ends (stop() or a device
        # error), so it has no startup race.
        return not self._finished.is_set()

    @property
    def error(self):
        return self._error

    # — internals ——————————————————————————————————————————————————————————————
    def _pick_format(self, dev):
        """Pick the device format of the wanted size whose max fps >= self.fps,
        preferring '420v' (NV12, MJPEG-backed) — the format that actually
        sustains 30fps at 1080p over USB 2.0 (vs 'yuvs'/uyvy, which caps ~5fps)."""
        best = None
        for f in dev.formats():
            desc = f.formatDescription()
            dims = _CM.CMVideoFormatDescriptionGetDimensions(desc)
            if dims.width != self.want_w or dims.height != self.want_h:
                continue
            maxfps = max((r.maxFrameRate() for r in f.videoSupportedFrameRateRanges()),
                         default=0.0)
            if maxfps + 0.5 < self.fps:
                continue
            sub = _fourcc(_CM.CMFormatDescriptionGetMediaSubType(desc))
            score = (sub == "420v", maxfps)
            if best is None or score > best[0]:
                best = (score, f, sub)
        if best is None:
            return None
        self.chosen_subtype = best[2]
        return best[1]

    def _on_finish(self, error):
        self._error = error
        self._finished.set()

    def _on_session_failure(self, kind, error):
        """AVCaptureSession runtime-error / interruption observer callback. Flips
        the recorder to finished so is_running() goes False and the orchestrator
        stops the (dead) recording instead of writing an empty file for 1h."""
        self._error = error if error is not None else RuntimeError(f"session {kind}")
        log.error(f"[record] AVCaptureSession {kind}: {error}")
        self._finished.set()

    def frames_seen(self) -> int | None:
        """Count of camera buffers delivered to the sidecar output (pre-throttle),
        or None if no sidecar output is attached (fall back to file-size growth)."""
        d = self._data_delegate
        if d is None:
            return None
        try:
            return int(d._frames)
        except Exception:
            return None

    def wait_until_streaming(self, timeout: float = FRAME_LIVENESS_TIMEOUT) -> bool:
        """Block until the capture is provably producing output — camera buffers
        arriving (preferred) or the movie file growing past FRAME_LIVENESS_MIN_BYTES
        — or until the session posts a runtime error, whichever comes first.
        Returns False on timeout (the silent black-session failure) so the caller
        can abort the print instead of recording nothing. Safe to call from the
        orchestrator worker thread; the main thread services the CFRunLoop."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._finished.is_set():
                return False  # a runtime error already fired
            frames = self.frames_seen()
            if frames is not None:
                if frames > 0:
                    return True
            else:
                try:
                    if self.out_path.stat().st_size >= FRAME_LIVENESS_MIN_BYTES:
                        return True
                except OSError:
                    pass
            time.sleep(0.5)
        return False


# ── YOUTUBE API ────────────────────────────────────────────────────────────────

# Serialize all DMG mount/unmount across threads — two concurrent `hdiutil
# attach` calls on the same vault (e.g. the token keepalive firing while an
# upload reads the vault) can collide. RLock so a future nested vault op is safe.
_vault_lock = threading.RLock()


def _vault_get_password() -> str:
    """Read the vault passphrase from the macOS Keychain."""
    result = subprocess.run(
        ["security", "find-generic-password", "-s", YT_VAULT_KEYCHAIN_KEY, "-w"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"Keychain lookup failed for '{YT_VAULT_KEYCHAIN_KEY}': "
            f"{result.stderr.strip()}"
        )
    return result.stdout.strip()


def _vault_mount(password: str) -> tuple[str, str]:
    """
    Attach YT_VAULT_DMG with the given password.
    Returns (mount_point, device_node) parsed from hdiutil output.

    Acquires _vault_lock on success and holds it until the paired
    _vault_unmount — so concurrent threads can't run two `hdiutil attach`
    calls on the same DMG at once. On any failure the lock is released before
    raising (the caller's finally won't run since no device was returned).
    """
    _vault_lock.acquire()
    try:
        result = subprocess.run(
            ["hdiutil", "attach", str(YT_VAULT_DMG), "-stdinpass", "-nobrowse"],
            input=password.encode(),
            capture_output=True,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"hdiutil attach failed: {result.stderr.decode().strip()}"
            )
        # Output lines: "<device>\t<type>\t<mount_point>"
        for line in result.stdout.decode().splitlines():
            parts = [p.strip() for p in line.split("\t")]
            if len(parts) >= 3 and parts[2].startswith("/Volumes/"):
                return parts[2], parts[0]
        raise RuntimeError(
            f"Could not parse mount point from hdiutil output:\n{result.stdout.decode()}"
        )
    except BaseException:
        _vault_lock.release()
        raise


def _vault_unmount(device: str):
    try:
        subprocess.run(["hdiutil", "detach", device, "-quiet"], capture_output=True)
    finally:
        _vault_lock.release()


def _vault_read_token(token_file: str = YT_VAULT_TOKEN_FILE) -> str | None:
    """Read an OAuth token JSON out of the vault; None if absent.

    `token_file` selects WHICH token: the upload token (yt_token.json, bound to
    the channel that receives the videos) or the playlist token
    (YT_VAULT_PLAYLIST_TOKEN_FILE, bound to the channel that owns the playlist).
    The default keeps every existing caller pointed at the upload token.
    """
    try:
        password = _vault_get_password()
        mount_point, device = _vault_mount(password)
        try:
            token_path = Path(mount_point) / token_file
            return token_path.read_text() if token_path.exists() else None
        finally:
            _vault_unmount(device)
            log.info("[vault] Vault unmounted")
    except Exception as e:
        log.warning(f"[vault] Could not read {token_file}: {e}")
        return None


_sftp_password_cache: str | None = None


def _vault_read_sftp_password() -> str:
    """Read the FTPS password from the vault, cached for the process lifetime.

    Mirrors `_vault_read_token` but for SFTP_PASS_FILE (default 'SFTP-pass.txt').
    Called lazily on the first snapshot upload so the vault stays unmounted
    when there's no work to do.
    """
    global _sftp_password_cache
    if _sftp_password_cache is not None:
        return _sftp_password_cache
    password = _vault_get_password()
    mount_point, device = _vault_mount(password)
    try:
        path = Path(mount_point) / SFTP_PASS_FILE
        if not path.exists():
            raise RuntimeError(
                f"{SFTP_PASS_FILE} not found in vault at {mount_point}"
            )
        _sftp_password_cache = path.read_text().strip()
        log.info(f"[vault] SFTP password loaded ({len(_sftp_password_cache)} chars)")
        return _sftp_password_cache
    finally:
        _vault_unmount(device)
        log.info("[vault] Vault unmounted")


def _vault_write_token(token_json: str, token_file: str = YT_VAULT_TOKEN_FILE):
    """Write an OAuth token JSON into the vault (see _vault_read_token for
    which file is which)."""
    password = _vault_get_password()
    mount_point, device = _vault_mount(password)
    try:
        (Path(mount_point) / token_file).write_text(token_json)
        log.info(f"[vault] {token_file} saved to vault")
    finally:
        _vault_unmount(device)
        log.info("[vault] Vault unmounted")


def _load_client_secrets() -> dict:
    """
    Retrieve the OAuth client secrets from the encrypted vault DMG.
    Mounts, reads, then unmounts — the secret never touches disk outside the DMG.
    """
    log.info("[vault] Reading client secrets from encrypted vault...")
    password = _vault_get_password()
    mount_point, device = _vault_mount(password)
    try:
        secret_path = Path(mount_point) / YT_VAULT_SECRET_FILE
        data = json.loads(secret_path.read_text())
        log.info("[vault] Client secrets loaded")
        return data
    finally:
        _vault_unmount(device)
        log.info("[vault] Vault unmounted")


# Set by an uploader thread when googleapiclient's lazy credential refresh dies
# on the wire. The record orchestrator caches its YouTube client for the life of
# the process, so without this a token that expires mid-run poisons that client
# permanently and EVERY later upload fails the same way — the failure mode that
# stranded prints #38–#45. Clearing the cached client on this signal means the
# next print re-reads the vault, so a fresh token (written by backfill_uploads.py)
# is picked up without restarting the daemon.
_YT_AUTH_DEAD = threading.Event()

# Same idea for the playlist token, which is a SEPARATE OAuth grant on a
# separate channel and so dies on its own schedule. Kept distinct from
# _YT_AUTH_DEAD so a dead playlist token never makes the orchestrator throw away
# a perfectly good upload client (and vice versa) — the two failures are
# independent and have different recoveries.
_YT_PLAYLIST_AUTH_DEAD = threading.Event()


def _build_service(token_file: str, interactive: bool, label: str, recovery: str):
    """
    Build a YouTube Data API v3 client from one of the vault's OAuth tokens.

    `token_file` picks the grant — and therefore the CHANNEL. The upload token
    is bound to the channel the videos land on; the playlist token is bound to
    the channel that owns YT_PLAYLIST_ID. They are separate consents and must
    never be interchanged: using the wrong one uploads to the wrong channel or
    404s on the playlist. Client secrets (the same OAuth *client* for both) are
    loaded from the vault only when a consent flow actually runs.

    `interactive=False` makes this refresh-only: if the stored refresh token is
    dead it returns None instead of starting the consent flow. Background
    threads MUST pass False — `run_local_server` blocks until a browser hits its
    loopback redirect and `run_console` blocks on input(), so on a daemon either
    one wedges the calling thread forever. Wedging the record orchestrator would
    stop *recording*, turning a failed-upload problem into a lost-footage one.
    Recovery from a dead refresh token is `./run_auth_setup.sh` (or
    `backfill_uploads.py` for the upload token), run by hand from the GUI
    session where blocking on a human is fine.
    """
    creds = None
    token_json = _vault_read_token(token_file)
    if token_json:
        creds = Credentials.from_authorized_user_info(json.loads(token_json), YT_SCOPES)

    if not creds or not creds.valid:
        refreshed = False
        if creds and creds.expired and creds.refresh_token:
            log.info(f"[{label}] Refreshing OAuth token ({token_file})...")
            try:
                creds.refresh(Request())
                refreshed = True
            except Exception as e:
                log.warning(f"[{label}] Token refresh failed: {e!r}")
                creds = None
        if not refreshed and not interactive:
            log.error(
                f"[{label}] No usable token in {token_file} and interactive auth "
                f"is disabled on this thread. {recovery}"
            )
            return None
        if not refreshed:
            log.info(f"[{label}] Starting OAuth flow for {token_file}...")
            try:
                client_config = _load_client_secrets()
            except Exception as e:
                log.error(f"[vault] Failed to load client secrets: {e}")
                log.warning(f"[{label}] Disabled until the vault is reachable.")
                return None
            flow = InstalledAppFlow.from_client_config(client_config, YT_SCOPES)
            try:
                creds = flow.run_local_server(port=0)
            except Exception:
                # Headless fallback: print URL, prompt for code
                creds = flow.run_console()
        _vault_write_token(creds.to_json(), token_file)

    return build("youtube", "v3", credentials=creds)


def get_youtube_service(interactive: bool = True):
    """The UPLOAD client — the channel that receives the videos (videos.insert).
    Thin wrapper over _build_service; see it for the interactive= contract."""
    return _build_service(
        YT_VAULT_TOKEN_FILE, interactive, "youtube",
        f"Uploads are paused; recordings will be kept in {RECORDING_DIR}. "
        f"Recover with:  ./run_backfill.sh",
    )


def get_playlist_service(interactive: bool = False):
    """The PLAYLIST client — the channel that owns YT_PLAYLIST_ID
    (playlistItems.insert). A different channel from the upload one, hence a
    different token. Defaults to non-interactive: every in-daemon caller is a
    background thread."""
    return _build_service(
        YT_VAULT_PLAYLIST_TOKEN_FILE, interactive, "playlist",
        "Videos will still upload, but nothing will be added to the playlist. "
        "Recover with:  ./run_auth_setup.sh --which playlist",
    )


def refresh_youtube_token_once(token_file: str = YT_VAULT_TOKEN_FILE,
                              tag: str = "youtube-refresh") -> bool:
    """Refresh-only keepalive for one of the vault's OAuth tokens — NEVER
    interactive.

    Reads `token_file` from the vault and, if it carries a refresh_token, forces
    a refresh (which both keeps the access token warm AND proves the refresh
    token is still alive), then writes the fresh token back. On failure it logs
    LOUDLY and returns False — it deliberately does NOT fall back to the browser
    flow the way _build_service does, because this runs on a background thread
    with no human present and an interactive flow there would wedge silently.
    Returns True iff a usable token is present after the attempt.

    Called once per token: the upload grant and the playlist grant are separate
    consents on separate channels and expire independently, so each needs its
    own keepalive (and its own loud line in the log when it dies).
    """
    token_json = _vault_read_token(token_file)
    if not token_json:
        log.info(f"[{tag}] no {token_file} in vault yet — nothing to refresh "
                 f"(run ./run_auth_setup.sh to create it)")
        return False
    try:
        creds = Credentials.from_authorized_user_info(json.loads(token_json), YT_SCOPES)
    except Exception as e:
        log.warning(f"[{tag}] token unreadable: {e!r}")
        return False
    if not creds.refresh_token:
        log.warning(f"[{tag}] token has no refresh_token — cannot keep alive")
        return False
    try:
        creds.refresh(Request())
    except Exception as e:
        log.error(
            f"[{tag}] refresh FAILED for {token_file}: {e!r} — the refresh token "
            f"is expired/revoked. Re-auth with ./run_auth_setup.sh. NOTE: if this "
            f"OAuth app is in Google 'Testing' status its refresh tokens die 7 "
            f"days after issuance regardless of use — and there are now TWO of "
            f"them, so publish the app to production to stop this recurring.")
        return False
    try:
        _vault_write_token(creds.to_json(), token_file)
    except Exception as e:
        log.warning(f"[{tag}] refreshed OK but could not persist to vault: {e!r}")
    log.info(f"[{tag}] token refreshed OK (access-token expiry {creds.expiry})")
    return True


def poll_token_refresh():
    """Background OAuth-token keepalive thread.

    Refreshes the cached token every YT_TOKEN_REFRESH_INTERVAL seconds
    independent of any start signal, so an expired/revoked refresh token shows
    up in the log hours early instead of first surfacing as a wedged recording.
    Refresh-only (see refresh_youtube_token_once). Its own loop so a transient
    error in one cycle never kills the keepalive (unlike _safe_run's run-once)."""
    hrs = YT_TOKEN_REFRESH_INTERVAL / 3600.0
    log.info(f"[youtube-refresh] keepalive started (every {hrs:.1f}h)")
    # Small initial delay so a boot-time keepalive doesn't race a first-recording
    # auth for the vault lock.
    time.sleep(min(120.0, YT_TOKEN_REFRESH_INTERVAL))
    while True:
        try:
            refresh_youtube_token_once()
        except Exception as e:
            log.warning(f"[youtube-refresh] unexpected error: {e!r}")
        # The playlist grant is a second, independent consent (different
        # channel, different expiry clock), so keep it warm too — but only when
        # a playlist is actually configured.
        if YT_PLAYLIST_ID:
            try:
                refresh_youtube_token_once(YT_VAULT_PLAYLIST_TOKEN_FILE,
                                           "playlist-refresh")
            except Exception as e:
                log.warning(f"[playlist-refresh] unexpected error: {e!r}")
        time.sleep(YT_TOKEN_REFRESH_INTERVAL)


# HTTP statuses worth another go: request timeout, rate limit, and the 5xx
# family. Anything else (400 malformed, 403 quota/permission, 404) is a standing
# condition that retrying would only burn API quota on.
_RETRYABLE_HTTP_STATUS = frozenset({408, 429, 500, 502, 503, 504})

# Transport failures worth another go. Most land as OSError — BrokenPipeError,
# TimeoutError, ConnectionResetError, ssl.SSLError and OSError(49, "Can't assign
# requested address") are all subclasses. The other two are NOT OSError
# subclasses and would otherwise fall through to the non-retryable branch:
# httplib2.ServerNotFoundError is a DNS failure, and google.auth's TransportError
# wraps a connection error during a token fetch. RefreshError is deliberately
# absent (and is not a TransportError subclass), so a dead token still fails fast.
_RETRYABLE_TRANSPORT_EXC = (OSError, httplib2.HttpLib2Error, TransportError)


def _http_error_status(e: HttpError) -> int | None:
    return getattr(getattr(e, "resp", None), "status", None)


def _upload_retry_delay(attempt: int) -> float:
    """Exponential backoff before whole-upload attempt N+1 (attempt is 1-based)."""
    return min(UPLOAD_RETRY_BASE_DELAY * (2 ** (attempt - 1)), UPLOAD_RETRY_MAX_DELAY)


def _attempt_upload(youtube, out_path: Path, body: dict, attempt: int) -> tuple[str | None, bool]:
    """One videos.insert against a fresh resumable session.

    Returns (video_id, retryable): video_id is None on failure, and retryable
    says whether an outer retry stands any chance of doing better.
    """
    mime = "video/mp4" if out_path.suffix.lower() == ".mp4" else "video/quicktime"
    media = MediaFileUpload(str(out_path), mimetype=mime,
                            resumable=True, chunksize=8 * 1024 * 1024)
    request = youtube.videos().insert(
        part="snippet,status",
        body=body,
        media_body=media,
    )
    tag = out_path.name if attempt == 1 else f"{out_path.name} (attempt {attempt})"

    response = None
    last_progress_log = 0.0
    while response is None:
        try:
            # num_retries lets googleapiclient retry THIS chunk against the same
            # resumable session, with its own exponential backoff. The server
            # keeps whatever it already received, so this is far cheaper than
            # restarting — it is the layer that should absorb most blips.
            status, response = request.next_chunk(num_retries=UPLOAD_CHUNK_RETRIES)
            if status:
                pct = status.progress() * 100
                if pct - last_progress_log >= 10:
                    log.info(f"[upload] {tag} {pct:.0f}%")
                    last_progress_log = pct
        except RefreshError as e:
            # The stored refresh token is dead (Google expires them 7 days after
            # issuance while the app is in 'Testing'). Never retryable — the
            # orchestrator must drop its cached client and a human must re-auth.
            _YT_AUTH_DEAD.set()
            log.error(
                f"[upload] AUTH DEAD on {out_path.name}: {e!r} — the refresh "
                f"token is expired or revoked. Recordings will keep being made "
                f"and kept in {RECORDING_DIR}, but nothing will upload until you "
                f"re-auth:  ./run_backfill.sh   (permanent fix: publish the "
                f"OAuth app to 'In production' in the Google Cloud console — "
                f"Testing-status refresh tokens always die after 7 days)"
            )
            return None, False
        except HttpError as e:
            status_code = _http_error_status(e)
            retryable = status_code in _RETRYABLE_HTTP_STATUS
            log.error(
                f"[upload] HttpError {status_code} on {tag}: {e}"
                f"{'' if retryable else ' (not retryable)'}"
            )
            return None, retryable
        except _RETRYABLE_TRANSPORT_EXC as e:
            # Exactly what the resumable protocol exists to survive — see the
            # tuple's definition for what lands here and why.
            log.warning(f"[upload] transport error on {tag}: {e!r}")
            return None, True
        except Exception as e:
            # Genuinely unexpected — surface it rather than silently burning
            # attempts on a bug that a retry cannot fix.
            log.error(f"[upload] Unexpected error on {tag}: {e!r}", exc_info=True)
            return None, False

    video_id = response.get("id") if isinstance(response, dict) else None
    if not video_id:
        log.error(f"[upload] videos.insert returned no id: {response!r}")
        return None, False
    return video_id, False


def upload_recording(youtube, out_path: Path, title: str) -> str | None:
    """Resumable upload of a finished recording (QuickTime .mov) via
    videos.insert, retried across transient network failures.

    Returns the 11-char YouTube video id, or None if every attempt failed (in
    which case the caller leaves the .mov on disk for ./run_backfill.sh)."""
    body = {
        "snippet": {
            "title": title,
            "description": (
                "Submit your own art here: "
                f"{SITE_BASE_URL}/upload\n\n"
                f"Recorded by P.A.R. — {SITE_BASE_URL}"
            ),
            "categoryId": "28",  # Science & Technology
        },
        "status": {
            "privacyStatus": "public",
            "selfDeclaredMadeForKids": False,
        },
    }
    for attempt in range(1, UPLOAD_MAX_ATTEMPTS + 1):
        # The source can vanish between attempts (manual cleanup, a backfill run
        # moving it to uploaded/). Retrying then is pointless.
        if not out_path.exists():
            log.error(f"[upload] {out_path} disappeared before attempt {attempt}; giving up")
            return None

        video_id, retryable = _attempt_upload(youtube, out_path, body, attempt)
        if video_id:
            log.info(f"[upload] {out_path.name} -> video_id={video_id}")
            return video_id
        if not retryable:
            return None
        if attempt == UPLOAD_MAX_ATTEMPTS:
            break

        delay = _upload_retry_delay(attempt)
        log.warning(
            f"[upload] retrying {out_path.name} in {delay:.0f}s "
            f"(attempt {attempt + 1}/{UPLOAD_MAX_ATTEMPTS})"
        )
        time.sleep(delay)

    log.error(
        f"[upload] {out_path.name} failed after {UPLOAD_MAX_ATTEMPTS} attempts; "
        f"leaving it on disk for ./run_backfill.sh"
    )
    return None


# ── PLAYLIST ───────────────────────────────────────────────────────────────────
#
# Filing each finished upload into a playlist. The playlist lives on a DIFFERENT
# channel than the videos, which is the whole reason this needs its own client:
# playlistItems.insert is authorised against the playlist's OWNING channel, not
# the video's. A video from another channel can be added freely as long as it is
# public or unlisted (ours are public — see upload_recording's body), so no
# cross-channel permission is involved beyond owning the playlist itself.
#
# This is strictly best-effort: the upload and the gallery attachment are what
# matter, and a playlist failure must never cost a recording. Every path here
# logs and returns instead of raising.

_playlist_service = None
_playlist_service_lock = threading.Lock()
_playlist_fallback_warned = False


def _get_playlist_client(upload_client=None):
    """Cached playlist client, rebuilt when its token is reported dead.

    Falls back to the UPLOAD client when no playlist token exists in the vault —
    which is only correct if the playlist happens to live on the upload channel.
    That's a legitimate single-channel setup, so it's a warning (once), not an
    error; if the playlist is really on another channel the insert just 404s and
    says so.
    """
    global _playlist_service, _playlist_fallback_warned
    with _playlist_service_lock:
        if _YT_PLAYLIST_AUTH_DEAD.is_set():
            log.warning("[playlist] discarding cached playlist client (auth was "
                        "reported dead); re-reading the vault")
            _playlist_service = None
            _YT_PLAYLIST_AUTH_DEAD.clear()
        if _playlist_service is None:
            try:
                # Non-interactive: every caller is a background uploader thread.
                _playlist_service = get_playlist_service(interactive=False)
            except Exception as e:
                log.error(f"[playlist] auth failed: {e!r}")
                _playlist_service = None
        if _playlist_service is None and upload_client is not None:
            if not _playlist_fallback_warned:
                log.warning(
                    f"[playlist] no {YT_VAULT_PLAYLIST_TOKEN_FILE} in the vault "
                    f"— falling back to the upload channel's own credentials. "
                    f"That only works if {YT_PLAYLIST_ID} is a playlist on the "
                    f"UPLOAD channel. If it belongs to another channel, run "
                    f"./run_auth_setup.sh --which playlist.")
                _playlist_fallback_warned = True
            return upload_client
        return _playlist_service


def add_video_to_playlist(youtube, video_id: str, playlist_id: str) -> bool:
    """playlistItems.insert with a couple of quick retries. Returns success.

    Never raises — the caller is finishing a successful upload and a playlist
    problem must not turn that into a failure."""
    body = {
        "snippet": {
            "playlistId": playlist_id,
            "resourceId": {"kind": "youtube#video", "videoId": video_id},
        }
    }
    for attempt in range(1, PLAYLIST_MAX_ATTEMPTS + 1):
        tag = f"{video_id} -> {playlist_id}" + (f" (attempt {attempt})" if attempt > 1 else "")
        try:
            youtube.playlistItems().insert(part="snippet", body=body).execute()
            return True
        except RefreshError as e:
            # Playlist token dead. Distinct from the upload token's death: mark
            # only the playlist client stale so the next print re-reads the
            # vault and picks up a token freshly written by run_auth_setup.sh.
            _YT_PLAYLIST_AUTH_DEAD.set()
            log.error(
                f"[playlist] AUTH DEAD adding {tag}: {e!r} — the PLAYLIST refresh "
                f"token is expired or revoked (this is the second, separate grant "
                f"— uploads are unaffected). Re-auth with: "
                f"./run_auth_setup.sh --which playlist")
            return False
        except HttpError as e:
            status = _http_error_status(e)
            if status == 409:
                # Playlist configured to reject duplicates and the video is
                # already in it. Nothing to do, and definitely not an error.
                log.info(f"[playlist] {video_id} already in {playlist_id}")
                return True
            retryable = status in _RETRYABLE_HTTP_STATUS
            hint = ""
            if status == 404:
                hint = (" — playlist not found for THIS token's channel; check "
                        "YT_PLAYLIST_ID and that the playlist token was consented "
                        "for the channel that owns it")
            elif status == 403:
                hint = (" — forbidden; the playlist token's channel does not own "
                        "this playlist")
            log.error(f"[playlist] HttpError {status} on {tag}: {e}{hint}"
                      f"{'' if retryable else ' (not retryable)'}")
            if not retryable:
                return False
        except _RETRYABLE_TRANSPORT_EXC as e:
            log.warning(f"[playlist] transport error on {tag}: {e!r}")
        except Exception as e:
            log.error(f"[playlist] unexpected error on {tag}: {e!r}", exc_info=True)
            return False

        if attempt == PLAYLIST_MAX_ATTEMPTS:
            break
        delay = PLAYLIST_RETRY_BASE_DELAY * (2 ** (attempt - 1))
        log.warning(f"[playlist] retrying {video_id} in {delay:.0f}s "
                    f"(attempt {attempt + 1}/{PLAYLIST_MAX_ATTEMPTS})")
        time.sleep(delay)

    log.error(f"[playlist] gave up adding {video_id} to {playlist_id} after "
              f"{PLAYLIST_MAX_ATTEMPTS} attempts — add it by hand if you want it there")
    return False


def attach_to_playlist(video_id: str, gallery_id: int | None = None,
                       upload_client=None) -> bool:
    """Best-effort 'file this upload into the configured playlist'. No-ops when
    YT_PLAYLIST_ID is unset. Never raises."""
    if not YT_PLAYLIST_ID:
        return False
    try:
        client = _get_playlist_client(upload_client)
        if client is None:
            log.error(f"[playlist] no playlist client; {video_id} NOT added to "
                      f"{YT_PLAYLIST_ID}")
            return False
        ok = add_video_to_playlist(client, video_id, YT_PLAYLIST_ID)
        if ok:
            where = f" (#{gallery_id})" if gallery_id is not None else ""
            log.info(f"[playlist] added {video_id}{where} to {YT_PLAYLIST_ID}")
        return ok
    except Exception as e:
        log.error(f"[playlist] attach raised for {video_id}: {e!r}", exc_info=True)
        return False


# ── CAMERA ─────────────────────────────────────────────────────────────────────

def _list_avfoundation_devices() -> tuple[list[tuple[str, str]], list[tuple[str, str]]]:
    """Enumerate ffmpeg's avfoundation devices. Returns (video, audio), each a
    list of (index, name) tuples in the order ffmpeg reports them.

    `ffmpeg -f avfoundation -list_devices true -i ""` writes the device list to
    stderr and exits non-zero (it treats the empty input as an error) — that's
    expected, so we ignore the return code and parse stderr. Each device line
    looks like `[AVFoundation indev @ 0x..] [1] Brio 100`; the FIRST bracketed
    integer is the indev handle, the SECOND is the device index we want."""
    try:
        result = subprocess.run(
            ["ffmpeg", "-hide_banner", "-f", "avfoundation",
             "-list_devices", "true", "-i", ""],
            capture_output=True, text=True, timeout=15,
        )
    except FileNotFoundError as e:
        # The camera is fine; ffmpeg itself isn't on PATH. Surface that plainly
        # so it doesn't get logged as "no device matching CAMERA_NAME found".
        raise RuntimeError(
            "ffmpeg not found on PATH — install it (e.g. `brew install ffmpeg`). "
            "The camera cannot be enumerated without it."
        ) from e
    video: list[tuple[str, str]] = []
    audio: list[tuple[str, str]] = []
    section: str | None = None
    for line in result.stderr.splitlines():
        if "AVFoundation video devices:" in line:
            section = "video"
            continue
        if "AVFoundation audio devices:" in line:
            section = "audio"
            continue
        if section is None:
            continue
        # `[1] Brio 100` — match the LAST `[<int>] <name>` on the line so the
        # `[AVFoundation indev @ 0x..]` prefix is skipped.
        m = re.search(r"\[(\d+)\]\s+(.*\S)\s*$", line)
        if m:
            (video if section == "video" else audio).append((m.group(1), m.group(2)))
    return video, audio


def _match_device(devices: list[tuple[str, str]], prefix: str) -> tuple[str, str] | None:
    """First device whose name starts (case-insensitively) with `prefix`."""
    want = prefix.strip().lower()
    for idx, name in devices:
        if name.lower().startswith(want):
            return idx, name
    return None


def _refresh_camera() -> str | None:
    """Enumerate avfoundation devices, find the VIDEO device whose name starts
    with CAMERA_NAME (case-insensitive), cache its index as the input spec, and
    return it. The spec is always the bare video index ("1") — audio is never
    captured. Returns None (leaving the cache untouched) if no matching video
    device is currently present.

    Enumerating does NOT open the camera, so this is cheap and leaves the
    privacy LED off — safe to call on every keeper tick."""
    global _camera_spec
    try:
        video, _audio = _list_avfoundation_devices()
    except Exception as e:
        log.warning(f"[camera] device enumeration failed: {e!r}")
        return None

    vmatch = _match_device(video, CAMERA_NAME)
    if vmatch is None:
        log.warning(f"[camera] no device matching {CAMERA_NAME!r} found "
                    f"(present: {[n for _, n in video]})")
        return None
    vidx, vname = vmatch

    with _camera_lock:
        changed = vidx != _camera_spec
        _camera_spec = vidx
    if changed:
        log.info(f"[camera] matched {vname!r} — avfoundation video index {vidx!r}")
    return vidx


def verify_camera_accessible() -> bool:
    """True if the configured webcam currently enumerates. Doesn't open the
    device (so no privacy-LED flash, and no contention with an in-flight
    recording) — for a USB cam, enumerating means openable; if another process
    is holding it, the recording's ffmpeg surfaces that at spawn time."""
    return _refresh_camera() is not None


def camera_keeper() -> None:
    """Background thread — re-discover the webcam every CAMERA_POLL_INTERVAL
    seconds whenever a recording isn't in progress.

    Unlike the old RTSP keeper there are no lightweight "warm knocks" and no
    connected/disconnected fast path: each tick simply re-enumerates the
    avfoundation device list and refreshes the cached input spec, so the cam is
    always re-found even after an unplug/replug or an index change — even if it
    was connected on the previous tick. Listing devices doesn't open the
    camera, so this never contends with an active recording (the keeper still
    pauses entirely while one is in flight)."""
    log.info(f"[camera] Keeper started (interval={CAMERA_POLL_INTERVAL}s, "
             f"name prefix={CAMERA_NAME!r})")
    while True:
        if not _recording_active.is_set():
            try:
                _refresh_camera()  # logs its own failure reason (missing ffmpeg
                                   # vs. missing camera); no extra line needed
            except Exception as e:
                log.warning(f"[camera] keeper probe error: {e!r}")
        time.sleep(CAMERA_POLL_INTERVAL)


# ── SNAPSHOT ───────────────────────────────────────────────────────────────────

def _read_live_frame(dst: Path, timeout: float = 6.0, max_age: float = 20.0) -> bool:
    """Copy the recorder's live JPEG sidecar (LATEST_FRAME_PATH) to `dst`,
    waiting up to `timeout`s for a fresh, fully-written frame. Returns True on
    success.

    The recording ffmpeg rewrites the sidecar ~1fps, so a copy can occasionally
    catch a half-written file — we validate each copy by decoding it with PIL
    and retry on failure. `max_age` rejects a stale frame left over from a prior
    recording (e.g. if the new recording hasn't produced a frame yet)."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            if LATEST_FRAME_PATH.exists():
                if (time.time() - LATEST_FRAME_PATH.stat().st_mtime) <= max_age:
                    shutil.copyfile(LATEST_FRAME_PATH, dst)
                    with Image.open(dst) as im:
                        im.load()  # force decode; raises on a torn JPEG
                    return True
        except Exception:
            pass  # mid-rewrite / torn read — fall through and retry
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.4)


def grab_snapshot() -> Path | None:
    """Return a local JPEG of the camera view, or None.

    Two sources, because a USB webcam allows only ONE opener at a time:
      - while a print is recording, copy the recorder's live JPEG sidecar
        (LATEST_FRAME_PATH) — we must NOT open the device out from under the
        recording's ffmpeg;
      - otherwise, open the camera directly for a one-shot frame.
    """
    SNAPSHOT_LOCAL_DIR.mkdir(parents=True, exist_ok=True)
    out_path = SNAPSHOT_LOCAL_DIR / datetime.now().strftime("snap_%Y%m%d_%H%M%S.jpg")

    if _recording_active.is_set():
        if _read_live_frame(out_path):
            log.info(f"[snapshot] Grabbed {out_path.name} from live recording frame")
            return out_path
        log.error("[snapshot] recording active but no fresh live frame available")
        out_path.unlink(missing_ok=True)
        return None

    spec = _refresh_camera()
    if spec is None:
        log.error(f"[snapshot] no device matching {CAMERA_NAME!r}; cannot grab frame")
        return None

    cmd = [
        "ffmpeg", "-y",
        "-f", "avfoundation",
        "-framerate", CAMERA_FRAMERATE,
        "-i", spec,
        "-frames:v", "1",
        # ffmpeg 8.x's image2 muxer refuses a single fixed filename without
        # -update 1 (it otherwise wants a %d sequence pattern): it still decodes
        # the frame and writes the JPEG, but exits non-zero, so without this the
        # grab is treated as a failure and the good file gets unlinked below.
        "-update", "1",
        "-q:v", "2",
        str(out_path),
    ]
    result = subprocess.run(cmd, capture_output=True)
    if result.returncode == 0 and out_path.exists():
        log.info(f"[snapshot] Grabbed {out_path.name}")
        return out_path
    else:
        log.error(f"[snapshot] ffmpeg failed: {result.stderr.decode()[-200:]}")
        out_path.unlink(missing_ok=True)
        return None


def _join_remote(*parts: str) -> str:
    """Join with '/' but skip empty pieces — so '' + '4' + 'image.jpg' -> '4/image.jpg'."""
    return "/".join(p.strip("/") for p in parts if p)


def upload_snapshot(local_path: Path, gallery_id: int | None) -> bool:
    """Upload snapshot to Site5 gallery via FTPS (explicit TLS on port 21).

    If gallery_id is provided, the file lands at gallery/<id>/image.jpg so it
    can be associated with the gallery entry. Otherwise it falls back to the
    timestamped filename in the gallery root for ad-hoc captures.

    Why FTPS and not SFTP: the addon FTP account on this shared Site5 host is
    FTP/FTPS only — SSH/SFTP on :22 is reserved for the main cPanel user.
    """
    try:
        password = _vault_read_sftp_password()

        # Shared hosts present a wildcard / shared-CN cert that won't match
        # `shared187.accountservergroup.com`. Disable verification — TLS still
        # encrypts the password in transit, which is the threat we care about.
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE

        ftp = ftplib.FTP_TLS(context=ctx, timeout=30)
        ftp.connect(SFTP_HOST, SFTP_PORT)
        ftp.login(SFTP_USER, password)
        ftp.prot_p()  # encrypt data channel too
        try:
            if gallery_id is not None:
                entry_dir = _join_remote(SFTP_REMOTE_DIR, str(gallery_id))
                try:
                    ftp.mkd(entry_dir)
                except ftplib.error_perm as e:
                    # 550 = already exists; anything else is a real failure.
                    if not str(e).startswith("550"):
                        raise
                remote_path = _join_remote(entry_dir, "image.jpg")
            else:
                remote_path = _join_remote(SFTP_REMOTE_DIR, local_path.name)

            with open(local_path, "rb") as f:
                ftp.storbinary(f"STOR {remote_path}", f)
        finally:
            try:
                ftp.quit()
            except Exception:
                ftp.close()

        log.info(f"[snapshot] Uploaded to {remote_path}")
        local_path.unlink()
        return True
    except Exception as e:
        log.error(f"[snapshot] FTPS upload failed: {e}")
        return False


def poll_snapshot_queue():
    """Background thread — polls server for pending snapshot requests."""
    log.info("[snapshot] Poller started")
    while True:
        try:
            resp = requests.post(
                SNAPSHOT_REQUEST_URL,
                data={"secret": SNAPSHOT_SECRET},
                headers={"User-Agent": "P.A.R./1.0"},
                timeout=10,
            )
            if resp.status_code == 200:
                data = resp.json()
                if data.get("ok") and data.get("entry"):
                    gallery_id = data.get("id")
                    target = f"gallery/{gallery_id}" if gallery_id is not None else "gallery root"
                    log.info(f"[snapshot] Request received (queued at {data['entry']}, target {target}), grabbing frame...")
                    snap = grab_snapshot()
                    if snap:
                        upload_snapshot(snap, gallery_id)

                    # Tie this snapshot to the in-flight recording (if any) so it
                    # doubles as the "print done → stop recording" signal. A
                    # snapshot popped with id=null is the Arduino's post-display
                    # touch (its body id is dropped by snapshot-request.php), so
                    # fall back to the recording's own id. Signal after the grab
                    # (the live-frame sidecar copy already happened) and
                    # regardless of upload success, so a failed snapshot upload
                    # doesn't strand the recording at the cap.
                    with _inflight_lock:
                        inflight_id = _inflight_id
                    effective_id = gallery_id if gallery_id is not None else inflight_id
                    if effective_id is not None:
                        global _snapshot_stop_id, _snapshot_stop_ts
                        with _snapshot_stop_lock:
                            _snapshot_stop_id = effective_id
                            _snapshot_stop_ts = time.monotonic()
            elif resp.status_code == 204:
                log.debug("[snapshot] Queue empty")
            else:
                log.warning(f"[snapshot] Poll returned {resp.status_code}")
        except Exception as e:
            log.warning(f"[snapshot] Poll error: {e}")

        time.sleep(SNAPSHOT_POLL_INTERVAL)


# ── MODERATION ─────────────────────────────────────────────────────────────────
#
# Polls the server for pixel-art submissions awaiting moderation. Each item is
# checked by two parallel Claude SDK calls (image + name). Both must clear with
# high confidence to auto-approve; anything else (review, reject, low conf, or
# an exception) sends an email to the human moderator with one-click links.
#
# Auth uses MOD_SECRET (falls back to SNAPSHOT_SECRET, since both endpoints
# live on the same trust boundary — the Mac Mini).
#
# NOTE: The system prompts and user prompts below are PLACEHOLDERS. Tune them
# to your community's policy before relying on auto-action.

_MOD_IMAGE_SYSTEM_PROMPT = """\
You are a content-policy reviewer for a community pixel-art LED display
(37x18 monochrome).

First, decide whether you can confidently identify WHAT the image depicts
(a smiley, a heart, a yin-yang, a cat, a house, the letters "HI", etc.).

APPROVE when ALL of:
  (a) you confidently identify the subject,
  (b) that subject is plainly benign (everyday object, animal, plant, food,
      vehicle, scene, letterform, widely-recognized benign cultural/religious
      symbol with no hate-imagery overlap — e.g. yin-yang, peace sign, heart,
      star, cross, om, ankh, infinity, smiley),
  (c) the subject has no plausible reading as a hate symbol, slur,
      sexual content, gore, doxxing, or targeted harassment.

REVIEW when ANY of:
  - You can describe the geometry but cannot confidently name what it
    depicts (e.g. "four arms radiating from a center, but I'm not sure what
    it represents" — escalate, the low-res may be hiding the real subject).
  - The shape visually overlaps with or could be misread as a hate symbol.
  - Politically charged imagery (non-hate political speech, partisan emblems).
  - Reasonable viewers could disagree on appropriateness for public display.

REJECT when the image clearly depicts a hate symbol, sexual content, gore,
slurs spelled as text, doxxing, or targeted harassment.

Curse words alone are NOT reject.

Important: an "abstract pattern" or "geometric shape" that you can't tie to
a specific benign subject is NOT auto-approve — it goes to review. The
low-res renderer can blur recognizable symbols into shapes that look
abstract; let a human decide.

Respond with ONLY valid JSON, no markdown, no preamble. Schema:
{"verdict": "approve" | "review" | "reject",
 "confidence": <float 0.0-1.0>,
 "flags": [<short strings>],
 "reasoning": "<max 20 words>"
}
"""

_MOD_NAME_SYSTEM_PROMPT = """\
You are a content-policy reviewer for a community pixel-art display. You will be given the user-submitted display name/title (free text, up to ~100 chars).

Reject for: slurs, hate speech, sexual content, doxxing, targeted harassment, or spam/scam URLs. Approve harmless names (including silly, edgy-but-clean, or non-English content). Use "review" only when uncertain.

Curse words are NOT reason for rejection.

Non-hate political speech should be a "review" descision.

Respond with ONLY valid JSON, no markdown, no preamble. Schema:
{"verdict": "approve" | "review" | "reject",
 "confidence": <float 0.0-1.0>,
 "flags": [<short strings>],
}
"""

_MOD_ARTIST_SYSTEM_PROMPT = """\
You are a content-policy reviewer for a community pixel-art display. You will be given the user-submitted artist name (free text, up to ~100 chars). It is an optional credit line, shown in the gallery as "by <artist>".

Reject for: slurs, hate speech, sexual content, doxxing, targeted harassment, or spam/scam URLs. Approve harmless names (including silly, edgy-but-clean, non-English content, real names, handles, or pseudonyms). Use "review" only when uncertain.

Curse words are NOT reason for rejection.

Impersonation of a real, identifiable public figure or organization should be a "review" descision.

Non-hate political speech should be a "review" descision.

Respond with ONLY valid JSON, no markdown, no preamble. Schema:
{"verdict": "approve" | "review" | "reject",
 "confidence": <float 0.0-1.0>,
 "flags": [<short strings>],
}
"""


PIXEL_W = 37
PIXEL_H = 18
PIXEL_ON  = (0x02, 0xb2, 0xd9)  # cyan #02b2d9
PIXEL_OFF = (0x00, 0x00, 0x00)
# Different scales / resampling for the two consumers:
# - Claude path uses BILINEAR. NEAREST stretches single-pixel diagonal lines
#   into staircases of disconnected boxes, and the vision model then describes
#   things like a rotated swastika as "two chevrons" or a manji as "four
#   hollow squares". BILINEAR smooths the staircases back into recognizable
#   strokes and the model correctly flags hate symbols.
# - Email path stays NEAREST because human moderators want crisp pixels.
CLAUDE_PIXEL_SCALE = 8    # 296x144 BILINEAR PNG sent to the model
EMAIL_PIXEL_SCALE  = 12   # 444x216 NEAREST PNG embedded in the moderator email


def packed_bitmap_to_png_b64(bitmap_b64: str, scale: int, *, smooth: bool = False) -> str:
    """Decode the 84-byte packed bitmap (37x18 bits, MSB-first per byte) into
    an upscaled PNG and return its base64 string.

    smooth=True uses BILINEAR resampling (for the Claude moderation path —
    smooth lines let the vision model recognize hate-symbol shapes). Default
    NEAREST preserves the crisp pixel look for the email path.
    """
    raw = base64.b64decode(bitmap_b64)
    img = Image.new("RGB", (PIXEL_W, PIXEL_H), PIXEL_OFF)
    for i in range(PIXEL_W * PIXEL_H):
        if (raw[i // 8] >> (7 - (i % 8))) & 1:
            img.putpixel((i % PIXEL_W, i // PIXEL_W), PIXEL_ON)
    resample = Image.Resampling.BILINEAR if smooth else Image.Resampling.NEAREST
    img = img.resize((PIXEL_W * scale, PIXEL_H * scale), resample)
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode("ascii")


def _claude_json(system_prompt: str, user_content, model: str) -> dict:
    """Run one Claude Agent SDK call and parse its JSON reply.

    `user_content` is either a plain string OR a list of content blocks
    (text + image). Image content REQUIRES the streaming-input mode via
    ClaudeSDKClient — `query()` silently strips images per the SDK docs:
    https://code.claude.com/docs/en/agent-sdk/streaming-vs-single-mode
    ("Single message input mode does NOT support: Direct image attachments").
    """
    async def _message_gen(content):
        yield {
            "type": "user",
            "message": {
                "role": "user",
                "content": content,  # list of {type, ...} blocks
            },
        }

    async def _run():
        result_text = ""
        block_types_seen: list[str] = []
        options = ClaudeAgentOptions(
            system_prompt=system_prompt,
            max_turns=1,
            allowed_tools=[],
            model=model,
            effort=MOD_REASONING,
        )

        with anyio.move_on_after(MOD_CHECK_TIMEOUT) as scope:
            if isinstance(user_content, str):
                # Text-only fast path: query() is fine.
                async for message in claude_query(prompt=user_content, options=options):
                    if isinstance(message, AssistantMessage):
                        for block in message.content:
                            block_types_seen.append(type(block).__name__)
                            if isinstance(block, TextBlock):
                                result_text += block.text
            else:
                # Image-bearing path: must use ClaudeSDKClient for the CLI
                # to actually pass the image through to the model.
                async with ClaudeSDKClient(options=options) as client:
                    await client.query(_message_gen(user_content))
                    async for message in client.receive_response():
                        if isinstance(message, AssistantMessage):
                            for block in message.content:
                                block_types_seen.append(type(block).__name__)
                                if isinstance(block, TextBlock):
                                    result_text += block.text
                        if isinstance(message, ResultMessage):
                            break

        if scope.cancelled_caught:
            raise TimeoutError(
                f"Claude SDK call exceeded {MOD_CHECK_TIMEOUT}s "
                f"(model={model}, blocks_so_far={block_types_seen})"
            )
        cleaned = result_text.strip()
        if cleaned.startswith("```"):
            cleaned = cleaned.strip("`")
            if cleaned.lower().startswith("json"):
                cleaned = cleaned[4:]
            cleaned = cleaned.strip()
        try:
            return json.loads(cleaned)
        except json.JSONDecodeError:
            log.warning(
                f"[mod] Claude returned non-JSON (model={model}). "
                f"blocks={block_types_seen} raw={cleaned[:500]!r}"
            )
            raise

    return anyio.run(_run)


def check_image(png_b64: str) -> dict:
    """png_b64 is a base64-encoded PNG (NOT the raw packed bitmap).

    The bundled Claude CLI flakes ~30% of the time on cold-start image
    requests (subprocess stalls after the SystemMessage + RateLimitEvent
    bootstrap, never produces an AssistantMessage). Healthy calls finish
    in <10s. We retry on TimeoutError up to MOD_CHECK_RETRIES extra times.
    """
    user_content = [
        {
            "type": "image",
            "source": {
                "type": "base64",
                "media_type": "image/png",
                "data": png_b64,
            },
        },
        {
            "type": "text",
            "text": (
                "Moderate this 37x18 pixel-art image (cyan-on-black, "
                "upscaled for visibility).\n\n"
                'Respond ONLY with JSON: {"verdict":"approve|review|reject",'
                '"confidence":0.0-1.0,"flags":[...],"reasoning":"max 20 words"}'
            ),
        },
    ]
    last_err: Exception | None = None
    for attempt in range(1 + MOD_CHECK_RETRIES):
        try:
            return _claude_json(_MOD_IMAGE_SYSTEM_PROMPT, user_content, MOD_IMAGE_MODEL)
        except TimeoutError as e:
            last_err = e
            if attempt < MOD_CHECK_RETRIES:
                log.warning(
                    f"[mod] image check attempt {attempt+1} timed out "
                    f"({MOD_CHECK_TIMEOUT}s); retrying"
                )
    assert last_err is not None
    raise last_err


def check_name(name: str) -> dict:
    user_prompt = (
        f"Moderate this submission name for a pixel art community site: {name}\n\n"
        'Respond ONLY with JSON: {"verdict":"approve|review|reject",'
        '"confidence":0.0-1.0,"flags":[...],"reasoning":"max 20 words"}'
    )
    return _claude_json(_MOD_NAME_SYSTEM_PROMPT, user_prompt, MOD_NAME_MODEL)


def _timed_check(fn, *args):
    """Run one moderation check and report its OWN duration as (result, seconds).

    The three checks run concurrently but are read back SEQUENTIALLY, so logging
    `now - batch_start` credits each check with the wall time of every check read
    before it. That made the instant blank-artist short-circuit (which issues no
    SDK call at all) log as a 6-9s model call, indistinguishable from a real one.
    """
    t = time.monotonic()
    return fn(*args), time.monotonic() - t


def check_artist(artist: str) -> dict:
    """Moderate the optional artist credit, independently of the piece name.

    The field is optional, so a blank value is not "an empty name to judge" —
    it is nothing at all. Short-circuit to a synthetic high-confidence approve
    rather than asking the model about an empty string, which would otherwise
    drag every artist-less submission into human review.
    """
    artist = (artist or "").strip()
    if not artist:
        return {"verdict": "approve", "confidence": 1.0,
                "flags": [], "reasoning": "no artist name given"}

    user_prompt = (
        f"Moderate this artist name for a pixel art community site: {artist}\n\n"
        'Respond ONLY with JSON: {"verdict":"approve|review|reject",'
        '"confidence":0.0-1.0,"flags":[...],"reasoning":"max 20 words"}'
    )
    return _claude_json(_MOD_ARTIST_SYSTEM_PROMPT, user_prompt, MOD_ARTIST_MODEL)


def _render_result(label: str, result: dict | Exception) -> str:
    if isinstance(result, Exception):
        return (
            f"<h3>{label}</h3>"
            f"<p style='color:#a00'><strong>Exception:</strong> "
            f"{type(result).__name__}: {result}</p>"
        )
    verdict = result.get("verdict", "?")
    conf    = result.get("confidence", "?")
    flags   = ", ".join(result.get("flags", []) or []) or "—"
    reason  = result.get("reasoning", "")
    return (
        f"<h3>{label}</h3>"
        f"<p><strong>Verdict:</strong> {verdict} "
        f"(<strong>confidence:</strong> {conf})</p>"
        f"<p><strong>Flags:</strong> {flags}</p>"
        f"<p><strong>Reasoning:</strong> {reason}</p>"
    )


def send_mod_email(item_id: str, name: str, png_b64: str,
                   image_result, name_result,
                   artist: str = "", artist_result=None) -> bool:
    if not (NOTIFY_EMAIL and SMTP_HOST and SMTP_USER and SMTP_PASS):
        log.warning("[mod] Email not sent — SMTP not configured")
        return False

    approve_url = f"{MOD_ACTION_URL}?secret={MOD_SECRET}&id={item_id}&verdict=approve"
    reject_url  = f"{MOD_ACTION_URL}?secret={MOD_SECRET}&id={item_id}&verdict=reject"

    # Gmail (and many other clients) refuse to render inline data: URIs in
    # HTML email — they show a black rectangle. Reference the image via a
    # cid: URL and attach it as a related MIME part instead.
    image_cid = f"par-bitmap-{item_id}@par.local"
    html = f"""\
<html><body style="font-family:system-ui,sans-serif;max-width:48rem">
  <h2>P.A.R. moderation review: #{item_id}</h2>
  <p><strong>Submission name:</strong> {name or '<em>(none)</em>'}</p>
  <p><strong>Artist name:</strong> {artist or '<em>(none)</em>'}</p>
  <p>
    <img src="cid:{image_cid}"
         style="image-rendering:pixelated;width:456px;height:216px;
                border:1px solid #444;background:#000"/>
  </p>
  {_render_result('Image check', image_result)}
  {_render_result('Name check', name_result)}
  {_render_result('Artist check', artist_result) if artist_result is not None else ''}
  <p style="margin-top:2rem">
    <a href="{approve_url}"
       style="background:#0a0;color:#fff;padding:0.6rem 1rem;
              text-decoration:none;border-radius:4px;margin-right:1rem">
      ✓ Approve
    </a>
    <a href="{reject_url}"
       style="background:#a00;color:#fff;padding:0.6rem 1rem;
              text-decoration:none;border-radius:4px">
      ✗ Reject
    </a>
  </p>
</body></html>
"""

    msg = MIMEMultipart("related")
    msg["Subject"] = f"[PAR Mod] Review needed — #{item_id}: {name or '(no name)'}"
    msg["From"]    = SMTP_USER
    msg["To"]      = NOTIFY_EMAIL

    alt = MIMEMultipart("alternative")
    alt.attach(MIMEText("HTML view required — open in an HTML-capable client.", "plain"))
    alt.attach(MIMEText(html, "html"))
    msg.attach(alt)

    img = MIMEImage(base64.b64decode(png_b64), _subtype="png")
    img.add_header("Content-ID", f"<{image_cid}>")
    img.add_header("Content-Disposition", "inline", filename=f"par-{item_id}.png")
    msg.attach(img)

    try:
        with smtplib.SMTP_SSL(SMTP_HOST, SMTP_PORT, timeout=30) as smtp:
            smtp.login(SMTP_USER, SMTP_PASS)
            smtp.send_message(msg)
        log.info(f"[mod] Email sent for #{item_id}")
        return True
    except Exception as e:
        log.error(f"[mod] Failed to send email for #{item_id}: {e}")
        return False


def _post_mod_action(item_id: str, verdict: str) -> bool:
    if not MOD_ACTION_URL:
        log.warning(f"[mod] mod-action skipped for #{item_id}: MOD_ACTION_URL unset")
        return False
    try:
        resp = requests.post(
            MOD_ACTION_URL,
            data={"secret": MOD_SECRET, "id": item_id, "verdict": verdict},
            headers={"User-Agent": "P.A.R./1.0"},
            timeout=15,
        )
        if resp.status_code == 200:
            return True
        log.warning(f"[mod] mod-action POST for #{item_id} verdict={verdict} "
                    f"returned {resp.status_code}: {resp.text[:200]}")
        return False
    except Exception as e:
        log.error(f"[mod] mod-action POST failed for #{item_id}: {e}")
        return False


def _is_clear_approve(r: dict) -> bool:
    return (r.get("verdict") == "approve"
            and float(r.get("confidence", 0.0)) >= MOD_AUTO_THRESHOLD)


def process_mod_item(item: dict) -> None:
    item_id   = str(item.get("id", ""))
    image_b64 = item.get("image_b64", "")
    name      = item.get("name", "") or ""
    # Optional credit line; absent on legacy queue entries and on submissions
    # that left it blank, so it is deliberately NOT part of the guard below.
    artist    = item.get("artist", "") or ""

    if not item_id or not image_b64:
        log.warning(f"[mod] Skipping malformed item: {item!r}")
        return

    # `image_b64` from the queue is the 84-byte packed bitmap. Render two
    # PNGs at different scales: small one for Claude, big one for the email.
    try:
        png_for_claude = packed_bitmap_to_png_b64(image_b64, CLAUDE_PIXEL_SCALE, smooth=True)
        png_for_email  = packed_bitmap_to_png_b64(image_b64, EMAIL_PIXEL_SCALE)
    except Exception as e:
        log.error(f"[mod] Could not decode bitmap for #{item_id}: {e!r} — skipping")
        return

    t0 = time.monotonic()
    log.info(f"[mod] #{item_id} → checking (name={name!r}, artist={artist!r}, "
             f"effort={MOD_REASONING})")

    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        fut_img    = pool.submit(_timed_check, check_image, png_for_claude)
        fut_name   = pool.submit(_timed_check, check_name, name)
        fut_artist = pool.submit(_timed_check, check_artist, artist)

        # Outer timeout = generous bound around per-attempt timeout * (1 + retries)
        # so the threadpool .result() doesn't fire before check_image's
        # internal retry loop has had a chance to run.
        outer_budget = MOD_CHECK_TIMEOUT * (2 + MOD_CHECK_RETRIES)

        try:
            image_result, image_secs = fut_img.result(timeout=outer_budget)
            log.info(f"[mod] #{item_id} image check done in "
                     f"{image_secs:.1f}s: "
                     f"verdict={image_result.get('verdict')!r} "
                     f"conf={image_result.get('confidence')}")
        except concurrent.futures.TimeoutError:
            log.warning(f"[mod] image check exhausted {outer_budget}s budget "
                        f"for #{item_id} — treating as failure")
            image_result = TimeoutError(f"image check >{outer_budget}s")
        except Exception as e:
            log.warning(f"[mod] image check raised for #{item_id} after "
                        f"{time.monotonic()-t0:.1f}s: {e}")
            image_result = e

        try:
            name_result, name_secs = fut_name.result(timeout=outer_budget)
            log.info(f"[mod] #{item_id} name check done in "
                     f"{name_secs:.1f}s: "
                     f"verdict={name_result.get('verdict')!r} "
                     f"conf={name_result.get('confidence')}")
        except concurrent.futures.TimeoutError:
            log.warning(f"[mod] name check exhausted {outer_budget}s budget "
                        f"for #{item_id} — treating as failure")
            name_result = TimeoutError(f"name check >{outer_budget}s")
        except Exception as e:
            log.warning(f"[mod] name check raised for #{item_id} after "
                        f"{time.monotonic()-t0:.1f}s: {e}")
            name_result = e

        try:
            artist_result, artist_secs = fut_artist.result(timeout=outer_budget)
            if not artist.strip():
                # check_artist short-circuits a blank credit line; no SDK session
                # is spent. Logged distinctly so this is visible in stream.log.
                log.info(f"[mod] #{item_id} artist check SKIPPED (no artist name) "
                         f"— no SDK call, synthetic approve")
            else:
                log.info(f"[mod] #{item_id} artist check done in "
                         f"{artist_secs:.1f}s: "
                         f"verdict={artist_result.get('verdict')!r} "
                         f"conf={artist_result.get('confidence')}")
        except concurrent.futures.TimeoutError:
            log.warning(f"[mod] artist check exhausted {outer_budget}s budget "
                        f"for #{item_id} — treating as failure")
            artist_result = TimeoutError(f"artist check >{outer_budget}s")
        except Exception as e:
            log.warning(f"[mod] artist check raised for #{item_id} after "
                        f"{time.monotonic()-t0:.1f}s: {e}")
            artist_result = e

    # Auto-approve only when ALL THREE checks come back as high-confidence
    # approve. Per user spec, anything else — including a confident reject —
    # goes to a human via email rather than being auto-rejected.
    results = (image_result, name_result, artist_result)
    if (all(isinstance(r, dict) for r in results)
            and all(_is_clear_approve(r) for r in results)):
        log.info(f"[mod] auto-approve #{item_id} "
                 f"(img conf={image_result.get('confidence')}, "
                 f"name conf={name_result.get('confidence')}, "
                 f"artist conf={artist_result.get('confidence')})")
        _post_mod_action(item_id, "approve")
        return

    log.info(f"[mod] human review #{item_id} — emailing moderator")
    sent = send_mod_email(item_id, name, png_for_email, image_result, name_result,
                          artist=artist, artist_result=artist_result)
    if sent:
        _post_mod_action(item_id, "email_sent")


def poll_mod_queue() -> None:
    log.info(f"[mod] Poller started (interval={MOD_POLL_INTERVAL}s, queue={MOD_QUEUE_URL})")
    if not (MOD_QUEUE_URL and MOD_ACTION_URL and MOD_SECRET):
        log.warning("[mod] Disabled — MOD_QUEUE_URL/MOD_ACTION_URL/MOD_SECRET not configured")
        return

    polls_since_heartbeat = 0
    HEARTBEAT_EVERY = max(1, 600 // max(1, MOD_POLL_INTERVAL))  # ~every 10 min

    while True:
        try:
            resp = requests.post(
                MOD_QUEUE_URL,
                data={"secret": MOD_SECRET},
                headers={"User-Agent": "P.A.R./1.0"},
                timeout=15,
            )
            if resp.status_code == 200:
                data = resp.json()
                items = data.get("items", []) if isinstance(data, dict) else []
                if items:
                    log.info(f"[mod] {len(items)} item(s) to review")
                    polls_since_heartbeat = 0
                for it in items:
                    try:
                        process_mod_item(it)
                    except Exception as e:
                        log.error(f"[mod] process_mod_item crashed: {e}")
            elif resp.status_code == 204:
                log.debug("[mod] queue empty")
            else:
                log.warning(f"[mod] Poll returned {resp.status_code}: {resp.text[:200]}")
        except Exception as e:
            log.warning(f"[mod] Poll error: {e}")

        polls_since_heartbeat += 1
        if polls_since_heartbeat >= HEARTBEAT_EVERY:
            log.info(f"[mod] alive — {polls_since_heartbeat} polls since last heartbeat (queue idle)")
            polls_since_heartbeat = 0

        time.sleep(MOD_POLL_INTERVAL)


# ── RECORDING ORCHESTRATOR ─────────────────────────────────────────────────────
#
# Single-thread state machine: poll stream-start.php for a (gallery_id, name)
# start signal, start an AVFRecorder (native AVCaptureSession) recording the
# local USB webcam -> /tmp/recordings/<id>_<ts>.mov, wait for the snapshot-stop
# signal (or the cap), stop the recorder (finalizes the moov), then hand the
# file off to a background uploader so the next print can start recording
# immediately even if the upload is slow.


def _post_video_id(gallery_id: int, video_id: str) -> bool:
    """Attach the uploaded video to its gallery entry. True on success.

    THE RETURN VALUE IS LOAD-BEARING: the caller keeps the .mov unless this
    succeeds. This used to be fire-and-forget with no retry while the caller
    removed the recording unconditionally afterwards — so a single 500 or
    timeout left the video public on YouTube, the gallery entry with no
    video_id, and no local file for backfill to retry from. Silent, permanent,
    one log line.

    4xx is permanent (bad id, malformed video_id, entry genuinely absent) and is
    not retried. Transport errors and 408/429/5xx are.
    """
    if not STREAM_VIDEO_ID_URL:
        log.warning("[stream-start] STREAM_VIDEO_ID_URL not set; skipping video_id POST")
        return False

    for attempt in range(1, VIDEO_ID_MAX_ATTEMPTS + 1):
        try:
            resp = requests.post(
                STREAM_VIDEO_ID_URL,
                data={"secret": SNAPSHOT_SECRET, "id": str(gallery_id), "video_id": video_id},
                headers={"User-Agent": "P.A.R./1.0"},
                timeout=15,
            )
            if resp.status_code == 200:
                log.info(f"[stream-start] Attached video_id={video_id} to gallery #{gallery_id}")
                return True
            log.warning(f"[stream-start] video_id POST attempt {attempt}/"
                        f"{VIDEO_ID_MAX_ATTEMPTS} returned {resp.status_code}: "
                        f"{resp.text[:200]}")
            if resp.status_code not in (408, 429, 500, 502, 503, 504):
                log.error(f"[stream-start] video_id POST for #{gallery_id} failed "
                          f"permanently ({resp.status_code}); not retrying")
                return False
        except Exception as e:
            log.warning(f"[stream-start] video_id POST attempt {attempt}/"
                        f"{VIDEO_ID_MAX_ATTEMPTS} failed: {e}")

        if attempt < VIDEO_ID_MAX_ATTEMPTS:
            time.sleep(VIDEO_ID_RETRY_BASE_DELAY * (2 ** (attempt - 1)))

    log.error(f"[stream-start] video_id POST for #{gallery_id} gave up after "
              f"{VIDEO_ID_MAX_ATTEMPTS} attempts (video_id={video_id})")
    return False


def _wait_for_start() -> tuple[int, str] | None:
    """Poll stream-start.php until it returns a print. Returns (id, name) or
    None on transient error (caller sleeps and retries)."""
    try:
        resp = requests.post(
            STREAM_START_URL,
            data={"secret": SNAPSHOT_SECRET},
            headers={"User-Agent": "P.A.R./1.0"},
            timeout=10,
        )
    except Exception as e:
        log.warning(f"[record] start-poll error: {e}")
        return None
    if resp.status_code == 204:
        return None
    if resp.status_code != 200:
        log.warning(f"[record] start-poll returned {resp.status_code}")
        return None
    try:
        data = resp.json()
    except Exception as e:
        log.warning(f"[record] start-poll bad json: {e}")
        return None
    if not data.get("ok"):
        return None
    gid = data.get("id")
    name = data.get("name", "") or ""
    if not isinstance(gid, int):
        log.warning(f"[record] start-poll missing id: {data!r}")
        return None
    return gid, name


def _check_stop(expected_gid: int, started: float) -> bool:
    """Return True if the snapshot poller has captured the snapshot for this
    recording — the unified "print done" signal that replaces the old
    stream-end.php poll. The snapshot flag is armed twice per print (job-start by
    next.php with the gallery id, and display-done by the Arduino with id=null),
    so only honor a signal that landed at least MIN_RECORD_SECONDS into the
    recording: the job-start arm fires within seconds of (or before) the
    recording start and is filtered out, while the post-display snapshot lands
    minutes in. `started` is this recording's time.monotonic() start stamp."""
    with _snapshot_stop_lock:
        if _snapshot_stop_id != expected_gid:
            return False
        if (_snapshot_stop_ts - started) <= MIN_RECORD_SECONDS:
            return False
    return True


def _check_gallery_completed(expected_gid: int) -> bool:
    """Independent recording-stop trigger: return True if the gallery entry for
    this recording has finalized server-side (display done). This path does NOT
    depend on the snapshot flag or the snapshot poller — it asks gallery.php
    directly — so it stops the recording even when the Arduino's
    snapshot-request.php (the flag arm) is lost but complete.php still finalizes
    the entry (the failure mode that let a recording run to the cap).

    An entry reads pending=true from next.php pop until display-done, when
    snapshot-request.php / complete.php rename pending.json -> info.json (or a
    photo lands); gallery.php derives pending=false at that point. So a
    pending=false read is a genuine "print done" signal that can't fire early.
    Fails closed (returns False) on any error/misconfig so a flaky read can never
    cut a recording short — the snapshot signal and the cap remain in force."""
    if not GALLERY_URL:
        return False
    try:
        resp = requests.get(
            GALLERY_URL,
            headers={"User-Agent": "P.A.R./1.0"},
            timeout=10,
        )
        if resp.status_code != 200:
            return False
        data = resp.json()
    except Exception as e:
        log.debug(f"[record] gallery completion poll error: {e}")
        return False
    items = data if isinstance(data, list) else data.get("items", [])
    for it in items:
        try:
            if int(it.get("id")) == expected_gid:
                return it.get("pending") is False
        except (TypeError, ValueError):
            continue
    return False


# ── TIMELAPSE INTRO COMPOSITION ───────────────────────────────────────────────
#
# Turns a finished recording into <the whole thing at TIMELAPSE_SPEED> + <the whole
# recording at 1x>, with a bottom-right label naming the speed of each half.
#
# THREE PASSES, not one filter graph. Each segment is encoded on its own, then
# the two are joined by the concat DEMUXER with `-c copy` (a remux — seconds):
#
#   A  timelapse segment  (setpts + label)  -> seg_tl.mp4
#   B  real-time segment  (label)           -> seg_rt.mp4
#   C  concat -c copy A+B                   -> <stem>_tl.mp4
#
# The one-graph version (`-i src -i src` + the concat FILTER) works, but every
# input shares one set of decoder flags, so the timelapse branch is forced to
# fully decode all ~81k frames of a 45-min print just to keep ~650 of them.
# Splitting the passes lets A decode KEYFRAMES ONLY (see _keyframe_interval),
# which is where nearly all the savings are. It also keeps each process to a
# single input, so there is no cross-branch frame buffering to rely on.
#
# A and B must be encoded identically (same encoder, bitrate, size, pixfmt, SAR,
# frame rate) or the `-c copy` concat in C is invalid — that is why both carry
# `fps=`, `format=yuv420p` and `setsar=1`.


def _probe_video(path: Path) -> tuple[float, int, int, float] | None:
    """(duration_s, width, height, fps) for a video file, or None if ffprobe
    can't read it. fps falls back to CAMERA_FRAMERATE."""
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=width,height,r_frame_rate:format=duration",
             "-of", "json", str(path)],
            capture_output=True, timeout=60, check=True,
        ).stdout
        meta = json.loads(out)
        st = meta["streams"][0]
        dur = float(meta["format"]["duration"])
        w, h = int(st["width"]), int(st["height"])
        num, _, den = st.get("r_frame_rate", "").partition("/")
        try:
            fps = float(num) / float(den)
        except (ValueError, ZeroDivisionError):
            fps = float(CAMERA_FRAMERATE)
        if not 1.0 <= fps <= 240.0:
            fps = float(CAMERA_FRAMERATE)
        if dur <= 0 or w <= 0 or h <= 0:
            return None
        return dur, w, h, fps
    except Exception as e:
        log.warning(f"[timelapse] ffprobe failed on {path.name}: {e!r}")
        return None


def _timelapse_font(px: int):
    for cand in TIMELAPSE_FONT_CANDIDATES:
        if os.path.exists(cand):
            try:
                return ImageFont.truetype(cand, px)
            except Exception:
                continue
    log.warning("[timelapse] no TrueType font found; falling back to PIL default "
                "(the label will be tiny)")
    return ImageFont.load_default()


def _render_label_png(text: str, frame_h: int, out_path: Path) -> None:
    """Render `text` to a transparent PNG sized to the glyphs plus a little
    padding. ffmpeg composites it with `overlay`; see the TIMELAPSE_ENABLED
    comment for why this isn't drawtext."""
    px = max(12, int(round(frame_h / TIMELAPSE_FONT_DIV)))
    font = _timelapse_font(px)
    probe = ImageDraw.Draw(Image.new("RGBA", (1, 1)))
    l, t, r, b = probe.textbbox((0, 0), text, font=font)
    pad = max(2, px // 5)
    img = Image.new("RGBA", (r - l + 2 * pad, b - t + 2 * pad), (0, 0, 0, 0))
    ImageDraw.Draw(img).text((pad - l, pad - t), text, font=font,
                             fill=TIMELAPSE_TEXT_RGB + (255,))
    img.save(out_path)


def _keyframe_interval(src: Path, sample_s: float = 120.0) -> float | None:
    """Mean seconds between keyframes over the first `sample_s` of the file, or
    None if it can't be determined. `-read_intervals` stops the scan early, so
    this costs a fraction of a second even on a multi-GB recording.

    This gates keyframe-only decoding of the timelapse pass: recordings come from
    AVCaptureMovieFileOutput, whose GOP we don't control, so the density has to be
    MEASURED rather than assumed. Too-sparse keyframes would make the intro repeat
    frames instead of stepping smoothly through the print."""
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0", "-skip_frame", "nokey",
             "-read_intervals", f"%+{sample_s:.0f}",
             "-show_entries", "frame=pts_time", "-of", "csv=p=0", str(src)],
            capture_output=True, timeout=120, check=True,
        ).stdout.decode("utf-8", "replace")
        ts = []
        for tok in out.split():
            try:
                ts.append(float(tok.strip().rstrip(",")))
            except ValueError:
                continue
        if len(ts) < 2:
            return None
        return (ts[-1] - ts[0]) / (len(ts) - 1)
    except Exception as e:
        log.debug(f"[timelapse] keyframe probe failed on {src.name}: {e!r}")
        return None


def _encoder_args(encoder: str) -> list[str]:
    args = ["-c:v", encoder, "-b:v", TIMELAPSE_BITRATE]
    if encoder == "h264_videotoolbox":
        args += ["-allow_sw", "1", "-realtime", "0", "-profile:v", "high"]
    else:
        args += ["-preset", "veryfast", "-pix_fmt", "yuv420p"]
    return args + ["-tag:v", "avc1"]


def _segment_cmd(src: Path, label_png: Path, dst: Path, fps: float, margin: int,
                 encoder: str, speed: float | None, keyframes_only: bool) -> list[str]:
    """One encoded segment: the whole source, sped up by `speed` (None = 1x),
    with `label_png` composited bottom-right."""
    pos = f"x=W-w-{margin}:y=H-h-{margin}"
    head = f"[0:v]setpts=PTS/{speed:.6f},fps={fps:.6f}[s];" if speed else f"[0:v]fps={fps:.6f}[s];"
    fc = head + f"[s][1:v]overlay={pos}:eof_action=repeat,format=yuv420p,setsar=1[v]"
    cmd = ["ffmpeg", "-hide_banner", "-nostdin", "-y", "-loglevel", "warning"]
    if keyframes_only:
        # Decoder flag — must precede the input it applies to.
        cmd += ["-skip_frame", "nokey"]
    cmd += ["-i", str(src), "-i", str(label_png),
            "-filter_complex", fc,
            "-map", "[v]", "-an"]        # video-only, always — never broadcast audio
    return cmd + _encoder_args(encoder) + [str(dst)]


def _run_ffmpeg(cmd: list[str], what: str) -> bool:
    t0 = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=TIMELAPSE_TIMEOUT)
    except subprocess.TimeoutExpired:
        log.error(f"[timelapse] {what} timed out after {TIMELAPSE_TIMEOUT:.0f}s")
        return False
    if proc.returncode != 0:
        tail = proc.stderr.decode("utf-8", "replace").strip()[-500:]
        log.error(f"[timelapse] {what} rc={proc.returncode}: {tail}")
        return False
    log.info(f"[timelapse] {what} ok in {time.monotonic() - t0:.0f}s")
    return True


def compose_with_timelapse(src: Path) -> Path | None:
    """Build "<timelapse intro> + <full recording>" next to `src` and return the
    new path, or None to upload `src` unchanged (disabled, too short, or the
    compose failed). Never raises and never touches `src` — a failure here must
    cost the label, not the recording."""
    if not TIMELAPSE_ENABLED:
        return None
    info = _probe_video(src)
    if not info:
        return None
    dur, w, h, fps = info
    mult = round(TIMELAPSE_SPEED, 1)
    intro = dur / mult
    if mult <= 1.0 or intro < TIMELAPSE_MIN_INTRO_S:
        log.info(f"[timelapse] {src.name} is only {dur:.0f}s — a {intro:.1f}s intro "
                 f"at {mult:g}x; uploading as-is")
        return None

    # Trailing ".0" is noise on the common whole-number rate, but a fractional
    # TIMELAPSE_SPEED override still shows its tenth.
    tl_text = f"Timelapse ({mult:g}x speed)"
    rt_text = "Real-time (1x speed)"
    dst = src.with_name(f"{src.stem}_tl.mp4")
    margin = max(8, int(round(h / TIMELAPSE_MARGIN_DIV)))

    # The intro samples one source frame every `mult/fps` seconds. Keyframe-only
    # decoding can supply that ONLY if keyframes are at least that dense; with a
    # 2x margin it degrades to full decode rather than a stuttering intro.
    need_every = mult / fps
    kf = _keyframe_interval(src) if TIMELAPSE_KEYFRAME_DECODE else None
    keyframes_only = kf is not None and kf <= need_every / 2.0
    if TIMELAPSE_KEYFRAME_DECODE:
        log.info(f"[timelapse] keyframes every {kf if kf is None else round(kf, 2)}s, "
                 f"intro needs one frame per {need_every:.2f}s -> "
                 f"{'keyframe-only' if keyframes_only else 'full'} decode")

    tmpdir = Path(tempfile.mkdtemp(prefix="par_tl_"))
    try:
        tl_png, rt_png = tmpdir / "tl.png", tmpdir / "rt.png"
        _render_label_png(tl_text, h, tl_png)
        _render_label_png(rt_text, h, rt_png)
        seg_tl, seg_rt = tmpdir / "seg_tl.mp4", tmpdir / "seg_rt.mp4"

        encoders = [TIMELAPSE_ENCODER]
        if TIMELAPSE_ENCODER != "libx264":
            encoders.append("libx264")   # VideoToolbox can be unavailable/busy
        for enc in encoders:
            log.info(f"[timelapse] {src.name}: {dur:.0f}s source -> "
                     f"{intro:.1f}s intro at {mult:g}x [{enc}]")
            t0 = time.monotonic()
            ok = _run_ffmpeg(
                _segment_cmd(src, tl_png, seg_tl, fps, margin, enc, mult, keyframes_only),
                f"pass A (timelapse, {intro:.1f}s)")
            if ok:
                ok = _run_ffmpeg(
                    _segment_cmd(src, rt_png, seg_rt, fps, margin, enc, None, False),
                    f"pass B (real-time, {dur:.0f}s)")
            if ok:
                # The concat demuxer needs a list file; both segments came out of
                # the same encoder with the same settings, so -c copy is valid.
                listing = tmpdir / "segments.txt"
                listing.write_text("".join(
                    f"file '{seg}'\n" for seg in (seg_tl, seg_rt)))
                ok = _run_ffmpeg(
                    ["ffmpeg", "-hide_banner", "-nostdin", "-y", "-loglevel", "warning",
                     "-f", "concat", "-safe", "0", "-i", str(listing),
                     "-c", "copy", "-movflags", "+faststart", str(dst)],
                    "pass C (concat)")
            if ok and dst.exists() and dst.stat().st_size > 0:
                log.info(f"[timelapse] composed {dst.name} ({dst.stat().st_size} bytes) "
                         f"in {time.monotonic() - t0:.0f}s total")
                return dst
            dst.unlink(missing_ok=True)
            seg_tl.unlink(missing_ok=True)
            seg_rt.unlink(missing_ok=True)
        log.error(f"[timelapse] giving up on {src.name}; uploading it unmodified")
        return None
    except Exception as e:
        log.error(f"[timelapse] compose raised: {e!r}")
        dst.unlink(missing_ok=True)
        return None
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def _drop_composed(composed: Path | None) -> None:
    """Remove the transient timelapse composite. The raw .mov is the recovery
    artifact backfill knows how to find, so the composite is never what we keep."""
    if composed is None:
        return
    try:
        composed.unlink(missing_ok=True)
    except Exception as e:
        log.warning(f"[timelapse] could not remove {composed}: {e}")


def _upload_and_attach(youtube, out_path: Path, gallery_id: int, name: str) -> None:
    """Background uploader. Builds the title (no timestamp), uploads, posts
    the resulting video_id back to the gallery, and unlinks on success."""
    if not out_path.exists() or out_path.stat().st_size == 0:
        log.error(f"[upload] {out_path} missing or empty; skipping")
        return
    clean = (name or "").strip() or "Untitled"
    title = f'"{clean}" printing - P.A.R.'
    # Prepend the timelapse intro. What gets uploaded is the composed file when
    # compose succeeds and the raw .mov otherwise — compose failure costs the
    # intro, never the print. `out_path` stays untouched throughout, so it
    # remains the recovery artifact for ./run_backfill.sh in every failure path;
    # the composed file is transient and is dropped on every exit below.
    composed = compose_with_timelapse(out_path)
    upload_path = composed or out_path
    log.info(f"[upload] starting: {title!r} ({upload_path.stat().st_size} bytes)")
    video_id = upload_recording(youtube, upload_path, title)
    if not video_id:
        _drop_composed(composed)
        log.warning(f"[upload] failed for #{gallery_id}; leaving {out_path} on disk")
        return
    _drop_composed(composed)
    try:
        attached = _post_video_id(gallery_id, video_id)
    except Exception as e:
        log.error(f"[upload] video_id POST raised: {e!r}")
        attached = False
    # Best-effort, and deliberately AFTER the gallery attachment: the site
    # embed is what the print is for, the playlist is a nicety. `youtube` is
    # passed only as the single-channel fallback (see _get_playlist_client) —
    # the playlist normally has its own token for its own channel.
    attach_to_playlist(video_id, gallery_id, upload_client=youtube)

    if not attached:
        # Keep the recording so the print stays recoverable, and drop a sidecar
        # naming the video we already uploaded. Without it a later backfill
        # would re-upload the same footage as a duplicate YouTube video; with
        # it, backfill skips straight to re-POSTing the id.
        try:
            out_path.with_suffix(out_path.suffix + ".video_id").write_text(video_id)
        except Exception as e:
            log.warning(f"[upload] could not write video_id sidecar: {e}")
        log.error(f"[upload] #{gallery_id} uploaded as {video_id} but NOT attached; "
                  f"keeping {out_path} for ./run_backfill.sh")
        return

    try:
        out_path.unlink()
    except Exception as e:
        log.warning(f"[upload] could not remove {out_path}: {e}")


def record_orchestrator() -> None:
    """Main recording loop — see module docstring. Owns YouTube auth (lazy,
    on first start signal) and the single ffmpeg child process."""
    log.info(f"[record] Orchestrator started (interval={STREAM_POLL_INTERVAL}s)")
    if not STREAM_START_URL:
        log.warning("[record] Disabled — STREAM_START_URL not set")
        return
    # Recordings stop when the snapshot poller captures the print's snapshot (the
    # unified "print done" signal); the RECORD_MAX_SECONDS cap is the backstop. The old
    # stream-end.php poll is retired, so STREAM_END_URL is no longer used here.

    RECORDING_DIR.mkdir(parents=True, exist_ok=True)
    youtube = None

    while True:
        sig = _wait_for_start()
        if sig is None:
            time.sleep(STREAM_POLL_INTERVAL)
            continue
        gallery_id, name = sig
        log.info(f"[record] Start signal: id={gallery_id} name={name!r}")

        # We've already popped the start signal, so this print is ours. The
        # webcam can be transiently absent (just plugged in / enumerating, USB
        # hiccup) right when the print begins; keep retrying for up to 10 min so
        # a camera that comes up mid-window still gets recorded. Only give up
        # (skipping this print) if it never appears within the window.
        cam_deadline = time.monotonic() + CAMERA_WAIT_SECONDS
        camera_ready = False
        while True:
            if verify_camera_accessible():
                camera_ready = True
                break
            if time.monotonic() >= cam_deadline:
                break
            log.warning(f"[record] Camera {CAMERA_NAME!r} not found for "
                        f"#{gallery_id}; retrying in {CAMERA_RETRY_DELAY}s "
                        f"({cam_deadline - time.monotonic():.0f}s left)")
            time.sleep(CAMERA_RETRY_DELAY)
        if not camera_ready:
            log.warning(f"[record] Camera never came up within "
                        f"{CAMERA_WAIT_SECONDS}s; skipping #{gallery_id}")
            continue

        # verify_camera_accessible just confirmed the cam enumerates; the
        # AVFRecorder re-selects it by name itself. (Tiny race if it vanished in
        # between — AVFRecorder.start() raises and we skip this print.)
        # A previous upload found the refresh token dead, which permanently
        # poisons the cached client. Drop it so we re-read the vault below — that
        # picks up a token freshly written by backfill_uploads.py without needing
        # a daemon restart.
        if _YT_AUTH_DEAD.is_set():
            log.warning("[record] discarding cached YouTube client (auth was "
                        "reported dead); re-reading the vault")
            youtube = None
            _YT_AUTH_DEAD.clear()

        if youtube is None:
            try:
                # interactive=False: this is a background thread, and the consent
                # flow would block it forever — which would stop recording, not
                # just uploading. See get_youtube_service's docstring.
                youtube = get_youtube_service(interactive=False)
            except Exception as e:
                log.error(f"[youtube] auth failed: {e!r}")
            if not youtube:
                log.error("[record] No YouTube client; recording but cannot upload")

        out_path = RECORDING_DIR / f"{gallery_id}_{int(time.time())}.mov"

        rec = AVFRecorder(out_path, CAMERA_NAME, LATEST_FRAME_PATH)
        try:
            rec.start()
        except Exception as e:
            log.error(f"[record] failed to start AVFoundation capture for "
                      f"#{gallery_id}: {e!r}")
            time.sleep(STREAM_POLL_INTERVAL)
            continue

        log.info(f"[record] recording #{gallery_id} -> {out_path.name} "
                 f"(device={rec.device_name!r}, format={rec.chosen_subtype}, "
                 f"{CAMERA_REC_W}x{CAMERA_REC_H}@{CAMERA_FRAMERATE})")

        # Frame-liveness gate: a session can start cleanly yet stream nothing
        # (camera unauthorized in this launch context, device grabbed by another
        # opener, etc.). If no frames land within the window, abort now — otherwise
        # the loop below would "record" an empty file until the cap and the
        # upload would silently skip it (the exact multi-print outage this guards).
        if not rec.wait_until_streaming():
            log.error(f"[record] #{gallery_id}: NO frames within "
                      f"{FRAME_LIVENESS_TIMEOUT:.0f}s (camera auth={rec.auth_status} "
                      f"[{_CAMERA_AUTH_NAMES.get(rec.auth_status, '?')}], "
                      f"err={rec.error}) — aborting, nothing recorded for #{gallery_id}")
            try:
                rec.stop(timeout=5.0)
            except Exception as e:
                log.warning(f"[record] abort stop error: {e!r}")
            try:
                out_path.unlink(missing_ok=True)
            except Exception:
                pass
            time.sleep(STREAM_POLL_INTERVAL)
            continue

        _recording_active.set()  # pause the camera keeper while we record
        started = time.monotonic()
        with _inflight_lock:
            global _inflight_rec, _inflight_path, _inflight_id, _inflight_started
            _inflight_rec = rec
            _inflight_path = out_path
            _inflight_id = gallery_id
            _inflight_started = started
        deadline = started + RECORD_MAX_SECONDS
        stop_reason = "unknown"
        # Independent completion poll (see _check_gallery_completed): throttled to
        # its own slower cadence, and gated by MIN_RECORD_SECONDS like the
        # snapshot signal so a stale finalized state can't stop a fresh recording.
        next_gallery_poll = started + max(MIN_RECORD_SECONDS,
                                          GALLERY_COMPLETE_POLL_INTERVAL)

        try:
            while True:
                if not rec.is_running():
                    stop_reason = f"capture-ended(err={rec.error})"
                    break
                if time.monotonic() >= deadline:
                    stop_reason = "cap"
                    break
                if _check_stop(gallery_id, started):
                    stop_reason = "snapshot"
                    break
                now = time.monotonic()
                if now >= next_gallery_poll:
                    next_gallery_poll = now + GALLERY_COMPLETE_POLL_INTERVAL
                    if _check_gallery_completed(gallery_id):
                        stop_reason = "gallery-complete"
                        break
                time.sleep(STREAM_POLL_INTERVAL)

            log.info(f"[record] stopping ({stop_reason}) after "
                     f"{time.monotonic()-started:.0f}s")
            rec.stop()
        finally:
            _recording_active.clear()  # resume the camera keeper
            with _inflight_lock:
                _inflight_rec = None
                _inflight_path = None
                _inflight_id = None
                _inflight_started = None
            # Clear the snapshot-stop signal so a stale one can't immediately
            # stop the next recording.
            with _snapshot_stop_lock:
                global _snapshot_stop_id
                _snapshot_stop_id = None
            # Drop the live-frame sidecar so the next print's early snapshots
            # can't pick up a stale frame from this recording.
            try:
                LATEST_FRAME_PATH.unlink(missing_ok=True)
            except Exception as e:
                log.warning(f"[record] could not remove {LATEST_FRAME_PATH}: {e!r}")

        if youtube:
            threading.Thread(
                target=_upload_and_attach,
                args=(youtube, out_path, gallery_id, name),
                daemon=True,
                name=f"upload-{gallery_id}",
            ).start()
        else:
            log.warning(f"[record] Skipping upload of {out_path.name} — no YT client")


# ── MAIN ───────────────────────────────────────────────────────────────────────

def _safe_run(name, fn, *args):
    try:
        fn(*args)
    except Exception as e:
        log.error(f"[{name}] crashed: {e!r} — subsystem disabled, others continue")


def record_watchdog() -> None:
    """Last-resort wall-clock backstop for a runaway recording.

    The orchestrator's RECORD_MAX_SECONDS cap is checked inside the record loop,
    which shares its thread with the blocking native stop() call. If stop() ever
    wedges (it did: an AVFoundation graph-teardown deadlock recorded for 13h), the
    loop is frozen and the cap can never fire while the capture graph keeps writing
    — a multi-GB runaway file. This watchdog runs on its own thread, owns no AVF
    objects, and simply force-exits the whole process if any single recording
    outlives RECORD_MAX_SECONDS + RECORD_WATCHDOG_GRACE. Exiting kills the runaway
    capture immediately; the operator (or a launchd KeepAlive) restarts the daemon.
    A hard os._exit avoids running atexit/finalizers that could themselves block on
    the same wedged AVF state."""
    limit = RECORD_MAX_SECONDS + RECORD_WATCHDOG_GRACE
    while True:
        time.sleep(30)
        with _inflight_lock:
            started = _inflight_started
            gid = _inflight_id
        if started is None:
            continue
        elapsed = time.monotonic() - started
        if elapsed > limit:
            log.critical(
                f"[watchdog] recording #{gid} has run {elapsed:.0f}s "
                f"(> cap {RECORD_MAX_SECONDS}s + grace {RECORD_WATCHDOG_GRACE}s) — "
                f"stop() is wedged; force-exiting to kill the runaway capture")
            os._exit(1)


if __name__ == "__main__":
    log.info("═" * 50)
    log.info("  P.A.R. Recorder + Snapshot/Mod Pollers")
    log.info("═" * 50)
    log.info(f"  Source : AVFoundation webcam, name prefix {CAMERA_NAME!r}")
    log.info(f"  Target : YouTube Data API (videos.insert)")
    log.info(f"  Video  : {CAMERA_REC_W}x{CAMERA_REC_H}@{CAMERA_FRAMERATE} H.264 (AVCaptureSession), "
             f"video-only (no audio), cap {RECORD_MAX_SECONDS}s")
    log.info(f"  Poller : {SNAPSHOT_REQUEST_URL}")
    if _AVF_IMPORT_ERROR is not None:
        log.error(f"  WARNING: PyObjC AVFoundation unavailable ({_AVF_IMPORT_ERROR!r}); "
                  f"recording will fail until pyobjc is installed")
    else:
        # Camera-TCC status THIS process sees at boot. Authorized(3) → recordings
        # will stream; anything else → the capture graph starts but delivers no
        # frames (a daemon can't show the consent prompt), so every recording is
        # silently empty. Logged here so the state is visible without waiting for
        # the first print. See the frame-liveness gate in record_orchestrator.
        _boot_auth = _AVF.AVCaptureDevice.authorizationStatusForMediaType_(
            _AVF.AVMediaTypeVideo)
        _boot_auth_name = _CAMERA_AUTH_NAMES.get(_boot_auth, "?")
        if _boot_auth == 3:
            log.info(f"  Camera : TCC authorization = {_boot_auth} ({_boot_auth_name})")
        else:
            log.error(f"  Camera : TCC authorization = {_boot_auth} ({_boot_auth_name}) "
                      f"— recordings will stream NO frames until Camera access is "
                      f"granted to this daemon's launch context")
            # NotDetermined: proactively raise the system consent prompt now, so a
            # GUI login session (physical screen OR Screen Sharing into a headless,
            # auto-logged-in mini) shows the dialog immediately — no need to wait
            # for a print. The decision is recorded system-wide once clicked, so the
            # completion handler is best-effort; the main CFRunLoop services it. Only
            # NotDetermined(0) can prompt — Denied/Restricted never re-prompt.
            if _boot_auth == 0:
                def _cam_grant(granted):
                    log.info(f"  Camera : consent dialog answered — granted={bool(granted)}")
                _AVF.AVCaptureDevice.requestAccessForMediaType_completionHandler_(
                    _AVF.AVMediaTypeVideo, _cam_grant)
                log.info("  Camera : consent prompt requested — click Allow in the GUI "
                         "session (Screen Sharing works headless) to authorize permanently")
    log.info("═" * 50)

    # Start mod poller FIRST so it's running regardless of camera/YouTube state.
    threading.Thread(
        target=_safe_run, args=("mod", poll_mod_queue),
        daemon=True, name="mod-poller",
    ).start()

    # Snapshot poller — independent of camera reachability.
    threading.Thread(
        target=_safe_run, args=("snapshot", poll_snapshot_queue),
        daemon=True, name="snapshot-poller",
    ).start()

    # Camera keeper — knocks on the RTSP path every CAMERA_RETRY_DELAY seconds
    # whenever a recording isn't in progress, so the path never goes stale.
    threading.Thread(
        target=_safe_run, args=("camera", camera_keeper),
        daemon=True, name="camera-keeper",
    ).start()

    # YouTube token keepalive — refreshes the cached OAuth token on a timer
    # even with no start signals, so a dead refresh token is caught early in the
    # log rather than wedging the next recording. Refresh-only, never interactive.
    threading.Thread(
        target=_safe_run, args=("youtube-refresh", poll_token_refresh),
        daemon=True, name="youtube-refresh",
    ).start()

    # Recording orchestrator — listens for stream-start.php hits, records,
    # uploads, attaches video_id to gallery. Lazily authenticates YouTube
    # on first hit.
    threading.Thread(
        target=_safe_run, args=("record", record_orchestrator),
        daemon=True, name="record",
    ).start()

    # Watchdog — force-exits if a recording ever outlives the hard ceiling, in
    # case stop() wedges (see record_watchdog).
    threading.Thread(
        target=_safe_run, args=("watchdog", record_watchdog),
        daemon=True, name="watchdog",
    ).start()

    def _shutdown_inflight():
        # Clean up any in-flight recording: stop the capture, delete the partial
        # mov. Runs on the main thread, where stop()'s graph-teardown perform
        # executes inline. The orchestrator thread is a daemon killed at
        # interpreter shutdown, so cleanup must happen here.
        with _inflight_lock:
            rec = _inflight_rec
            path = _inflight_path
        if rec is not None:
            log.info("[record] shutdown — stopping in-flight recording")
            try:
                rec.stop()
            except Exception as e:
                log.warning(f"[record] error stopping recording on shutdown: {e!r}")
        if path is not None and path.exists():
            try:
                path.unlink()
                log.info(f"[record] removed in-flight {path.name}")
            except Exception as e:
                log.warning(f"[record] could not unlink {path}: {e!r}")

    # The main thread MUST service a CoreFoundation run loop: AVCaptureSession
    # tears its graph down (stopRecording / stopRunning) by performing
    # `graphWillStop` on the MAIN thread with waitUntilDone:YES. With the main
    # thread parked in time.sleep() that perform is never serviced and the stop
    # call from the orchestrator (worker) thread deadlocks forever while the
    # capture keeps writing — the root cause of the 13-hour runaway recording.
    # Servicing the run loop lets those performs execute. When PyObjC is
    # unavailable, recording is disabled (no session is ever created), so a plain
    # idle loop is sufficient.
    #
    # Shutdown: a CFRunLoop does NOT raise KeyboardInterrupt the way time.sleep
    # does, so register explicit SIGINT/SIGTERM handlers that set a stop flag and
    # kick the run loop. Short (0.25s) slices bound how long the loop can sit
    # between flag checks, so shutdown is prompt even if CFRunLoopStop is missed.
    _stop_evt = threading.Event()

    def _on_signal(signum, _frame):
        _stop_evt.set()
        if _AVF_IMPORT_ERROR is None:
            try:
                import CoreFoundation as _CF
                _CF.CFRunLoopStop(_CF.CFRunLoopGetMain())
            except Exception:
                pass

    signal.signal(signal.SIGINT, _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)

    try:
        if _AVF_IMPORT_ERROR is None:
            import CoreFoundation
            while not _stop_evt.is_set():
                CoreFoundation.CFRunLoopRunInMode(
                    CoreFoundation.kCFRunLoopDefaultMode, 0.25, False)
        else:
            while not _stop_evt.is_set():
                time.sleep(0.5)
    finally:
        log.info("Shutting down.")
        _shutdown_inflight()
        sys.exit(0)
