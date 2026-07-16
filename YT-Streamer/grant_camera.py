#!/usr/bin/env python3
"""One-shot camera-permission primer for YT-Streamer.

The daemon records via AVFoundation, which needs macOS Camera (TCC) permission.
A detached daemon can't show the consent prompt, so run THIS interactively — in a
real Terminal.app window on the Mac mini's GUI session (NOT over plain SSH) — to
trigger the system prompt and click Allow. The grant is recorded against this
launch context; start the watchdog from the SAME Terminal afterwards.

    ./venv/bin/python grant_camera.py

Then verify the daemon sees it: its boot banner should read
    Camera : TCC authorization = 3 (Authorized)
"""
import time
import AVFoundation as A
import CoreFoundation as CF

NAMES = {0: "NotDetermined", 1: "Restricted", 2: "Denied", 3: "Authorized"}


def status() -> int:
    return A.AVCaptureDevice.authorizationStatusForMediaType_(A.AVMediaTypeVideo)


def main() -> None:
    s = status()
    print(f"Current camera authorization: {s} ({NAMES.get(s, '?')})")
    if s == 3:
        print("Already Authorized — nothing to do. Start the watchdog from this Terminal.")
        return
    if s in (1, 2):
        print(f"Status is {NAMES[s]} — the prompt won't reappear. Open System Settings > "
              "Privacy & Security > Camera and enable this Terminal (or reset with "
              "`tccutil reset Camera` and re-run this from Terminal.app).")
        return

    print("Requesting access — a system dialog should appear. Click \"Allow\"...")
    done = {"v": None}

    def handler(granted):
        done["v"] = bool(granted)

    A.AVCaptureDevice.requestAccessForMediaType_completionHandler_(A.AVMediaTypeVideo, handler)

    # Spin a runloop up to 90s so the completion handler can fire after you click.
    deadline = time.monotonic() + 90
    while done["v"] is None and time.monotonic() < deadline:
        CF.CFRunLoopRunInMode(CF.kCFRunLoopDefaultMode, 0.25, False)

    final = status()
    print(f"Result: granted={done['v']}  -> authorization now {final} ({NAMES.get(final, '?')})")
    if final == 3:
        print("Success. Now start the watchdog FROM THIS SAME TERMINAL:")
        print('  ( nohup caffeinate -i python3 watchdog.py >> stream_watchdog.log 2>&1 & )')
    else:
        print("Still not Authorized. If no dialog appeared, you're likely on SSH/headless — "
              "run this at the mini's screen or via Screen Sharing.")


if __name__ == "__main__":
    main()
