# YT-Streamer launch + camera permission

The daemon records via AVFoundation, which needs macOS **Camera (TCC)** permission.
A plain `python`/`launchd` process can't hold that grant (it runs as
`org.python.python`, which has no camera usage string and can't show the consent
prompt) — so recordings start but stream **zero frames** (silent empty `.mov`s).
This directory solves that with a signed app-bundle identity the grant attaches to,
launched by a GUI LaunchAgent so it's persistent + controllable from SSH.

## Pieces

- **`PARRecorder.app`** — a tiny signed bundle (`com.par.ytstreamer`) with an
  `NSCameraUsageDescription`. Its launcher (`parrecorder.c`) forks
  `caffeinate -i venv/python watchdog.py` and stays alive as the signed identity,
  so the python daemon (its child) inherits the camera grant as its "responsible"
  process. Built with `build_recorder_app.sh`.
- **`com.par.ytstreamer.plist`** — the LaunchAgent (GUI/`Aqua` session), launches
  `PARRecorder.app`, `RunAtLoad` + `KeepAlive` (survives crashes + reboots).
- **`launchagent.sh`** — install / restart / stop / status / uninstall, all
  SSH-safe (`launchctl … gui/$(id -u)`).
- **`watchdog.py`** — unchanged supervisor (rapid-fail budget) that runs the daemon.

## First-time setup (one-time GUI click, then SSH forever)

```bash
cd ~/P.A.R./YT-Streamer
./launchagent.sh install
```
Then **Screen Share** into the mini (works headless — `vnc://<mini-ip>` from another
Mac's Finder → *Connect to Server*) and click **Allow** on the Camera dialog once.
The grant attaches to `com.par.ytstreamer` and persists across reboots/restarts.

Verify: `./launchagent.sh status` → the last log line should read
`Camera : TCC authorization = 3 (Authorized)`.

## Day-to-day (all from SSH)

```bash
./launchagent.sh restart    # pick up new code (git pull / edits)
./launchagent.sh stop
./launchagent.sh status
```
It auto-starts at login and restarts on crash — no `nohup`, no per-launch clicks.

## Gotchas

- **Rebuilding the app voids the grant.** `build_recorder_app.sh` re-signs → new
  cdhash → the camera grant no longer matches. Re-approve once (restart + Screen
  Share → Allow) after any rebuild. Editing `YT_streamer.py`/`watchdog.py` does NOT
  require a rebuild (they're loaded by the daemon, not part of the signed binary) —
  just `./launchagent.sh restart`.
- **Needs an active GUI login session.** Headless is fine as long as the mini
  auto-logs-in a user (so an `Aqua` session exists for the agent + prompt). If
  nobody is logged in, the agent can't get the camera.
- **Do not launch the daemon directly from SSH** (`python YT_streamer.py` or the
  old `nohup watchdog.py`). That runs as `org.python.python` with no grant →
  silent empty recordings again. Always go through the LaunchAgent.
