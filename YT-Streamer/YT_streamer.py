#!/usr/bin/env python3
"""
24/7 YouTube Live Streamer + Snapshot Poller
Pulls RTSP from Eufy cam, pushes to YouTube Live with auto-reconnect.
Uses YouTube Data API v3 to auto-create public broadcasts when camera is reachable.
Also polls Site5 for snapshot requests from Arduino and uploads to gallery.
"""

import subprocess
import time
import logging
import sys
import threading
import json
import requests
import paramiko
import os
from datetime import datetime
from pathlib import Path
from dotenv import load_dotenv

from google.oauth2.credentials import Credentials
from google_auth_oauthlib.flow import InstalledAppFlow
from google.auth.transport.requests import Request
from googleapiclient.discovery import build
from googleapiclient.errors import HttpError

load_dotenv(Path(__file__).parent / ".env")

# ── CONFIG ─────────────────────────────────────────────────────────────────────
RTSP_URL      = os.getenv("RTSP_URL")
YT_STREAM_KEY = os.getenv("YT_STREAM_KEY")   # optional static fallback
SNAPSHOT_SECRET        = os.getenv("SNAPSHOT_SECRET")

SNAPSHOT_REQUEST_URL   = os.getenv("SNAPSHOT_REQUEST_URL")
SNAPSHOT_POLL_INTERVAL = 5
SNAPSHOT_LOCAL_DIR     = Path("/tmp/snapshots")

SFTP_HOST       = os.getenv("SFTP_HOST")
SFTP_PORT       = int(os.getenv("SFTP_PORT", "22"))
SFTP_USER       = os.getenv("SFTP_USER")
SFTP_KEY_PATH   = Path(os.getenv("SFTP_KEY_PATH", str(Path.home() / ".ssh" / "id_rsa")))
SFTP_PASSWORD   = os.getenv("SFTP_PASSWORD") or None
SFTP_REMOTE_DIR = os.getenv("SFTP_REMOTE_DIR")

# YouTube Data API OAuth (write scope for broadcast management)
YT_SCOPES              = ["https://www.googleapis.com/auth/youtube"]
YT_VAULT_DMG           = Path(__file__).parent / "YT_streamer_vault.dmg"
YT_VAULT_KEYCHAIN_KEY  = "YT-streamer-vault-pass"
YT_VAULT_SECRET_FILE   = (
    "client_secret_27189767089-e62aj0abrchdkl8romoeq0ua4l6nem9k"
    ".apps.googleusercontent.com.json"
)
YT_VAULT_TOKEN_FILE    = "yt_token.json"

_required = {
    "RTSP_URL": RTSP_URL,
    "SNAPSHOT_SECRET": SNAPSHOT_SECRET,
    "SNAPSHOT_REQUEST_URL": SNAPSHOT_REQUEST_URL,
    "SFTP_HOST": SFTP_HOST,
    "SFTP_USER": SFTP_USER,
    "SFTP_REMOTE_DIR": SFTP_REMOTE_DIR,
}
_missing = [k for k, v in _required.items() if not v]
if _missing:
    print("ERROR: Missing required environment variables:", ", ".join(_missing))
    sys.exit(1)

VIDEO_BITRATE        = "2500k"
AUDIO_BITRATE        = "128k"
RESTART_DELAY        = 5    # seconds between stream reconnect attempts
LIVE_TRANSITION_DELAY = 20  # seconds after ffmpeg start before forcing "live" transition
# ───────────────────────────────────────────────────────────────────────────────

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    handlers=[
        logging.StreamHandler(sys.stdout),
        logging.FileHandler("stream.log"),
    ]
)
log = logging.getLogger(__name__)


def build_ffmpeg_cmd(rtmp_url: str) -> list[str]:
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
        "-f", "flv",
        rtmp_url,
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
        if creds and creds.expired and creds.refresh_token:
            log.info("[youtube] Refreshing OAuth token...")
            creds.refresh(Request())
        else:
            log.info("[youtube] Starting OAuth flow...")
            try:
                client_config = _load_client_secrets()
            except Exception as e:
                log.error(f"[vault] Failed to load client secrets: {e}")
                log.warning("[youtube] Will use static YT_STREAM_KEY if set.")
                return None
            flow = InstalledAppFlow.from_client_config(client_config, YT_SCOPES)
            try:
                creds = flow.run_local_server(port=0)
            except Exception:
                # Headless fallback: print URL, prompt for code
                creds = flow.run_console()
        _vault_write_token(creds.to_json())

    return build("youtube", "v3", credentials=creds)


def create_live_broadcast(youtube) -> tuple[str, str, str]:
    """
    Creates a YouTube broadcast + stream and binds them.
    Returns (broadcast_id, stream_id, stream_key).
    enableAutoStart transitions broadcast to live automatically once data flows in;
    we also schedule a manual transition as a fallback.
    """
    now = datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%S.000Z")

    broadcast = youtube.liveBroadcasts().insert(
        part="snippet,status,contentDetails",
        body={
            "snippet": {
                "title": "P.A.R. Livestream",
                "scheduledStartTime": now,
            },
            "status": {
                "privacyStatus": "public",
                "selfDeclaredMadeForKids": False,
            },
            "contentDetails": {
                "enableAutoStart": True,
                "enableAutoStop": True,
                "latencyPreference": "ultraLow",
            },
        },
    ).execute()

    broadcast_id = broadcast["id"]
    log.info(f"[youtube] Broadcast created: {broadcast_id}")

    yt_stream = youtube.liveStreams().insert(
        part="snippet,cdn",
        body={
            "snippet": {"title": "P.A.R."},
            "cdn": {
                "frameRate": "30fps",
                "ingestionType": "rtmp",
                "resolution": "1080p",
            },
        },
    ).execute()

    stream_id  = yt_stream["id"]
    stream_key = yt_stream["cdn"]["ingestionInfo"]["streamName"]
    log.info(f"[youtube] Stream created: {stream_id}")

    youtube.liveBroadcasts().bind(
        part="id,contentDetails",
        id=broadcast_id,
        streamId=stream_id,
    ).execute()
    log.info(f"[youtube] Bound broadcast → stream")

    return broadcast_id, stream_id, stream_key


def transition_broadcast(youtube, broadcast_id: str, status: str):
    """Transition broadcast to 'testing', 'live', or 'complete'."""
    try:
        youtube.liveBroadcasts().transition(
            broadcastStatus=status,
            id=broadcast_id,
            part="id,status",
        ).execute()
        log.info(f"[youtube] Broadcast {broadcast_id} → {status}")
    except HttpError as e:
        if "redundantTransition" in str(e):
            log.debug(f"[youtube] Already in {status} (redundantTransition)")
        else:
            log.error(f"[youtube] Transition to {status} failed: {e}")
    except Exception as e:
        log.error(f"[youtube] Transition to {status} failed: {e}")


# ── CAMERA ─────────────────────────────────────────────────────────────────────

def verify_camera_accessible() -> bool:
    """Pull one frame from RTSP to confirm the camera is reachable before creating a broadcast."""
    log.info("[camera] Verifying camera is accessible...")
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


# ── SNAPSHOT ───────────────────────────────────────────────────────────────────

def grab_snapshot() -> Path | None:
    """Grab a single frame from the RTSP stream, return local path or None."""
    SNAPSHOT_LOCAL_DIR.mkdir(parents=True, exist_ok=True)
    filename = datetime.now().strftime("snap_%Y%m%d_%H%M%S.jpg")
    out_path = SNAPSHOT_LOCAL_DIR / filename

    cmd = [
        "ffmpeg", "-y",
        "-rtsp_transport", "tcp",
        "-i", RTSP_URL,
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
        return None


def upload_snapshot(local_path: Path, gallery_id: int | None) -> bool:
    """Upload snapshot to Site5 gallery via SFTP.

    If gallery_id is provided, the file lands at gallery/<id>/image.jpg so it
    can be associated with the gallery entry. Otherwise it falls back to the
    timestamped filename in the gallery root for ad-hoc captures.
    """
    try:
        transport = paramiko.Transport((SFTP_HOST, SFTP_PORT))
        if SFTP_PASSWORD:
            transport.connect(username=SFTP_USER, password=SFTP_PASSWORD)
        else:
            key = paramiko.RSAKey.from_private_key_file(str(SFTP_KEY_PATH))
            transport.connect(username=SFTP_USER, pkey=key)

        sftp = paramiko.SFTPClient.from_transport(transport)
        if gallery_id is not None:
            entry_dir = f"{SFTP_REMOTE_DIR}/{gallery_id}"
            try:
                sftp.stat(entry_dir)
            except IOError:
                sftp.mkdir(entry_dir)
            remote_path = f"{entry_dir}/image.jpg"
        else:
            remote_path = f"{SFTP_REMOTE_DIR}/{local_path.name}"
        sftp.put(str(local_path), remote_path)
        sftp.close()
        transport.close()

        log.info(f"[snapshot] Uploaded to {remote_path}")
        local_path.unlink()
        return True
    except Exception as e:
        log.error(f"[snapshot] SFTP upload failed: {e}")
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


# ── STREAM ─────────────────────────────────────────────────────────────────────

def stream(youtube):
    attempt = 0
    while True:
        attempt += 1
        log.info(f"Starting stream attempt #{attempt}")

        # Confirm camera is reachable before creating a broadcast
        while not verify_camera_accessible():
            log.warning(f"[camera] Not reachable — retrying in {RESTART_DELAY}s...")
            time.sleep(RESTART_DELAY)
        log.info("[camera] Camera is accessible")

        # Create a YouTube broadcast via API; fall back to static key
        broadcast_id = None
        rtmp_url = None

        if youtube:
            try:
                broadcast_id, _, stream_key = create_live_broadcast(youtube)
                rtmp_url = f"rtmp://a.rtmp.youtube.com/live2/{stream_key}"
            except Exception as e:
                log.error(f"[youtube] Broadcast creation failed: {e}")

        if not rtmp_url:
            if YT_STREAM_KEY:
                log.warning("[youtube] Falling back to static YT_STREAM_KEY")
                rtmp_url = f"rtmp://a.rtmp.youtube.com/live2/{YT_STREAM_KEY}"
            else:
                log.error(f"[youtube] No stream key available — retrying in {RESTART_DELAY}s...")
                time.sleep(RESTART_DELAY)
                continue

        ffmpeg_cmd = build_ffmpeg_cmd(rtmp_url)
        start = datetime.now()
        exit_code = -1
        proc = None

        try:
            proc = subprocess.Popen(ffmpeg_cmd)

            # Belt-and-suspenders: try forcing "live" after a delay in case
            # enableAutoStart doesn't fire (redundantTransition error is safe to ignore).
            if broadcast_id:
                def _force_live(bid=broadcast_id):
                    time.sleep(LIVE_TRANSITION_DELAY)
                    transition_broadcast(youtube, bid, "live")
                threading.Thread(target=_force_live, daemon=True).start()

            proc.wait()
            exit_code = proc.returncode

        except FileNotFoundError:
            log.error("ffmpeg not found — is it installed and on PATH?")
            sys.exit(1)
        except KeyboardInterrupt:
            log.info("Interrupted by user. Stopping.")
            if proc and proc.poll() is None:
                proc.terminate()
            if broadcast_id:
                transition_broadcast(youtube, broadcast_id, "complete")
            sys.exit(0)

        if broadcast_id:
            transition_broadcast(youtube, broadcast_id, "complete")

        duration = (datetime.now() - start).seconds
        log.warning(
            f"Stream ended (exit code {exit_code}) after {duration}s. "
            f"Restarting in {RESTART_DELAY}s..."
        )
        time.sleep(RESTART_DELAY)


# ── MAIN ───────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    log.info("═" * 50)
    log.info("  24/7 YouTube Live Streamer + Snapshot Poller")
    log.info("═" * 50)
    log.info(f"  Source : {RTSP_URL.split('@')[-1]}")
    log.info(f"  Target : YouTube Live (API-managed broadcasts)")
    log.info(f"  Video  : {VIDEO_BITRATE} H.264 veryfast")
    log.info(f"  Audio  : {AUDIO_BITRATE} AAC")
    log.info(f"  Poller : {SNAPSHOT_REQUEST_URL}")
    log.info("═" * 50)

    youtube = get_youtube_service()
    if not youtube:
        log.warning("[youtube] Running without API — will use static YT_STREAM_KEY")

    poller = threading.Thread(target=poll_snapshot_queue, daemon=True)
    poller.start()

    stream(youtube)
