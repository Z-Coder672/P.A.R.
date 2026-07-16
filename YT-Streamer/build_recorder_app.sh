#!/bin/bash
# Build + ad-hoc-sign PARRecorder.app — the signed bundle identity (com.par.ytstreamer)
# that the camera TCC grant attaches to. See parrecorder.c and LAUNCHAGENT.md.
#
# WARNING: re-signing changes the ad-hoc cdhash, which VOIDS the existing camera
# grant. After running this you MUST re-approve the camera prompt once:
#   ./launchagent.sh restart      # daemon re-requests; prompt appears in GUI session
#   # then Screen Share in and click Allow
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$HERE/PARRecorder.app"

clang -O2 -o "$APP/Contents/MacOS/parrecorder" "$HERE/parrecorder.c"
echo "[build] compiled launcher"
codesign --force --sign - --identifier com.par.ytstreamer "$APP"
echo "[build] ad-hoc signed com.par.ytstreamer"
codesign -v "$APP" && echo "[build] signature valid"
echo "[build] DONE. NOTE: the camera grant is now void — re-approve once:"
echo "        ./launchagent.sh restart   (then Screen Share → click Allow)"
