#!/usr/bin/env python3
"""Consent, verify and install the TWO YouTube OAuth tokens this daemon needs.

WHY TWO: a YouTube OAuth token is bound to exactly ONE channel.

  * the UPLOAD token   -> the channel `videos.insert` puts the recordings on
  * the PLAYLIST token -> the channel that OWNS YT_PLAYLIST_ID, because
                          `playlistItems.insert` is authorised against the
                          playlist's owning channel, not the video's

When those are the same channel one token would do. When they are different —
uploads on one channel, playlist on another — there is no single-token way to do
it (`onBehalfOfContentOwner` is CMS/partner-only), so each side gets its own
separately-consented token in the same encrypted vault, under different
filenames (YT_VAULT_TOKEN_FILE / YT_VAULT_PLAYLIST_TOKEN_FILE).

THE HAZARD this script exists to remove: the two consent screens are visually
identical. Picking the wrong channel silently uploads to the wrong place or
404s every playlist insert. So nothing is written to the vault until the token
has been used to ask Google *which channel it actually belongs to*, that channel
is printed, and — for the playlist token — it is proved to own YT_PLAYLIST_ID.

Run from the Mac's GUI session (needs the login Keychain for the vault and a
browser for consent) — ../run_auth_setup.sh does that for you over SSH:

    ./venv/bin/python setup_youtube_auth.py --show          # which channel is each token on?
    ./venv/bin/python setup_youtube_auth.py                 # re-consent both, in order
    ./venv/bin/python setup_youtube_auth.py --which upload
    ./venv/bin/python setup_youtube_auth.py --which playlist
"""
from __future__ import annotations

import argparse
import json
import sys

from google.oauth2.credentials import Credentials
from google_auth_oauthlib.flow import InstalledAppFlow
from googleapiclient.discovery import build

import YT_streamer as ys

log = ys.log

UPLOAD = "upload"
PLAYLIST = "playlist"


# ── presentation ──────────────────────────────────────────────────────────────

def banner(lines: list[str]) -> None:
    width = max(len(l) for l in lines) + 4
    print("\n" + "=" * width)
    for l in lines:
        print("| " + l.ljust(width - 4) + " |")
    print("=" * width + "\n")


def confirm(question: str, default_no: bool = True) -> bool:
    suffix = " [y/N] " if default_no else " [Y/n] "
    try:
        ans = input(question + suffix).strip().lower()
    except EOFError:
        return False
    if not ans:
        return not default_no
    return ans in ("y", "yes")


# ── channel identification ────────────────────────────────────────────────────

def describe_channel(youtube) -> dict | None:
    """Ask Google which channel this credential actually acts as.

    This is the whole point of the script: the consent screen doesn't tell you
    afterwards what you picked, so we ask the API. Returns
    {id, title, handle} or None if the account has no channel.
    """
    resp = youtube.channels().list(part="snippet", mine=True).execute()
    items = resp.get("items") or []
    if not items:
        return None
    snip = items[0].get("snippet", {})
    return {
        "id": items[0].get("id", "?"),
        "title": snip.get("title", "?"),
        # customUrl is the @handle, lower-cased by Google and only present once
        # the channel has claimed one.
        "handle": snip.get("customUrl", ""),
    }


def fmt_channel(ch: dict | None) -> str:
    if not ch:
        return "<no channel on this account>"
    handle = f" ({ch['handle']})" if ch.get("handle") else ""
    return f"{ch['title']}{handle}  [{ch['id']}]"


def playlist_owner(youtube, playlist_id: str) -> dict | None:
    """{title, channelId} for a playlist, as seen by this credential. None if
    the credential can't see it at all."""
    try:
        resp = youtube.playlists().list(part="snippet", id=playlist_id).execute()
    except Exception as e:
        log.warning(f"[setup] playlists.list failed: {e!r}")
        return None
    items = resp.get("items") or []
    if not items:
        return None
    snip = items[0].get("snippet", {})
    return {"title": snip.get("title", "?"), "channelId": snip.get("channelId", "?")}


# ── consent ───────────────────────────────────────────────────────────────────

def run_consent(prompt_line: str):
    """Interactive consent flow returning fresh Credentials (NOT yet stored).

    access_type=offline + prompt=consent forces Google to hand back a NEW
    refresh token; without prompt=consent a re-auth of an already-approved app
    returns an access token only and we'd be dead again within the hour.
    """
    client_config = ys._load_client_secrets()
    flow = InstalledAppFlow.from_client_config(client_config, ys.YT_SCOPES)
    return flow.run_local_server(
        port=0,
        access_type="offline",
        prompt="consent",
        open_browser=True,
        authorization_prompt_message=(
            f"\n>>> {prompt_line}\n"
            f">>> If the browser didn't open, use this URL:\n\n{{url}}\n"
        ),
    )


def do_token(kind: str, step: str, expect_handle: str | None, playlist_id: str) -> bool:
    """Consent for one token, verify what channel it landed on, and only then
    write it to the vault. Returns True if a token was installed."""
    if kind == UPLOAD:
        token_file = ys.YT_VAULT_TOKEN_FILE
        banner([
            f"{step}  UPLOAD TOKEN",
            "",
            "This token decides WHICH CHANNEL THE RECORDINGS ARE UPLOADED TO.",
            "At the Google account / channel picker, choose the channel the",
            "videos should live on"
            + (f":  {expect_handle}" if expect_handle else "."),
            "",
            f"It will be stored in the vault as: {token_file}",
        ])
        prompt_line = ("UPLOAD token — pick the channel the RECORDINGS go on"
                       + (f" ({expect_handle})" if expect_handle else ""))
    else:
        token_file = ys.YT_VAULT_PLAYLIST_TOKEN_FILE
        banner([
            f"{step}  PLAYLIST TOKEN",
            "",
            "This token decides WHICH CHANNEL'S PLAYLIST WE CAN WRITE TO.",
            "At the picker, choose the channel that OWNS the playlist —",
            "this is the OTHER channel, not the upload one.",
            "",
            f"Playlist: {playlist_id or '<YT_PLAYLIST_ID not set!>'}",
            f"It will be stored in the vault as: {token_file}",
        ])
        prompt_line = "PLAYLIST token — pick the channel that OWNS the playlist"
        if not playlist_id:
            log.error("[setup] YT_PLAYLIST_ID is not set in .env — set it first, "
                      "otherwise there is nothing to verify this token against.")
            return False

    if not confirm("Ready to open the consent screen?", default_no=False):
        log.info(f"[setup] skipped {kind} token")
        return False

    creds = run_consent(prompt_line)
    youtube = build("youtube", "v3", credentials=creds)

    try:
        ch = describe_channel(youtube)
    except Exception as e:
        log.error(f"[setup] could not read the channel for this token: {e!r}")
        return False

    print()
    log.info(f"[setup] this {kind} token acts as: {fmt_channel(ch)}")

    if not ch:
        log.error("[setup] that Google account has no YouTube channel — nothing "
                  "was written. Re-run and pick a channel.")
        return False

    if kind == UPLOAD:
        if expect_handle:
            want = expect_handle.lower().lstrip("@")
            got = (ch.get("handle") or "").lower().lstrip("@")
            if want != got:
                log.warning(f"[setup] expected @{want} but this token is @{got or '?'}")
                if not confirm("That is NOT the channel you asked for. Store it anyway?"):
                    log.info("[setup] nothing written — re-run and pick the right channel")
                    return False
            else:
                log.info(f"[setup] channel matches {expect_handle}")
        if not confirm(f"Upload all future recordings to {fmt_channel(ch)}?",
                       default_no=False):
            log.info("[setup] nothing written")
            return False
    else:
        owner = playlist_owner(youtube, playlist_id)
        if owner is None:
            log.error(
                f"[setup] this token cannot see playlist {playlist_id} at all. "
                f"Either the id is wrong, or the playlist is private and belongs "
                f"to a DIFFERENT channel than the one just authorised "
                f"({fmt_channel(ch)}). Nothing written.")
            return False
        log.info(f"[setup] playlist {playlist_id!r} is {owner['title']!r} "
                 f"owned by channel {owner['channelId']}")
        if owner["channelId"] != ch["id"]:
            # A public playlist is readable by anyone, so seeing it proves
            # nothing — only ownership lets us insert into it.
            log.error(
                f"[setup] MISMATCH: that playlist belongs to {owner['channelId']} "
                f"but this token is for {ch['id']}. playlistItems.insert would be "
                f"rejected. Nothing written — re-run and pick the channel that "
                f"owns the playlist.")
            return False
        log.info("[setup] ownership verified — this token can write that playlist")
        if not confirm(f"Add every upload to {owner['title']!r} on "
                       f"{fmt_channel(ch)}?", default_no=False):
            log.info("[setup] nothing written")
            return False

    ys._vault_write_token(creds.to_json(), token_file)
    log.info(f"[setup] {token_file} installed in the vault")
    return True


# ── vault reachability ────────────────────────────────────────────────────────

def require_vault() -> bool:
    """Fail fast and unambiguously when the vault passphrase is unreachable.

    Worth its own check because _vault_read_token() swallows the Keychain error
    and returns None — which would otherwise read as "no token stored" when the
    real problem is that this shell can't unlock the login Keychain at all (the
    classic SSH-session symptom: rc=36)."""
    try:
        ys._vault_get_password()
        return True
    except Exception as e:
        log.error(f"[setup] cannot read the vault passphrase from the login "
                  f"Keychain: {e}")
        log.error("[setup] an SSH shell CANNOT unlock the login Keychain — run "
                  "this from the Mac's GUI session, or from anywhere with: "
                  "./run_auth_setup.sh")
        return False


# ── --show ────────────────────────────────────────────────────────────────────

def show() -> int:
    """Report which channel each stored token is bound to. Read-only: no
    consent, no writes — this is the 'which is which?' command."""
    if not require_vault():
        return 2
    rc = 0
    seen: dict[str, str] = {}
    for kind, token_file in ((UPLOAD, ys.YT_VAULT_TOKEN_FILE),
                             (PLAYLIST, ys.YT_VAULT_PLAYLIST_TOKEN_FILE)):
        token_json = ys._vault_read_token(token_file)
        if not token_json:
            log.warning(f"[setup] {kind:<8} ({token_file}): NOT PRESENT in the vault")
            if kind == PLAYLIST and not ys.YT_PLAYLIST_ID:
                log.info("[setup]          (YT_PLAYLIST_ID unset — playlist step "
                         "is disabled, so this is expected)")
            else:
                rc = 1
            continue
        try:
            creds = Credentials.from_authorized_user_info(json.loads(token_json),
                                                          ys.YT_SCOPES)
            youtube = build("youtube", "v3", credentials=creds)
            ch = describe_channel(youtube)
        except Exception as e:
            log.error(f"[setup] {kind:<8} ({token_file}): UNUSABLE — {e!r}")
            rc = 1
            continue
        log.info(f"[setup] {kind:<8} ({token_file}): {fmt_channel(ch)}")
        if ch:
            seen[kind] = ch["id"]
        if kind == PLAYLIST and ys.YT_PLAYLIST_ID:
            owner = playlist_owner(youtube, ys.YT_PLAYLIST_ID)
            if owner is None:
                log.error(f"[setup]          playlist {ys.YT_PLAYLIST_ID} not "
                          f"visible to this token")
                rc = 1
            elif ch and owner["channelId"] != ch["id"]:
                log.error(f"[setup]          playlist {ys.YT_PLAYLIST_ID} is owned "
                          f"by {owner['channelId']} — this token CANNOT write it")
                rc = 1
            else:
                log.info(f"[setup]          playlist {ys.YT_PLAYLIST_ID} "
                         f"({owner['title']!r}) — writable")
    if len(seen) == 2 and seen[UPLOAD] == seen[PLAYLIST]:
        log.info("[setup] both tokens are on the SAME channel — fine if that's "
                 "what you want, but then only one was needed")
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--which", choices=[UPLOAD, PLAYLIST, "both"], default="both",
                    help="which token(s) to (re-)consent (default: both)")
    ap.add_argument("--show", action="store_true",
                    help="just report which channel each stored token is on, and exit")
    ap.add_argument("--expect-upload-handle", default="",
                    help="warn unless the upload token lands on this @handle "
                         "(e.g. @PixelArtRobot)")
    ap.add_argument("--playlist-id", default="",
                    help="override YT_PLAYLIST_ID from .env for this run")
    args = ap.parse_args()

    if args.show:
        return show()

    if not require_vault():
        return 2

    playlist_id = (args.playlist_id or ys.YT_PLAYLIST_ID).strip()
    kinds = [UPLOAD, PLAYLIST] if args.which == "both" else [args.which]
    expect = args.expect_upload_handle.strip()

    banner([
        "YouTube OAuth setup — two tokens, two channels",
        "",
        "A token is bound to ONE channel. The upload token decides where the",
        "videos go; the playlist token decides whose playlist we can write.",
        "Nothing is stored until the channel is verified, so a wrong pick is",
        "recoverable — just say no at the confirmation and re-run.",
        "",
        f"Doing: {', '.join(k.upper() for k in kinds)}",
    ])

    installed = []
    for i, kind in enumerate(kinds, start=1):
        step = f"STEP {i} of {len(kinds)} —"
        try:
            if do_token(kind, step, expect if kind == UPLOAD else None, playlist_id):
                installed.append(kind)
        except KeyboardInterrupt:
            log.warning("[setup] interrupted — nothing further written")
            break
        except Exception as e:
            log.error(f"[setup] {kind} token failed: {e!r}", exc_info=True)

    print()
    if installed:
        log.info(f"[setup] installed: {', '.join(installed)}")
        log.info("[setup] verify with:  ./venv/bin/python setup_youtube_auth.py --show")
        log.info("[setup] then restart the daemon:  ./launchagent.sh restart")
    else:
        log.warning("[setup] no tokens were installed")
    return 0 if installed else 1


if __name__ == "__main__":
    sys.exit(main())
