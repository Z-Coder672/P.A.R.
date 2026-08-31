#!/bin/bash
# Wait until YT-Streamer is idle (no recording in flight, no recording awaiting
# upload), then restart the daemon so it picks up new code.
#
# Why not just `launchagent.sh restart`: a restart mid-recording kills the
# AVCaptureSession before stopRecording() finalizes the moov, so the .mov is
# lost and never uploaded. This waits for the current print's upload to land
# (the uploader unlinks the .mov on success, which is the idle signal).
#
# Run detached:
#   nohup ./restart_after_upload.sh </dev/null >/dev/null 2>&1 &
# Watch:  tail -f restart_after_upload.log
# Cancel: kill $(cat restart_after_upload.pid)

set -u
cd "$(dirname "$0")"

REC_DIR="${REC_DIR:-/tmp/recordings}"
SIDECAR="$REC_DIR/latest_frame.jpg"
POLL=5                  # seconds between checks
SIDECAR_STALE=60        # sidecar older than this => not actively recording
IDLE_CONFIRM=3          # consecutive idle polls required before restarting
STRANDED_AFTER=900      # a .mov unchanged this long with no recording => upload
                        # already finished or failed; the daemon won't retry it
LOG="$(pwd)/restart_after_upload.log"

echo $$ > restart_after_upload.pid
trap 'rm -f restart_after_upload.pid' EXIT

log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"; }

log "[watch] started (pid $$); waiting for idle in $REC_DIR"

idle_streak=0
last_state=""
stranded_since=0

while true; do
  movs=$(ls -1 "$REC_DIR"/*.mov 2>/dev/null | wc -l | tr -d ' ')
  now=$(date +%s)

  # The sidecar is written ~1fps by the live recorder and unlinked on stop, so a
  # fresh one is the reliable "recording right now" signal.
  recording=0
  if [ -f "$SIDECAR" ]; then
    [ $(( now - $(stat -f %m "$SIDECAR") )) -lt "$SIDECAR_STALE" ] && recording=1
  fi

  # Newest .mov mtime — an upload in progress leaves the file untouched, so this
  # only tells us how long it has been sitting there.
  newest_mov_age=0
  if [ "$movs" -gt 0 ]; then
    newest=$(ls -t "$REC_DIR"/*.mov 2>/dev/null | head -1)
    newest_mov_age=$(( now - $(stat -f %m "$newest") ))
  fi

  state="movs=$movs recording=$recording"
  [ "$state" != "$last_state" ] && log "[watch] $state"
  last_state="$state"

  idle=0
  if [ "$movs" -eq 0 ] && [ "$recording" -eq 0 ]; then
    idle=1
  elif [ "$recording" -eq 0 ] && [ "$newest_mov_age" -gt "$STRANDED_AFTER" ]; then
    # Upload finished-but-unlinked-failed, or failed outright and the mov was
    # left for manual recovery. Either way nothing more will happen to it.
    [ "$stranded_since" -eq 0 ] && log "[watch] stranded .mov (${newest_mov_age}s idle); treating as done"
    stranded_since=$now
    idle=1
  fi

  if [ "$idle" -eq 1 ]; then
    idle_streak=$((idle_streak + 1))
    if [ "$idle_streak" -ge "$IDLE_CONFIRM" ]; then
      log "[watch] idle confirmed -> restarting daemon"
      ./launchagent.sh restart >> "$LOG" 2>&1
      log "[watch] restart issued; exiting"
      exit 0
    fi
  else
    [ "$idle_streak" -ne 0 ] && log "[watch] busy again, resetting idle streak"
    idle_streak=0
  fi

  sleep "$POLL"
done
