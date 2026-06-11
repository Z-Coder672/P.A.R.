#!/usr/bin/env python3
"""
Simple test: connect to RTSP camera, record for 1 minute, upload to YouTube
as an unlisted video titled "TestUpload@<date>".

Reuses the same vault-based OAuth flow as YT_streamer.py.
"""

import subprocess
import time
import logging
import sys
import json
import os
from urllib.parse import urlparse
from datetime import datetime
from pathlib import Path
from dotenv import load_dotenv

from google.oauth2.credentials import Credentials
from google_auth_oauthlib.flow import InstalledAppFlow
from google.auth.transport.requests import Request
from googleapiclient.discovery import build
from googleapiclient.errors import HttpError
from googleapiclient.http import MediaFileUpload

load_dotenv(Path(__file__).parent / ".env")

# ── CONFIG ─────────────────────────────────────────────────────────────────────
RTSP_URL          = os.getenv("RTSP_URL")
RECORDING_DIR     = Path("/tmp/recordings")
RECORD_SECONDS    = 60
VIDEO_BITRATE     = "2500k"
AUDIO_BITRATE     = "128k"
WARMUP_RETRY_DELAY = 2    # seconds between camera warm-up attempts
CAMERA_SETTLE_DELAY = 3   # seconds to let the camera path settle before recording
CAMERA_CHECK_RETRIES = 5  # extra attempts on the initial connection check
CAMERA_RETRY_DELAY = 10   # seconds between connection-check attempts

YT_SCOPES              = ["https://www.googleapis.com/auth/youtube"]
YT_VAULT_DMG           = Path(__file__).parent / os.getenv("YT_VAULT_DMG", "YT_streamer_vault.dmg")
YT_VAULT_KEYCHAIN_KEY  = os.getenv("YT_VAULT_KEYCHAIN_KEY", "")
YT_VAULT_SECRET_FILE   = os.getenv("YT_VAULT_SECRET_FILE", "")
YT_VAULT_TOKEN_FILE    = "yt_token.json"

_missing = [k for k, v in {
    "RTSP_URL": RTSP_URL,
    "YT_VAULT_KEYCHAIN_KEY": YT_VAULT_KEYCHAIN_KEY,
    "YT_VAULT_SECRET_FILE": YT_VAULT_SECRET_FILE,
}.items() if not v]
if _missing:
    print("ERROR: Missing required environment variables:", ", ".join(_missing))
    sys.exit(1)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    handlers=[logging.StreamHandler(sys.stdout)],
)
log = logging.getLogger(__name__)


def build_record_cmd(out_path: Path) -> list[str]:
    return [
        "ffmpeg",
        "-loglevel", "warning",
        "-rtsp_transport", "tcp",
        "-fflags", "nobuffer",
        "-i", RTSP_URL,
        "-c:v", "libx264",
        "-preset", "veryfast",
        "-tune", "zerolatency",
        "-b:v", VIDEO_BITRATE,
        "-maxrate", VIDEO_BITRATE,
        "-bufsize", str(int(VIDEO_BITRATE[:-1]) * 2) + "k",
        "-g", "30",
        "-c:a", "aac",
        "-b:a", AUDIO_BITRATE,
        "-movflags", "+empty_moov+default_base_moof+frag_keyframe+faststart",
        "-t", str(RECORD_SECONDS),
        "-f", "mp4",
        str(out_path),
    ]


# ── VAULT / YOUTUBE AUTH ───────────────────────────────────────────────────────

def _vault_get_password() -> str:
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
    result = subprocess.run(
        ["hdiutil", "attach", str(YT_VAULT_DMG), "-stdinpass", "-nobrowse"],
        input=password.encode(),
        capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"hdiutil attach failed: {result.stderr.decode().strip()}")
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
    try:
        password = _vault_get_password()
        mount_point, device = _vault_mount(password)
        try:
            token_path = Path(mount_point) / YT_VAULT_TOKEN_FILE
            return token_path.read_text() if token_path.exists() else None
        finally:
            _vault_unmount(device)
    except Exception as e:
        log.warning(f"[vault] Could not read token: {e}")
        return None


def _vault_write_token(token_json: str):
    password = _vault_get_password()
    mount_point, device = _vault_mount(password)
    try:
        (Path(mount_point) / YT_VAULT_TOKEN_FILE).write_text(token_json)
        log.info("[vault] Token saved to vault")
    finally:
        _vault_unmount(device)


def _load_client_secrets() -> dict:
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


def get_youtube_service():
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
                log.warning(f"[youtube] Token refresh failed: {e!r}")
                creds = None
        if not refreshed:
            log.info("[youtube] Starting OAuth flow...")
            client_config = _load_client_secrets()
            flow = InstalledAppFlow.from_client_config(client_config, YT_SCOPES)
            try:
                creds = flow.run_local_server(port=0)
            except Exception:
                creds = flow.run_console()
        _vault_write_token(creds.to_json())

    return build("youtube", "v3", credentials=creds)


# ── CAMERA ─────────────────────────────────────────────────────────────────────

def _warm_camera_path(attempts: int = 4, per_try_timeout: float = 5.0) -> bool:
    """Knock on TCP/554 to refresh the macOS neighbor cache before ffmpeg runs.
    Shells out to `nc -z` because, empirically, this is what unsticks the path
    on this network — a Python socket probe does not."""
    parsed = urlparse(RTSP_URL or "")
    host = parsed.hostname
    port = parsed.port or 554
    if not host:
        return False
    for i in range(attempts):
        try:
            result = subprocess.run(
                ["nc", "-z", "-v", "-G", str(int(per_try_timeout)), host, str(port)],
                capture_output=True,
                timeout=per_try_timeout + 2,
            )
            if result.returncode == 0:
                return True
        except subprocess.TimeoutExpired:
            pass
        if i < attempts - 1:
            time.sleep(WARMUP_RETRY_DELAY)
    return False


def verify_camera_accessible() -> bool:
    log.info("[camera] Verifying camera is accessible...")
    _warm_camera_path()
    cmd = [
        "ffmpeg", "-y",
        "-rtsp_transport", "tcp",
        "-i", RTSP_URL,
        "-frames:v", "1",
        "-f", "null", "-",
    ]
    try:
        result = subprocess.run(cmd, capture_output=True, timeout=30)
        ok = result.returncode == 0
        if not ok:
            log.warning(f"[camera] ffmpeg probe failed: {result.stderr.decode()[-200:]}")
        return ok
    except subprocess.TimeoutExpired:
        log.warning("[camera] Probe timed out after 30s")
        return False


# ── UPLOAD ─────────────────────────────────────────────────────────────────────

def upload_recording(youtube, out_path: Path, title: str) -> str | None:
    body = {
        "snippet": {
            "title": title,
            "description": "Test upload from YT-Streamer test_upload.py",
            "categoryId": "28",
        },
        "status": {
            "privacyStatus": "unlisted",
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
            log.error(f"[upload] HttpError: {e}")
            return None
        except Exception as e:
            log.error(f"[upload] Unexpected error: {e!r}")
            return None

    video_id = response.get("id") if isinstance(response, dict) else None
    if not video_id:
        log.error(f"[upload] videos.insert returned no id: {response!r}")
        return None
    log.info(f"[upload] {out_path.name} -> video_id={video_id}")
    return video_id


# ── MAIN ───────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    log.info("═" * 50)
    log.info("  Test recorder + uploader")
    log.info("═" * 50)

    for attempt in range(1 + CAMERA_CHECK_RETRIES):
        if verify_camera_accessible():
            break
        if attempt < CAMERA_CHECK_RETRIES:
            log.warning(f"[camera] Check attempt {attempt+1} failed; "
                        f"retrying in {CAMERA_RETRY_DELAY}s")
            time.sleep(CAMERA_RETRY_DELAY)
    else:
        log.error("Camera not reachable after "
                  f"{1 + CAMERA_CHECK_RETRIES} attempts; aborting")
        sys.exit(1)

    log.info("[youtube] Authenticating...")
    youtube = get_youtube_service()
    if not youtube:
        log.error("YouTube auth failed; aborting")
        sys.exit(1)

    RECORDING_DIR.mkdir(parents=True, exist_ok=True)
    out_path = RECORDING_DIR / f"testupload_{int(time.time())}.mp4"
    _warm_camera_path()
    log.info(f"[camera] Settling for {CAMERA_SETTLE_DELAY}s before recording...")
    time.sleep(CAMERA_SETTLE_DELAY)

    log.info(f"[record] Recording {RECORD_SECONDS}s -> {out_path}")
    proc = subprocess.Popen(build_record_cmd(out_path), stdin=subprocess.PIPE)
    try:
        proc.wait(timeout=RECORD_SECONDS + 30)
    except subprocess.TimeoutExpired:
        log.warning("[record] ffmpeg overran; killing")
        proc.kill()
        proc.wait()

    if not out_path.exists() or out_path.stat().st_size == 0:
        log.error(f"[record] {out_path} missing or empty")
        sys.exit(1)
    log.info(f"[record] Done ({out_path.stat().st_size} bytes)")

    title = f"TestUpload@{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}"
    log.info(f"[upload] Uploading as unlisted: {title!r}")
    video_id = upload_recording(youtube, out_path, title)
    if not video_id:
        log.error("[upload] Failed; leaving file on disk")
        sys.exit(1)

    log.info(f"[done] https://youtu.be/{video_id}")
    try:
        out_path.unlink()
    except Exception as e:
        log.warning(f"Could not unlink {out_path}: {e}")
