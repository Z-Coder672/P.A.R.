#!/usr/bin/env python3
"""
Per-print Recorder + Snapshot/Moderation Pollers.

Records a locally-attached USB webcam (a Logitech "Brio 100", matched by name
prefix) to a local mp4 while a print is active via ffmpeg's avfoundation input,
then uploads to YouTube via the Data API and attaches the resulting video id to
the gallery entry.

A recording starts when stream-start.php hands us a (gallery_id, name) and
stops when either stream-end.php fires (Arduino's signal after its 10-min
post-display linger) or a 1.5h hard cap elapses.

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
import requests
import ftplib
import ssl
import os
import smtplib
import base64
import io
import concurrent.futures
import anyio
from PIL import Image
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
from googleapiclient.discovery import build
from googleapiclient.errors import HttpError
from googleapiclient.http import MediaFileUpload

load_dotenv(Path(__file__).parent / ".env")

# ── CONFIG ─────────────────────────────────────────────────────────────────────
# Locally-attached USB webcam, accessed through ffmpeg's avfoundation input.
# CAMERA_NAME is matched case-insensitively against the START of each
# avfoundation VIDEO-device name, so "brio 100" matches "Brio 100", "Brio 100
# (1234)", etc. Recording is ALWAYS video-only — audio is never captured or
# broadcast (build_record_cmd hard-codes -an and the input is video-only).
CAMERA_NAME            = os.getenv("CAMERA_NAME", "Brio 100")
CAMERA_FRAMERATE       = os.getenv("CAMERA_FRAMERATE", "30")
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

# Local working dir for in-flight mp4 recordings. Uploads delete on success;
# failed uploads are left here for manual recovery.
RECORDING_DIR          = Path("/tmp/recordings")
# While a recording is in flight, the recording ffmpeg also writes a ~1fps JPEG
# here (a second output of the same capture). The snapshot poller copies THIS
# instead of opening the camera, because a USB webcam allows only one opener at
# a time and the recording owns it.
LATEST_FRAME_PATH      = RECORDING_DIR / "latest_frame.jpg"
# 1.5h hard cap on a single recording (safety net if the stop signal is lost).
RECORD_MAX_SECONDS     = 90 * 60

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

VIDEO_BITRATE        = "2500k"
CAMERA_RETRY_DELAY   = 30   # seconds between camera-availability re-checks
# Total window to keep retrying the camera after a start signal before giving
# up on a print. A cam that comes up any time inside this window gets recorded.
CAMERA_WAIT_SECONDS  = 10 * 60

# Cached avfoundation video-device index from the last successful enumeration
# (e.g. "1"). Audio is never captured. None = not currently present. The keeper
# refreshes this every CAMERA_POLL_INTERVAL seconds.
_camera_lock = threading.Lock()
_camera_spec: str | None = None
# Set while ffmpeg is actively recording a print — pauses the camera keeper so
# it doesn't re-enumerate devices while a recording holds the cam.
_recording_active = threading.Event()
# Tracks the currently in-flight recording (ffmpeg proc + its mp4 path) so a
# Ctrl+C handler can stop ffmpeg cleanly and delete the partial file. Guarded
# by _inflight_lock because the orchestrator thread writes it and the signal
# handler (main thread) reads it.
_inflight_lock = threading.Lock()
_inflight_proc: subprocess.Popen | None = None
_inflight_path: Path | None = None
# ───────────────────────────────────────────────────────────────────────────────

logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s [%(levelname)s] %(message)s",
    handlers=[
        logging.StreamHandler(sys.stdout),
        logging.FileHandler("stream.log"),
    ]
)
log = logging.getLogger(__name__)


def build_record_cmd(out_path: Path, cam_spec: str) -> list[str]:
    """Webcam (avfoundation) -> local mp4, plus a ~1fps JPEG sidecar, from one
    capture. `cam_spec` is the avfoundation VIDEO index from _refresh_camera().

    Two outputs:
      1. the fragmented mp4 (a SIGKILL still leaves a playable file),
      2. LATEST_FRAME_PATH, overwritten ~once a second, so the snapshot poller
         can grab a still WITHOUT opening the camera (a USB cam allows only one
         opener, and the recording owns the device).

    Audio is NEVER captured: the input is video-only and -an is hard-coded.
    -t on the INPUT caps total capture at RECORD_MAX_SECONDS, stopping both
    outputs together (a safety net if the stop signal is lost)."""
    return [
        "ffmpeg",
        "-loglevel", "warning",
        "-f", "avfoundation",
        "-framerate", CAMERA_FRAMERATE,
        "-t", str(RECORD_MAX_SECONDS),
        "-i", cam_spec,
        # ── output 1: the recording ──────────────────────────────────────────
        "-map", "0:v",
        "-c:v", "libx264",
        "-preset", "veryfast",
        "-tune", "zerolatency",
        "-b:v", VIDEO_BITRATE,
        "-maxrate", VIDEO_BITRATE,
        "-bufsize", str(int(VIDEO_BITRATE[:-1]) * 2) + "k",
        "-g", "30",
        "-an",
        "-movflags", "+empty_moov+default_base_moof+frag_keyframe+faststart",
        "-f", "mp4",
        str(out_path),
        # ── output 2: the live snapshot frame ────────────────────────────────
        "-map", "0:v",
        "-r", "1",
        "-update", "1",
        "-q:v", "2",
        "-f", "image2",
        str(LATEST_FRAME_PATH),
    ]


# ── YOUTUBE API ────────────────────────────────────────────────────────────────

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
    """
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


def _vault_unmount(device: str):
    subprocess.run(["hdiutil", "detach", device, "-quiet"], capture_output=True)


def _vault_read_token() -> str | None:
    """Read yt_token.json from the vault; returns JSON string or None if absent."""
    try:
        password = _vault_get_password()
        mount_point, device = _vault_mount(password)
        try:
            token_path = Path(mount_point) / YT_VAULT_TOKEN_FILE
            return token_path.read_text() if token_path.exists() else None
        finally:
            _vault_unmount(device)
            log.info("[vault] Vault unmounted")
    except Exception as e:
        log.warning(f"[vault] Could not read token: {e}")
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


def _vault_write_token(token_json: str):
    """Write yt_token.json into the vault."""
    password = _vault_get_password()
    mount_point, device = _vault_mount(password)
    try:
        (Path(mount_point) / YT_VAULT_TOKEN_FILE).write_text(token_json)
        log.info("[vault] Token saved to vault")
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


def get_youtube_service():
    """
    Authenticate with YouTube Data API v3 via OAuth 2.0.
    Client secrets are loaded from the encrypted vault DMG on each auth flow.
    Token is cached in yt_token.json for headless restarts (no vault needed after first auth).
    Returns a service object, or None if auth fails.
    """
    creds = None
    token_json = _vault_read_token()
    if token_json:
        creds = Credentials.from_authorized_user_info(json.loads(token_json), YT_SCOPES)

    if not creds or not creds.valid:
        refreshed = False
        if creds and creds.expired and creds.refresh_token:
            log.info("[youtube] Refreshing OAuth token...")
            try:
                creds.refresh(Request())
                refreshed = True
            except Exception as e:
                log.warning(f"[youtube] Token refresh failed: {e!r} — falling back to browser OAuth")
                creds = None
        if not refreshed:
            log.info("[youtube] Starting OAuth flow...")
            try:
                client_config = _load_client_secrets()
            except Exception as e:
                log.error(f"[vault] Failed to load client secrets: {e}")
                log.warning("[youtube] Uploads disabled until vault is reachable.")
                return None
            flow = InstalledAppFlow.from_client_config(client_config, YT_SCOPES)
            try:
                creds = flow.run_local_server(port=0)
            except Exception:
                # Headless fallback: print URL, prompt for code
                creds = flow.run_console()
        _vault_write_token(creds.to_json())

    return build("youtube", "v3", credentials=creds)


def upload_recording(youtube, out_path: Path, title: str) -> str | None:
    """Resumable upload of a finished mp4 via videos.insert.
    Returns the 11-char YouTube video id, or None on failure."""
    body = {
        "snippet": {
            "title": title,
            "description": "Recorded by P.A.R. — par.zimmzimm.com",
            "categoryId": "28",  # Science & Technology
        },
        "status": {
            "privacyStatus": "public",
            "selfDeclaredMadeForKids": False,
        },
    }
    media = MediaFileUpload(str(out_path), mimetype="video/mp4",
                            resumable=True, chunksize=8 * 1024 * 1024)
    request = youtube.videos().insert(
        part="snippet,status",
        body=body,
        media_body=media,
    )

    response = None
    last_progress_log = 0.0
    while response is None:
        try:
            status, response = request.next_chunk()
            if status:
                pct = status.progress() * 100
                if pct - last_progress_log >= 10:
                    log.info(f"[upload] {out_path.name} {pct:.0f}%")
                    last_progress_log = pct
        except HttpError as e:
            log.error(f"[upload] HttpError on {out_path.name}: {e}")
            return None
        except Exception as e:
            log.error(f"[upload] Unexpected error on {out_path.name}: {e!r}")
            return None

    video_id = response.get("id") if isinstance(response, dict) else None
    if not video_id:
        log.error(f"[upload] videos.insert returned no id: {response!r}")
        return None
    log.info(f"[upload] {out_path.name} -> video_id={video_id}")
    return video_id


# ── CAMERA ─────────────────────────────────────────────────────────────────────

def _list_avfoundation_devices() -> tuple[list[tuple[str, str]], list[tuple[str, str]]]:
    """Enumerate ffmpeg's avfoundation devices. Returns (video, audio), each a
    list of (index, name) tuples in the order ffmpeg reports them.

    `ffmpeg -f avfoundation -list_devices true -i ""` writes the device list to
    stderr and exits non-zero (it treats the empty input as an error) — that's
    expected, so we ignore the return code and parse stderr. Each device line
    looks like `[AVFoundation indev @ 0x..] [1] Brio 100`; the FIRST bracketed
    integer is the indev handle, the SECOND is the device index we want."""
    result = subprocess.run(
        ["ffmpeg", "-hide_banner", "-f", "avfoundation",
         "-list_devices", "true", "-i", ""],
        capture_output=True, text=True, timeout=15,
    )
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
        log.debug(f"[camera] no video device starting with {CAMERA_NAME!r}; "
                  f"present: {[n for _, n in video]}")
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
                if _refresh_camera() is None:
                    log.warning(f"[camera] no device matching {CAMERA_NAME!r} found")
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
                   image_result, name_result) -> bool:
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
  <p>
    <img src="cid:{image_cid}"
         style="image-rendering:pixelated;width:456px;height:216px;
                border:1px solid #444;background:#000"/>
  </p>
  {_render_result('Image check', image_result)}
  {_render_result('Name check', name_result)}
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
    log.info(f"[mod] #{item_id} → checking (name={name!r}, effort={MOD_REASONING})")

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        fut_img  = pool.submit(check_image, png_for_claude)
        fut_name = pool.submit(check_name, name)

        # Outer timeout = generous bound around per-attempt timeout * (1 + retries)
        # so the threadpool .result() doesn't fire before check_image's
        # internal retry loop has had a chance to run.
        outer_budget = MOD_CHECK_TIMEOUT * (2 + MOD_CHECK_RETRIES)

        try:
            image_result = fut_img.result(timeout=outer_budget)
            log.info(f"[mod] #{item_id} image check done in "
                     f"{time.monotonic()-t0:.1f}s: "
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
            name_result = fut_name.result(timeout=outer_budget)
            log.info(f"[mod] #{item_id} name check done in "
                     f"{time.monotonic()-t0:.1f}s: "
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

    # Auto-approve only when BOTH checks come back as high-confidence approve.
    # Per user spec, anything else — including a confident reject — goes to a
    # human via email rather than being auto-rejected.
    if (isinstance(image_result, dict) and isinstance(name_result, dict)
            and _is_clear_approve(image_result) and _is_clear_approve(name_result)):
        log.info(f"[mod] auto-approve #{item_id} "
                 f"(img conf={image_result.get('confidence')}, "
                 f"name conf={name_result.get('confidence')})")
        _post_mod_action(item_id, "approve")
        return

    log.info(f"[mod] human review #{item_id} — emailing moderator")
    sent = send_mod_email(item_id, name, png_for_email, image_result, name_result)
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
# Single-thread state machine driven by the server's two recording flags:
#   - stream-pending.flag (set by next.php on each print)  -> START
#   - stream-end.flag     (set by Arduino's stream-end-set) -> STOP
#
# Per print: poll stream-start.php for a (gallery_id, name) start signal,
# spawn ffmpeg to record RTSP -> /tmp/recordings/<id>_<ts>.mp4, poll
# stream-end.php until we see the matching stop signal (or 1.5h elapses),
# graceful-stop ffmpeg, then hand the file off to a background uploader so
# the next print can start recording immediately even if the upload is slow.


def _post_video_id(gallery_id: int, video_id: str) -> None:
    if not STREAM_VIDEO_ID_URL:
        log.warning("[stream-start] STREAM_VIDEO_ID_URL not set; skipping video_id POST")
        return
    try:
        resp = requests.post(
            STREAM_VIDEO_ID_URL,
            data={"secret": SNAPSHOT_SECRET, "id": str(gallery_id), "video_id": video_id},
            headers={"User-Agent": "P.A.R./1.0"},
            timeout=15,
        )
        if resp.status_code == 200:
            log.info(f"[stream-start] Attached video_id={video_id} to gallery #{gallery_id}")
        else:
            log.warning(f"[stream-start] video_id POST returned {resp.status_code}: {resp.text[:200]}")
    except Exception as e:
        log.error(f"[stream-start] video_id POST failed: {e}")


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


def _check_stop(expected_gid: int) -> bool:
    """Poll stream-end.php once. Returns True if a stop signal arrived for
    this recording (matching id, or id=None as fire-and-forget tolerance)."""
    if not STREAM_END_URL:
        return False
    try:
        resp = requests.post(
            STREAM_END_URL,
            data={"secret": SNAPSHOT_SECRET},
            headers={"User-Agent": "P.A.R./1.0"},
            timeout=10,
        )
    except Exception as e:
        log.warning(f"[record] stop-poll error: {e}")
        return False
    if resp.status_code != 200:
        return False
    try:
        data = resp.json()
    except Exception:
        return False
    if not data.get("ok"):
        return False
    flag_id = data.get("id")
    if flag_id is None or flag_id == expected_gid:
        return True
    log.warning(f"[record] stop signal id mismatch: got {flag_id}, expected {expected_gid} — ignoring")
    return False


def _graceful_stop_ffmpeg(proc: subprocess.Popen) -> None:
    """Tell ffmpeg to flush and exit cleanly. Sends 'q' on stdin (the muxer
    finalizes the moov atom), then SIGTERM, then SIGKILL as escalating
    fallbacks."""
    if proc.poll() is not None:
        return
    try:
        if proc.stdin and not proc.stdin.closed:
            proc.stdin.write(b"q\n")
            proc.stdin.flush()
    except Exception:
        pass
    try:
        proc.wait(timeout=10)
        return
    except subprocess.TimeoutExpired:
        log.warning("[record] ffmpeg did not exit after 'q'; SIGTERM")
    proc.terminate()
    try:
        proc.wait(timeout=5)
        return
    except subprocess.TimeoutExpired:
        log.warning("[record] ffmpeg did not exit after SIGTERM; SIGKILL")
    proc.kill()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        log.error("[record] ffmpeg unkillable")


def _upload_and_attach(youtube, out_path: Path, gallery_id: int, name: str) -> None:
    """Background uploader. Builds the title (no timestamp), uploads, posts
    the resulting video_id back to the gallery, and unlinks on success."""
    if not out_path.exists() or out_path.stat().st_size == 0:
        log.error(f"[upload] {out_path} missing or empty; skipping")
        return
    clean = (name or "").strip() or "Untitled"
    title = f'"{clean}" printing - P.A.R.'
    log.info(f"[upload] starting: {title!r} ({out_path.stat().st_size} bytes)")
    video_id = upload_recording(youtube, out_path, title)
    if not video_id:
        log.warning(f"[upload] failed for #{gallery_id}; leaving {out_path} on disk")
        return
    try:
        _post_video_id(gallery_id, video_id)
    except Exception as e:
        log.error(f"[upload] video_id POST raised: {e!r}")
    try:
        out_path.unlink()
    except Exception as e:
        log.warning(f"[upload] could not unlink {out_path}: {e}")


def record_orchestrator() -> None:
    """Main recording loop — see module docstring. Owns YouTube auth (lazy,
    on first start signal) and the single ffmpeg child process."""
    log.info(f"[record] Orchestrator started (interval={STREAM_POLL_INTERVAL}s)")
    if not STREAM_START_URL:
        log.warning("[record] Disabled — STREAM_START_URL not set")
        return
    if not STREAM_END_URL:
        log.warning("[record] STREAM_END_URL not set — recordings only stop on 1.5h cap")

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

        # verify_camera_accessible just refreshed the spec; capture it for this
        # recording. (Tiny race if the cam vanished in between — guard anyway.)
        with _camera_lock:
            cam_spec = _camera_spec
        if cam_spec is None:
            log.warning(f"[record] camera vanished before recording #{gallery_id}; skipping")
            continue

        if youtube is None:
            try:
                youtube = get_youtube_service()
            except Exception as e:
                log.error(f"[youtube] auth failed: {e!r}")
            if not youtube:
                log.error("[record] No YouTube client; recording but cannot upload")

        out_path = RECORDING_DIR / f"{gallery_id}_{int(time.time())}.mp4"

        try:
            proc = subprocess.Popen(
                build_record_cmd(out_path, cam_spec),
                stdin=subprocess.PIPE,
            )
        except FileNotFoundError:
            log.error("[record] ffmpeg not found on PATH — orchestrator exiting")
            return
        except Exception as e:
            log.error(f"[record] failed to spawn ffmpeg: {e!r}")
            time.sleep(STREAM_POLL_INTERVAL)
            continue

        log.info(f"[record] ffmpeg pid={proc.pid} -> {out_path.name}")
        _recording_active.set()  # pause the camera keeper while we record
        with _inflight_lock:
            global _inflight_proc, _inflight_path
            _inflight_proc = proc
            _inflight_path = out_path
        started = time.monotonic()
        deadline = started + RECORD_MAX_SECONDS
        stop_reason = "unknown"

        try:
            while True:
                if proc.poll() is not None:
                    stop_reason = f"ffmpeg-exit({proc.returncode})"
                    break
                if time.monotonic() >= deadline:
                    stop_reason = "cap"
                    break
                if _check_stop(gallery_id):
                    stop_reason = "signal"
                    break
                time.sleep(STREAM_POLL_INTERVAL)

            log.info(f"[record] stopping ({stop_reason}) after "
                     f"{time.monotonic()-started:.0f}s")
            _graceful_stop_ffmpeg(proc)
        finally:
            _recording_active.clear()  # resume the camera keeper
            with _inflight_lock:
                _inflight_proc = None
                _inflight_path = None
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


if __name__ == "__main__":
    log.info("═" * 50)
    log.info("  P.A.R. Recorder + Snapshot/Mod Pollers")
    log.info("═" * 50)
    log.info(f"  Source : avfoundation webcam, name prefix {CAMERA_NAME!r}")
    log.info(f"  Target : YouTube Data API (videos.insert)")
    log.info(f"  Video  : {VIDEO_BITRATE} H.264 veryfast, video-only (no audio), cap {RECORD_MAX_SECONDS}s")
    log.info(f"  Poller : {SNAPSHOT_REQUEST_URL}")
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

    # Recording orchestrator — listens for stream-start.php hits, records,
    # uploads, attaches video_id to gallery. Lazily authenticates YouTube
    # on first hit.
    threading.Thread(
        target=_safe_run, args=("record", record_orchestrator),
        daemon=True, name="record",
    ).start()

    # Main thread idles so daemon threads keep running.
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        log.info("Interrupted by user. Stopping.")
        # Clean up any in-flight recording: stop ffmpeg, delete the partial
        # mp4. The orchestrator thread is a daemon and will be killed at
        # interpreter shutdown, so we have to do this here on the main thread.
        with _inflight_lock:
            proc = _inflight_proc
            path = _inflight_path
        if proc is not None:
            log.info(f"[record] Ctrl+C — stopping in-flight ffmpeg pid={proc.pid}")
            try:
                _graceful_stop_ffmpeg(proc)
            except Exception as e:
                log.warning(f"[record] error stopping ffmpeg on shutdown: {e!r}")
        if path is not None and path.exists():
            try:
                path.unlink()
                log.info(f"[record] removed in-flight {path.name}")
            except Exception as e:
                log.warning(f"[record] could not unlink {path}: {e!r}")
        sys.exit(0)
