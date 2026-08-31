#!/usr/bin/env python3
"""Re-auth YouTube and upload the recordings the daemon left stranded on disk.

WHY THIS EXISTS: when the OAuth refresh token dies (Google kills them 7 days
after issuance while the app is in 'Testing' status), `_upload_and_attach`
fails with RefreshError and deliberately leaves the .mov in RECORDING_DIR.
The daemon caches its YouTube client for the life of the process and never
re-auths, so every subsequent print piles up another orphan file. This script
is the recovery path: it runs the interactive consent flow (which a headless
daemon thread must never do), writes the fresh token back to the vault, then
uploads every orphan and attaches its video_id to the gallery entry.

Run it from the Mac's GUI session (or anywhere you can open a browser):

    ./venv/bin/python backfill_uploads.py            # re-auth if needed, upload all
    ./venv/bin/python backfill_uploads.py --dry-run  # just list what it would do
    ./venv/bin/python backfill_uploads.py --force-reauth

It skips any .mov still being written by the running daemon, so it is safe to
run while a print is recording.

A successfully uploaded .mov is MOVED to RECORDING_DIR/uploaded/, never removed
— check that directory is empty-able by hand before it fills the disk.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path

import requests
from google.oauth2.credentials import Credentials
from google.auth.transport.requests import Request
from google_auth_oauthlib.flow import InstalledAppFlow
from googleapiclient.discovery import build

import YT_streamer as ys

log = ys.log

# <gallery_id>_<unix_ts>.mov — the name record_orchestrator writes.
MOV_RE = re.compile(r"^(\d+)_(\d+)\.mov$")

DONE_DIR = ys.RECORDING_DIR / "uploaded"


def _find_orphans() -> list[tuple[int, Path]]:
    """Orphan recordings, oldest print first. A .mov whose size is still
    changing belongs to the in-flight recording — leave it to the daemon."""
    out: list[tuple[int, Path]] = []
    for p in sorted(ys.RECORDING_DIR.glob("*.mov")):
        m = MOV_RE.match(p.name)
        if not m:
            log.warning(f"[backfill] skipping unrecognised name: {p.name}")
            continue
        first = p.stat().st_size
        time.sleep(1.5)
        if p.stat().st_size != first:
            log.info(f"[backfill] {p.name} is still growing (recording in "
                     f"progress) — leaving it to the daemon")
            continue
        if first == 0:
            log.warning(f"[backfill] {p.name} is empty — skipping")
            continue
        out.append((int(m.group(1)), p))
    return sorted(out, key=lambda t: t[0])


def _gallery_rows() -> list[dict]:
    resp = requests.get(ys.GALLERY_URL, headers={"User-Agent": "P.A.R./1.0"}, timeout=20)
    resp.raise_for_status()
    data = resp.json()
    return data if isinstance(data, list) else data.get("entries", data.get("items", []))


def _gallery_state() -> tuple[dict[int, str], dict[int, str]]:
    """(id -> name, id -> already-attached video_id), straight from gallery.php
    so titles match what the daemon would have used and an entry that somehow
    already has a recording is never uploaded twice."""
    names: dict[int, str] = {}
    videos: dict[int, str] = {}
    for e in _gallery_rows():
        try:
            gid = int(e["id"])
        except (KeyError, TypeError, ValueError):
            continue
        names[gid] = (e.get("name") or "").strip()
        if e.get("video_id"):
            videos[gid] = e["video_id"]
    return names, videos


def authenticate(force_reauth: bool = False):
    """Return a YouTube service, running the interactive consent flow when the
    stored refresh token is dead. Unlike the daemon's get_youtube_service this
    is allowed to block on a human — that's the whole point of running it by
    hand. The fresh token goes back into the vault so the daemon picks it up
    on its next restart."""
    creds = None
    if not force_reauth:
        token_json = ys._vault_read_token()
        if token_json:
            creds = Credentials.from_authorized_user_info(json.loads(token_json), ys.YT_SCOPES)
        if creds and creds.valid:
            log.info("[backfill] stored token is still valid")
            return build("youtube", "v3", credentials=creds)
        if creds and creds.refresh_token:
            log.info("[backfill] refreshing stored token...")
            try:
                creds.refresh(Request())
                ys._vault_write_token(creds.to_json())
                log.info("[backfill] refresh succeeded")
                return build("youtube", "v3", credentials=creds)
            except Exception as e:
                log.warning(f"[backfill] refresh failed ({e!r}) — falling back to consent flow")

    log.info("[backfill] starting interactive OAuth consent flow for the UPLOAD "
             "token — at the channel picker choose the channel the RECORDINGS "
             "belong on, NOT the one that owns the playlist. (The playlist "
             "token is a separate grant: ./run_auth_setup.sh --which playlist)")
    client_config = ys._load_client_secrets()
    flow = InstalledAppFlow.from_client_config(client_config, ys.YT_SCOPES)
    # access_type=offline + prompt=consent forces Google to hand back a NEW
    # refresh token. Without prompt=consent a re-auth of an already-approved
    # app returns an access token only, and we'd be dead again in hours.
    creds = flow.run_local_server(
        port=0,
        access_type="offline",
        prompt="consent",
        open_browser=True,
        authorization_prompt_message=(
            "\n>>> UPLOAD token — pick the channel the RECORDINGS go on.\n"
            ">>> Open this URL and click Allow:\n\n{url}\n"
        ),
    )
    ys._vault_write_token(creds.to_json())
    log.info("[backfill] new token written to vault — restart the daemon to pick it up")
    return build("youtube", "v3", credentials=creds)


def _retire(path: Path) -> None:
    """Move an uploaded recording out of the pickup directory. Deliberately a
    move, not an unlink: a botched upload that still returned an id is
    recoverable from here."""
    try:
        DONE_DIR.mkdir(parents=True, exist_ok=True)
        path.rename(DONE_DIR / path.name)
        log.info(f"[backfill] moved {path.name} -> {DONE_DIR}/")
    except Exception as e:
        log.warning(f"[backfill] could not move {path}: {e}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true",
                    help="list the orphans and exit without authenticating")
    ap.add_argument("--force-reauth", action="store_true",
                    help="skip the stored token and go straight to the consent flow")
    ap.add_argument("--keep", action="store_true",
                    help="leave a .mov in place after upload instead of moving it aside")
    args = ap.parse_args()

    orphans = _find_orphans()
    if not orphans:
        log.info("[backfill] no orphaned recordings — nothing to do")
        return 0

    names, already = _gallery_state()

    pending: list[tuple[int, Path, str]] = []
    for gid, path in orphans:
        if gid in already:
            log.warning(f"[backfill] #{gid} already has video_id={already[gid]} — "
                        f"skipping {path.name}")
            continue
        name = names.get(gid) or "Untitled"
        pending.append((gid, path, name))

    total = sum(p.stat().st_size for _, p, _ in pending)
    log.info(f"[backfill] {len(pending)} recording(s) to upload, "
             f"{total / 1e9:.2f} GB total")
    for gid, path, name in pending:
        log.info(f"[backfill]   #{gid} {name!r}  {path.name}  "
                 f"({path.stat().st_size / 1e6:.0f} MB)")
    if args.dry_run:
        log.info("[backfill] --dry-run: stopping here")
        return 0
    if not pending:
        return 0

    youtube = authenticate(force_reauth=args.force_reauth)

    ok, failed = 0, 0
    for gid, path, name in pending:
        title = f'"{name}" printing - P.A.R.'
        log.info(f"[backfill] uploading #{gid}: {title!r}")
        try:
            video_id = ys.upload_recording(youtube, path, title)
        except Exception as e:
            log.error(f"[backfill] #{gid} raised: {e!r}")
            video_id = None
        if not video_id:
            log.warning(f"[backfill] #{gid} failed; leaving {path} on disk")
            failed += 1
            continue
        ys._post_video_id(gid, video_id)
        # Same best-effort playlist filing the daemon does, so a recovered
        # recording ends up in the playlist too. `youtube` is the upload
        # client, passed only as the single-channel fallback.
        ys.attach_to_playlist(video_id, gid, upload_client=youtube)
        if not args.keep:
            _retire(path)
        ok += 1

    log.info(f"[backfill] done — {ok} uploaded, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
