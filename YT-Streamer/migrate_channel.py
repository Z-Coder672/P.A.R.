#!/usr/bin/env python3
"""Re-upload every gallery recording from the old main channel onto the new
@PixelArtRobot channel, adding the 125x timelapse+text intro to any that don't
already have it.

WHAT IT DOES
  1. Reads the gallery (gallery.php) and collects every entry that has a
     `video_id` — i.e. every print whose recording landed on YouTube. Some of
     those are unlisted; none are private, so no cookies are needed (yt-dlp can
     fetch an unlisted video straight from its watch URL).
  2. Downloads each with yt-dlp into a work dir.
  3. Adds the timelapse intro to the ones that don't already have it, detected by
     a pixel check (label_present): a composed upload carries the black label
     bottom-right the whole video, a raw one doesn't. Composing reuses
     YT_streamer.compose_with_timelapse — the exact same "<125x timelapse +
     label> + <real-time>" the daemon now prepends. Prints already carrying the
     intro are uploaded unchanged so they can't get two.
  4. Uploads each to the channel the vault's UPLOAD token acts as (@PixelArtRobot
     on the prod Mac Mini) via YT_streamer.upload_recording — public, same title.

  This ONLY populates the new channel. It does NOT touch the gallery, the site
  embed, latest-video.json, or any playlist — the site still points at the
  original videos. A run is idempotent and resumable: every old->new mapping is
  written to the state file (default migrate_state.json) and already-migrated
  entries are skipped on the next run.

WHERE IT LANDS — READ THIS
  Uploads go to whatever channel the vault's upload token (yt_token.json) is
  consented for. Before uploading anything the script asks Google which channel
  that is (channels.list mine=True) and, unless --yes, ABORTS if it isn't
  @PixelArtRobot. Uploading dozens of PUBLIC videos to the wrong channel is the
  expensive mistake this guard exists to prevent.

RUN IT FROM THE GUI SESSION (Keychain + browser) — see run_migrate.sh. Importing
YT_streamer runs its required-env check, so this must run in YT-Streamer/ with a
populated .env, exactly like backfill_uploads.py.

    ./venv/bin/python migrate_channel.py --dry-run     # list what it would do
    ./venv/bin/python migrate_channel.py               # do it
    ./venv/bin/python migrate_channel.py --limit 1     # one video, end to end

Successfully uploaded work files are MOVED to <work-dir>/uploaded/, never
deleted (same discipline as backfill) — clear that dir by hand.
"""
from __future__ import annotations

import argparse
import json
import logging
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import requests
from PIL import Image

import YT_streamer as ys

log = ys.log

# Match backfill's credential hygiene: these third-party loggers dump tokens /
# auth headers at DEBUG, and YT_streamer pins the root logger at DEBUG.
for _noisy in ("requests_oauthlib", "requests_oauthlib.oauth2_session", "oauthlib",
               "urllib3", "urllib3.connectionpool", "google_auth_oauthlib",
               "google.auth.transport.requests", "googleapiclient.discovery"):
    logging.getLogger(_noisy).setLevel(logging.WARNING)

VIDEO_ID_RE = re.compile(r"^[A-Za-z0-9_-]{11}$")

# ── intro detection (pixel check) ─────────────────────────────────────────────
# A composed upload carries the black label ("Timelapse (125x speed)" during the
# intro, "Real-time (1x speed)" after) bottom-right for the WHOLE video. A raw
# recording has none. So we grab one frame, look at exactly where compose would
# have drawn that label, and decide "already has the intro" if that patch holds a
# cluster of near-black pixels. Single-pixel is unreliable (the label is text on
# transparent bg, so a pixel can land between glyphs); the label region + a
# small black-fraction floor is the robust version of the same idea.
DETECT_AT_S       = float(os.getenv("MIGRATE_DETECT_AT", "1.0"))
# "exactly black or very close" — luma at/below this counts as label ink.
LABEL_LUMA_MAX    = int(os.getenv("MIGRATE_LABEL_LUMA", "40"))
# Fraction of the label box that must be near-black to call it present. Black
# text fills ~10-15% of its own bbox; 1.5% is a safe floor a lit scene rarely
# hits in that specific corner.
LABEL_MIN_FRAC    = float(os.getenv("MIGRATE_LABEL_FRAC", "0.015"))


# ── gallery ─────────────────────────────────────────────────────────────────
def fetch_gallery(gallery_url: str) -> list[dict]:
    """Every gallery entry that has a real YouTube video_id, oldest id first."""
    r = requests.get(gallery_url, timeout=30)
    r.raise_for_status()
    data = r.json()
    if not isinstance(data, list):
        raise ValueError(f"gallery.php did not return a list: {type(data).__name__}")
    out = []
    for e in data:
        vid = (e.get("video_id") or "").strip()
        if not VIDEO_ID_RE.match(vid):
            continue
        try:
            gid = int(e.get("id"))
        except (TypeError, ValueError):
            continue
        out.append({"id": gid, "video_id": vid, "name": (e.get("name") or "").strip()})
    out.sort(key=lambda x: x["id"])
    return out


# ── channel guard ───────────────────────────────────────────────────────────
def identify_channel(youtube) -> dict:
    """The channel the given (upload) token actually acts as."""
    resp = youtube.channels().list(part="snippet", mine=True).execute()
    items = resp.get("items") or []
    if not items:
        raise RuntimeError("channels.list(mine=True) returned no channel for this token")
    sn = items[0].get("snippet", {})
    return {
        "id": items[0].get("id", ""),
        "title": sn.get("title", ""),
        "handle": (sn.get("customUrl") or "").lstrip("@"),
    }


# ── download ────────────────────────────────────────────────────────────────
def download_video(video_id: str, work_dir: Path) -> tuple[Path, dict] | None:
    """yt-dlp the video (video-only uploads fall back to `best`) into work_dir.
    Returns (media_path, info_dict) or None on failure. info carries the
    original title for the re-upload."""
    url = f"https://www.youtube.com/watch?v={video_id}"
    out_tmpl = str(work_dir / f"{video_id}.%(ext)s")
    info_json = work_dir / f"{video_id}.info.json"
    cmd = [
        "yt-dlp",
        "--no-playlist",
        "--no-progress",
        "-f", "bestvideo*+bestaudio/best",
        "--merge-output-format", "mp4",
        "--write-info-json",
        "--no-simulate",
        "-o", out_tmpl,
        url,
    ]
    log.info(f"[migrate] downloading {video_id} ...")
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        log.error(f"[migrate] yt-dlp failed for {video_id} (rc={proc.returncode}): "
                  f"{proc.stderr.strip()[-400:]}")
        return None
    info: dict = {}
    if info_json.exists():
        try:
            info = json.loads(info_json.read_text())
        except Exception as e:
            log.warning(f"[migrate] could not parse {info_json.name}: {e!r}")
    # Find the media file yt-dlp actually wrote (extension varies before/after merge).
    media = [p for p in work_dir.glob(f"{video_id}.*")
             if p.suffix.lower() not in (".json",)]
    if not media:
        log.error(f"[migrate] no media file found for {video_id} after download")
        return None
    # Prefer the merged mp4 if present; otherwise the largest artifact.
    media.sort(key=lambda p: (p.suffix.lower() != ".mp4", -p.stat().st_size))
    return media[0], info


def _label_box(frame_w: int, frame_h: int) -> tuple[int, int, int, int] | None:
    """The bottom-right box where compose_with_timelapse draws its label, in the
    frame's own pixels. Re-renders the real label PNG (same font, same code) so
    the box matches what would have been composited. Returns (l, t, r, b) or None."""
    mult = round(ys.TIMELAPSE_SPEED, 1)
    tl_text = f"Timelapse ({mult:g}x speed)"     # the intro label; see compose_with_timelapse
    margin = max(8, int(round(frame_h / ys.TIMELAPSE_MARGIN_DIV)))
    with tempfile.TemporaryDirectory(prefix="par_lbl_") as td:
        png = Path(td) / "lbl.png"
        ys._render_label_png(tl_text, frame_h, png)
        with Image.open(png) as im:
            lw, lh = im.size
    # overlay pos in _segment_cmd: x=W-w-margin, y=H-h-margin
    r, b = frame_w - margin, frame_h - margin
    l, t = r - lw, b - lh
    if l < 0 or t < 0 or r <= l or b <= t:
        return None
    return l, t, r, b


def label_present(src: Path) -> bool | None:
    """True if the frame at DETECT_AT_S already carries the timelapse label
    (near-black cluster where the text would be), False if not, None if the
    check couldn't run (so the caller can fall back)."""
    with tempfile.TemporaryDirectory(prefix="par_frame_") as td:
        frame = Path(td) / "f.png"
        cmd = ["ffmpeg", "-hide_banner", "-nostdin", "-y", "-loglevel", "error",
               "-ss", f"{DETECT_AT_S:.3f}", "-i", str(src),
               "-frames:v", "1", str(frame)]
        proc = subprocess.run(cmd, capture_output=True)
        if proc.returncode != 0 or not frame.exists():
            log.warning(f"[migrate] frame grab failed on {src.name}: "
                        f"{proc.stderr.decode('utf-8','replace').strip()[-200:]}")
            return None
        with Image.open(frame) as im:
            W, H = im.size
            box = _label_box(W, H)
            if box is None:
                return None
            patch = im.crop(box).convert("L")
    px = list(patch.getdata())
    if not px:
        return None
    near_black = sum(1 for v in px if v <= LABEL_LUMA_MAX)
    frac = near_black / len(px)
    present = frac >= LABEL_MIN_FRAC
    log.info(f"[migrate] label check {src.name}: {frac*100:.1f}% near-black in "
             f"label box (>= {LABEL_MIN_FRAC*100:.1f}% => "
             f"{'HAS intro' if present else 'no intro'})")
    return present


def wants_intro(src: Path, force: bool, disable: bool) -> bool:
    """Whether to compose the intro onto this download."""
    if disable:
        return False
    if force:
        return True
    present = label_present(src)
    if present is None:
        # Detection failed — err toward NOT double-composing a video that might
        # already have the label. Uploading a raw one without an intro is a far
        # cheaper mistake than stacking two intros, and it's visible in the log.
        log.warning(f"[migrate] intro detection inconclusive for {src.name}; "
                     "uploading as-is (use --force-intro to compose anyway)")
        return False
    return not present


# ── main ────────────────────────────────────────────────────────────────────
def load_state(path: Path) -> dict:
    if path.exists():
        try:
            return json.loads(path.read_text())
        except Exception as e:
            log.warning(f"[migrate] state file unreadable ({e!r}); starting fresh")
    return {}


def save_state(path: Path, state: dict) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(state, indent=2, sort_keys=True))
    tmp.replace(path)


def move_aside(paths: list[Path], done_dir: Path) -> None:
    """Move finished work files out of the way — never delete (data-safety)."""
    done_dir.mkdir(parents=True, exist_ok=True)
    for p in paths:
        if p and p.exists():
            try:
                shutil.move(str(p), str(done_dir / p.name))
            except Exception as e:
                log.warning(f"[migrate] could not move {p.name} aside: {e!r}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gallery-url",
                    default=f"{ys.SITE_BASE_URL}/gallery.php",
                    help="gallery.php URL (default derived from SITE_BASE_URL)")
    ap.add_argument("--work-dir", default="/tmp/par-migrate",
                    help="download / compose scratch dir (default /tmp/par-migrate)")
    ap.add_argument("--state", default=None,
                    help="resume state file (default <work-dir>/migrate_state.json)")
    ap.add_argument("--force-intro", action="store_true",
                    help="compose the intro on EVERY video, skipping the pixel "
                         "check (only if you're sure none already have it)")
    ap.add_argument("--no-intro", action="store_true",
                    help="never compose; re-upload every video exactly as downloaded")
    ap.add_argument("--expect-handle", default="PixelArtRobot",
                    help="abort unless the upload token is on this @handle "
                         "(default PixelArtRobot); empty string disables the check")
    ap.add_argument("--yes", action="store_true",
                    help="skip the channel-handle guard (upload wherever the token lands)")
    ap.add_argument("--limit", type=int, default=0,
                    help="process at most N not-yet-migrated entries (0 = all)")
    ap.add_argument("--dry-run", action="store_true",
                    help="list what would happen; no download, no upload")
    args = ap.parse_args()

    if args.force_intro and args.no_intro:
        log.error("[migrate] --force-intro and --no-intro are mutually exclusive")
        return 2

    work_dir = Path(args.work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)
    done_dir = work_dir / "uploaded"
    state_path = Path(args.state) if args.state else work_dir / "migrate_state.json"
    state = load_state(state_path)

    entries = fetch_gallery(args.gallery_url)
    todo = [e for e in entries if e["video_id"] not in state]
    log.info(f"[migrate] gallery has {len(entries)} videos; "
             f"{len(entries) - len(todo)} already migrated; {len(todo)} to do")

    if args.dry_run:
        shown = todo[:args.limit] if args.limit else todo
        for e in shown:
            log.info(f"[migrate] would migrate #{e['id']} {e['video_id']} "
                     f"({e['name'] or 'Untitled'})")
        log.info(f"[migrate] dry-run: {len(shown)} of {len(todo)} shown "
                 "(intro decision is a per-video pixel check after download)")
        return 0

    if not todo:
        log.info("[migrate] nothing to do")
        return 0

    # Auth + channel guard BEFORE downloading anything.
    youtube = ys.get_youtube_service(interactive=True)
    ch = identify_channel(youtube)
    log.info(f"[migrate] upload token acts as: {ch['title']!r} "
             f"@{ch['handle'] or '?'} (id {ch['id']})")
    want = args.expect_handle.lstrip("@").lower()
    if want and not args.yes and ch["handle"].lower() != want:
        log.error(f"[migrate] REFUSING: token is on @{ch['handle'] or '?'}, "
                  f"expected @{want}. Re-run with --yes to override, or fix the "
                  "vault's upload token with ./run_auth_setup.sh --which upload")
        return 3

    processed = 0
    ok = 0
    for e in todo:
        if args.limit and processed >= args.limit:
            break
        processed += 1
        gid, vid, name = e["id"], e["video_id"], e["name"]
        log.info(f"[migrate] === #{gid} {vid} ({name or 'Untitled'}) ===")

        dl = download_video(vid, work_dir)
        if not dl:
            continue
        src, info = dl

        composed = None
        if wants_intro(src, args.force_intro, args.no_intro):
            log.info(f"[migrate] adding timelapse intro to {vid}")
            composed = ys.compose_with_timelapse(src)   # None -> upload src as-is
        else:
            log.info(f"[migrate] {vid} already has the intro; uploading unchanged")
        upload_path = composed or src

        title = (info.get("title") or "").strip()
        if not title:
            title = f'"{name or "Untitled"}" printing - P.A.R.'

        new_id = ys.upload_recording(youtube, upload_path, title)
        if not new_id:
            log.warning(f"[migrate] upload failed for {vid}; leaving files in {work_dir}")
            ys._drop_composed(composed)
            continue

        state[vid] = {"new_video_id": new_id, "gallery_id": gid, "title": title}
        save_state(state_path, state)
        ys._drop_composed(composed)
        move_aside([src], done_dir)
        ok += 1
        log.info(f"[migrate] #{gid} {vid} -> https://youtu.be/{new_id}  (public)")

    log.info(f"[migrate] done: {ok}/{processed} uploaded; state in {state_path}")
    return 0 if ok == processed else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        log.warning("[migrate] interrupted")
        sys.exit(130)
