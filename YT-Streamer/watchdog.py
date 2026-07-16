#!/usr/bin/env python3
"""External process supervisor for the P.A.R. YT-Streamer daemon.

Run THIS instead of launching the daemon directly — e.g. on the Mac mini,
detached so it survives the SSH session dropping:

    cd YT-Streamer
    nohup caffeinate -i python3 watchdog.py >> stream_watchdog.log 2>&1 &
    disown

It spawns ``YT_streamer.py`` as a child (using the repo's own venv interpreter,
regardless of how the watchdog itself was launched), waits for it to exit, and
relaunches it. This is the missing half of the daemon's own ``record_watchdog``:
that in-process watchdog converts a wedged AVFoundation stop() into an
``os._exit(1)`` precisely so an *external* supervisor can bring the daemon back
up — but no such supervisor existed (no launchd KeepAlive, no wrapper). This is
it.

Scope — PROCESS DEATH ONLY. The daemon writes no heartbeat, so this supervisor
cannot detect a *silent* freeze (a thread hung while the process stays alive).
The one known freeze (the AVF graph-teardown deadlock) is already self-converted
to a process exit by ``record_watchdog``, which this catches. Any crash,
unhandled exception, or force-exit is caught too.

Hot-loop guard: a child that dies almost immediately (before MIN_HEALTHY_UPTIME)
is a fast-fail — a bad ``.env``, a locked Keychain / unmountable vault, a syntax
error — where blind relaunching just spins. Such deaths burn a restart budget
(MAX_RAPID_RESTARTS) and, once exhausted, the supervisor gives up and exits
loudly rather than pinning the CPU. A child that ran healthily past
MIN_HEALTHY_UPTIME forgives the budget, so a daemon that dies once after days of
uptime always gets restarted.

Lifecycle: SIGINT / SIGTERM to the watchdog tears the child down too. This
module imports nothing from ``YT_streamer`` — the daemon is a subprocess.
"""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
DAEMON = HERE / "YT_streamer.py"
# Pin the child to the repo's venv interpreter (has requests/PIL/pyobjc/etc.);
# system python3 does not, and the watchdog itself may be launched with either.
VENV_PYTHON = HERE / "venv" / "bin" / "python"

# A child that lives at least this long is considered a real, healthy run — its
# eventual death is a genuine incident to recover from, and it forgives the
# rapid-restart budget below.
MIN_HEALTHY_UPTIME = 120.0        # seconds

# Consecutive fast-fails (child died before MIN_HEALTHY_UPTIME) tolerated before
# the supervisor gives up. Bounds a hot-loop on an unfixable startup error.
MAX_RAPID_RESTARTS = 5

# Backoff between restarts grows with consecutive fast-fails, capped.
BACKOFF_STEP = 5.0                # seconds per consecutive fast-fail
BACKOFF_CAP = 60.0                # seconds

KILL_GRACE = 10.0                 # SIGTERM → SIGKILL window on shutdown


def _log(msg: str) -> None:
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"{ts} [streamer-watchdog] {msg}", flush=True)


def _spawn() -> subprocess.Popen:
    """Launch the daemon in the repo dir with the venv interpreter.

    Child stdout/stderr are inherited so, when the watchdog is run under
    ``nohup ... >> stream_watchdog.log``, the daemon's console output lands in
    the same file (the daemon also keeps its own ``stream.log`` independently).
    """
    interp = str(VENV_PYTHON) if VENV_PYTHON.exists() else sys.executable
    child = subprocess.Popen([interp, str(DAEMON)], cwd=str(HERE), env=dict(os.environ))
    _log(f"spawned daemon pid={child.pid} ({interp} {DAEMON.name})")
    return child


def _kill(child: subprocess.Popen | None, grace: float = KILL_GRACE) -> None:
    """SIGTERM, then SIGKILL if the child doesn't exit within `grace`."""
    if child is None or child.poll() is not None:
        return
    try:
        child.terminate()
    except Exception:
        pass
    waited = 0.0
    while child.poll() is None and waited < grace:
        time.sleep(0.5)
        waited += 0.5
    if child.poll() is None:
        _log("child did not exit on SIGTERM — sending SIGKILL")
        try:
            child.kill()
        except Exception:
            pass
        try:
            child.wait(timeout=5)
        except Exception:
            pass


def main() -> int:
    if not DAEMON.exists():
        _log(f"FATAL: daemon not found at {DAEMON}")
        return 1

    _log(f"starting (watchdog pid={os.getpid()}, daemon={DAEMON}, "
         f"min_healthy={MIN_HEALTHY_UPTIME:.0f}s max_rapid_restarts={MAX_RAPID_RESTARTS})")

    stop = {"flag": False}

    def _on_sig(signum, _frame):
        _log(f"signal {signum} received — shutting down")
        stop["flag"] = True

    signal.signal(signal.SIGINT, _on_sig)
    signal.signal(signal.SIGTERM, _on_sig)

    rapid_restarts = 0
    child: subprocess.Popen | None = None
    try:
        while not stop["flag"]:
            child = _spawn()
            started = time.monotonic()

            # Wait for the child to exit, polling so signals are serviced promptly.
            while not stop["flag"]:
                try:
                    rc = child.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    continue
                break
            else:
                # stop flag set while child still alive — shutdown path.
                break

            if stop["flag"]:
                break

            uptime = time.monotonic() - started
            if uptime >= MIN_HEALTHY_UPTIME:
                # A real run ended — reset the fast-fail budget and recover.
                if rapid_restarts:
                    _log(f"daemon ran {uptime:.0f}s (healthy) — resetting rapid-restart budget")
                rapid_restarts = 0
                _log(f"daemon exited (code={rc}) after {uptime:.0f}s — restarting")
                backoff = BACKOFF_STEP
            else:
                rapid_restarts += 1
                _log(f"daemon exited (code={rc}) after only {uptime:.0f}s — "
                     f"fast-fail {rapid_restarts}/{MAX_RAPID_RESTARTS}")
                if rapid_restarts >= MAX_RAPID_RESTARTS:
                    _log("CRITICAL: too many fast-fails — the daemon cannot stay up "
                         "(bad .env? locked Keychain / unmountable vault? import error). "
                         "Giving up; fix it and restart the watchdog.")
                    return 1
                backoff = min(BACKOFF_STEP * rapid_restarts, BACKOFF_CAP)

            # Interruptible backoff.
            slept = 0.0
            while slept < backoff and not stop["flag"]:
                time.sleep(0.5)
                slept += 0.5
    finally:
        _log("stopping — terminating child")
        _kill(child)
    return 0


if __name__ == "__main__":
    sys.exit(main())
