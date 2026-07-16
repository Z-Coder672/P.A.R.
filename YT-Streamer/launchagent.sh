#!/bin/bash
# Install / control the P.A.R. YT-Streamer LaunchAgent. Safe to run over SSH as
# the 'admin' user — launchctl targets the GUI (Aqua) session domain gui/<uid>, so
# the daemon runs where the camera works even though you invoked it from SSH.
#
# Usage:
#   ./launchagent.sh install     # copy plist, retire old nohup watchdog, load agent
#   ./launchagent.sh restart     # restart (picks up new code)
#   ./launchagent.sh stop        # unload
#   ./launchagent.sh status      # is it loaded? + last camera-auth log line
#   ./launchagent.sh uninstall   # unload + remove the installed plist
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LABEL="com.par.ytstreamer"
SRC_PLIST="$HERE/$LABEL.plist"
DST_PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
UID_NUM="$(id -u)"
DOMAIN="gui/$UID_NUM"

cmd="${1:-status}"

case "$cmd" in
  install)
    mkdir -p "$HOME/Library/LaunchAgents"
    cp "$SRC_PLIST" "$DST_PLIST"
    echo "[install] copied plist -> $DST_PLIST"
    # Bootout FIRST (cleanly stops it, incl. KeepAlive) before touching processes,
    # then wait until the label is really gone — bootstrapping while it's still
    # registered fails with "Bootstrap failed: 5: Input/output error".
    launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      launchctl print "$DOMAIN/$LABEL" >/dev/null 2>&1 || break
      sleep 0.5
    done
    # Kill any legacy nohup watchdog + stray app so nothing double-runs.
    pkill -f "watchdog.py" 2>/dev/null || true
    pkill -f "PARRecorder.app" 2>/dev/null || true
    sleep 1
    launchctl bootstrap "$DOMAIN" "$DST_PLIST"
    launchctl enable "$DOMAIN/$LABEL" 2>/dev/null || true
    echo "[install] bootstrapped $LABEL into $DOMAIN"
    echo "[install] NOTE: first run will pop the Camera consent dialog in the GUI"
    echo "          session — connect via Screen Sharing and click Allow (one time)."
    ;;
  restart)
    launchctl kickstart -k "$DOMAIN/$LABEL"
    echo "[restart] kickstarted $LABEL"
    ;;
  stop)
    launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
    echo "[stop] unloaded $LABEL"
    ;;
  uninstall)
    launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
    rm -f "$DST_PLIST"
    echo "[uninstall] unloaded and removed $DST_PLIST"
    ;;
  status)
    if launchctl print "$DOMAIN/$LABEL" >/dev/null 2>&1; then
      echo "[status] $LABEL is LOADED in $DOMAIN"
      launchctl print "$DOMAIN/$LABEL" | grep -E "state =|pid =" || true
    else
      echo "[status] $LABEL is NOT loaded"
    fi
    echo "[status] last camera-auth log line:"
    grep "TCC authorization" "$HERE/stream.log" 2>/dev/null | tail -1 || echo "  (none yet)"
    ;;
  *)
    echo "usage: $0 {install|restart|stop|uninstall|status}" >&2
    exit 2
    ;;
esac
