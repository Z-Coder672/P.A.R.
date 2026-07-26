#!/usr/bin/env python3
"""
record_clip.py — standalone fixed-duration recorder for the Brio 100 webcam.

Records N minutes of video (NO audio, ever) from the local USB webcam to a .mov
in a chosen directory. Ad-hoc tool, independent of the YT-Streamer daemon: it
needs no .env, no vault, and never touches stream.log or the gallery.

    ./venv/bin/python record_clip.py --minutes 50 --out ~/Documents

It is a distillation of YT_streamer.AVFRecorder and carries the same hard-won
constraints (see CLAUDE.md → YT-Streamer gotchas); if you change one here, check
whether AVFRecorder needs the same change:

  * Native AVCaptureSession (PyObjC), NOT ffmpeg. ffmpeg's avfoundation input can
    only request the cam's *uncompressed* formats, which over USB 2.0 cap 1080p at
    ~5fps. We select the MJPEG-backed '420v' device format for a true 1080p30.
  * The frame duration is pinned to the format's OWN rational (the device
    advertises 1/30.00003 and rejects a hand-built CMTimeMake(1, 30)).
  * The H.264 average bitrate is pinned; AVFoundation's ~24 Mbps default would
    make a 50-min file ~9 GB instead of ~0.9 GB.
  * The main thread MUST service a CFRunLoop while a recording is stoppable —
    stopRecording()/stopRunning() deliver graphWillStop to the main thread with
    waitUntilDone:YES. A parked main thread deadlocks the stop while the capture
    keeps writing. Here the timer and the stop both live on the main thread, and
    SIGINT/SIGTERM only set a flag (a CFRunLoop raises no KeyboardInterrupt), so
    Ctrl+C finalizes the moov instead of corrupting it.
  * Camera TCC: if authorization is not Authorized(3) the session starts but macOS
    streams ZERO frames — a silently empty recording. We check up front, prompt if
    NotDetermined, and additionally gate on frames actually arriving before
    committing to the full duration. Launch this from Terminal.app (which holds a
    camera grant) — a python launched from an ungranted context cannot prompt.
  * One process at a time owns a USB webcam. If the YT-Streamer daemon starts a
    print recording while this runs, one of the two loses the camera.
"""

import argparse
import os
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

try:
    import objc
    import AVFoundation as AVF
    import CoreMedia as CM
    import CoreFoundation as CF
    import libdispatch
    from Foundation import NSObject, NSURL, NSNotificationCenter
except Exception as e:                                            # pragma: no cover
    sys.exit(f"ERROR: PyObjC AVFoundation stack unavailable ({e!r}).\n"
             "Run with ./venv/bin/python (the venv has pyobjc installed).")

AUTH_NAMES = {0: "NotDetermined", 1: "Restricted", 2: "Denied", 3: "Authorized"}
# A healthy session delivers frames within a second or two; anything past this is
# the silent black-session failure.
LIVENESS_TIMEOUT = 15.0


def log(msg: str) -> None:
    line = f"{datetime.now():%Y-%m-%d %H:%M:%S} {msg}"
    print(line, flush=True)
    if _LOG_FH is not None:
        _LOG_FH.write(line + "\n")
        _LOG_FH.flush()


_LOG_FH = None


def fourcc(n: int) -> str:
    return "".join(chr((n >> s) & 0xFF) for s in (24, 16, 8, 0))


def human_size(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024.0


class RecordDelegate(NSObject):
    """AVCaptureFileOutputRecordingDelegate + session-failure observer."""

    def initWithOwner_(self, owner):
        self = objc.super(RecordDelegate, self).init()
        if self is None:
            return None
        self._owner = owner
        return self

    def captureOutput_didFinishRecordingToOutputFileAtURL_fromConnections_error_(
            self, out, url, conns, error):
        self._owner._on_finish(error)

    def sessionRuntimeError_(self, note):
        err = note.userInfo().get(AVF.AVCaptureSessionErrorKey) if note.userInfo() else None
        self._owner._on_failure("runtime error", err)

    def sessionWasInterrupted_(self, note):
        reason = note.userInfo().get(AVF.AVCaptureSessionInterruptionReasonKey) \
            if note.userInfo() is not None else None
        self._owner._on_failure(f"interrupted(reason={reason})", None)


class CountDelegate(NSObject):
    """Counts camera buffers so we can prove frames are flowing and report the
    real average fps at the end (the container's avg_frame_rate is padded and
    lies about a starved capture)."""

    def init(self):
        self = objc.super(CountDelegate, self).init()
        if self is None:
            return None
        self._frames = 0
        return self

    def captureOutput_didOutputSampleBuffer_fromConnection_(self, out, sbuf, conn):
        self._frames += 1


class ClipRecorder:
    def __init__(self, out_path: Path, camera: str, width: int, height: int,
                 fps: int, bitrate_bps: int):
        self.out_path = out_path
        self.camera = camera.strip().lower()
        self.w, self.h, self.fps = width, height, fps
        self.bitrate_bps = bitrate_bps
        self.session = None
        self.movie_out = None
        self.delegate = None
        self.counter = None
        self.queue = None
        self.device_name = None
        self.subtype = None
        self.error = None
        self.started = False
        self.observing = False
        self._finished = threading.Event()

    # — lifecycle ——————————————————————————————————————————————————————————————
    def start(self) -> None:
        dt = AVF.AVCaptureDeviceDiscoverySession.discoverySessionWithDeviceTypes_mediaType_position_(
            [AVF.AVCaptureDeviceTypeExternal,
             AVF.AVCaptureDeviceTypeBuiltInWideAngleCamera],
            AVF.AVMediaTypeVideo, AVF.AVCaptureDevicePositionUnspecified)
        devs = list(dt.devices())
        dev = next((d for d in devs
                    if d.localizedName().lower().startswith(self.camera)), None)
        if dev is None:
            raise RuntimeError(f"no camera matching {self.camera!r} "
                               f"(present: {[d.localizedName() for d in devs]})")
        self.device_name = dev.localizedName()

        fmt = self._pick_format(dev)
        if fmt is None:
            raise RuntimeError(f"{self.device_name!r} has no "
                               f"{self.w}x{self.h}@>={self.fps}fps format")

        ok = dev.lockForConfiguration_(None)
        if not (ok[0] if isinstance(ok, tuple) else ok):
            raise RuntimeError("lockForConfiguration failed (camera busy?)")
        try:
            dev.setActiveFormat_(fmt)
            # The device's own rational — a hand-built 1/fps CMTime is rejected.
            r = sorted(fmt.videoSupportedFrameRateRanges(),
                       key=lambda x: -x.maxFrameRate())[0]
            dev.setActiveVideoMinFrameDuration_(r.minFrameDuration())
            dev.setActiveVideoMaxFrameDuration_(r.minFrameDuration())
        finally:
            dev.unlockForConfiguration()

        session = AVF.AVCaptureSession.alloc().init()
        session.beginConfiguration()
        inp, err = AVF.AVCaptureDeviceInput.deviceInputWithDevice_error_(dev, None)
        if inp is None or not session.canAddInput_(inp):
            raise RuntimeError(f"cannot add camera input: {err}")
        session.addInput_(inp)      # video only — no audio input, ever.

        movie = AVF.AVCaptureMovieFileOutput.alloc().init()
        if not session.canAddOutput_(movie):
            raise RuntimeError("cannot add movie output")
        session.addOutput_(movie)

        # Frame counter (best effort): liveness proof + a truthful fps at the end.
        try:
            data = AVF.AVCaptureVideoDataOutput.alloc().init()
            data.setAlwaysDiscardsLateVideoFrames_(True)
            self.queue = libdispatch.dispatch_queue_create(b"par.clip.count", None)
            self.counter = CountDelegate.alloc().init()
            data.setSampleBufferDelegate_queue_(self.counter, self.queue)
            if session.canAddOutput_(data):
                session.addOutput_(data)
            else:
                self.counter = None
                log("WARNING: frame counter rejected; falling back to file growth")
        except Exception as e:
            self.counter = None
            log(f"WARNING: frame counter setup failed ({e!r}); using file growth")

        session.commitConfiguration()
        self.session, self.movie_out = session, movie

        # Pin the average bitrate — the ~24 Mbps default would be ~9 GB for 50 min.
        # The video connection only exists once the output is on the session.
        try:
            conn = movie.connectionWithMediaType_(AVF.AVMediaTypeVideo)
            if conn is not None:
                movie.setOutputSettings_forConnection_({
                    AVF.AVVideoCodecKey: AVF.AVVideoCodecTypeH264,
                    AVF.AVVideoWidthKey: self.w,
                    AVF.AVVideoHeightKey: self.h,
                    AVF.AVVideoCompressionPropertiesKey: {
                        AVF.AVVideoAverageBitRateKey: self.bitrate_bps,
                        AVF.AVVideoMaxKeyFrameIntervalKey: self.fps,
                    },
                }, conn)
        except Exception as e:
            log(f"WARNING: could not pin bitrate ({e!r}); using AVFoundation default")

        # Observe BEFORE startRunning so a startup failure is caught too.
        self.delegate = RecordDelegate.alloc().initWithOwner_(self)
        nc = NSNotificationCenter.defaultCenter()
        nc.addObserver_selector_name_object_(
            self.delegate, b"sessionRuntimeError:",
            AVF.AVCaptureSessionRuntimeErrorNotification, session)
        nc.addObserver_selector_name_object_(
            self.delegate, b"sessionWasInterrupted:",
            AVF.AVCaptureSessionWasInterruptedNotification, session)
        self.observing = True

        session.startRunning()
        self.out_path.unlink(missing_ok=True)
        movie.startRecordingToOutputFileURL_recordingDelegate_(
            NSURL.fileURLWithPath_(str(self.out_path)), self.delegate)
        self.started = True

    def stop(self, timeout: float = 20.0) -> None:
        """Finalize the moov, then tear the session down. Idempotent. MUST be
        called from a thread servicing a run loop (here: the main thread)."""
        try:
            if self.movie_out is not None and self.started and not self._finished.is_set():
                self.movie_out.stopRecording()
                # Pump the run loop while waiting: the finish callback is
                # delivered through it, and stopRecording's teardown performs on
                # this very thread.
                deadline = time.monotonic() + timeout
                while not self._finished.is_set() and time.monotonic() < deadline:
                    CF.CFRunLoopRunInMode(CF.kCFRunLoopDefaultMode, 0.1, False)
                if not self._finished.is_set():
                    log("WARNING: finish callback timed out; file may be incomplete")
        except Exception as e:
            log(f"WARNING: stop error: {e!r}")
        finally:
            try:
                if self.session is not None and self.session.isRunning():
                    self.session.stopRunning()
            except Exception as e:
                log(f"WARNING: session stop error: {e!r}")
            if self.observing:
                try:
                    NSNotificationCenter.defaultCenter().removeObserver_(self.delegate)
                except Exception:
                    pass
                self.observing = False

    # — state ——————————————————————————————————————————————————————————————————
    def is_running(self) -> bool:
        return not self._finished.is_set()

    def frames(self):
        return None if self.counter is None else int(self.counter._frames)

    def _on_finish(self, error):
        self.error = error
        self._finished.set()

    def _on_failure(self, kind, error):
        self.error = error if error is not None else RuntimeError(f"session {kind}")
        log(f"ERROR: AVCaptureSession {kind}: {error}")
        self._finished.set()

    def _pick_format(self, dev):
        """Wanted size, max fps >= target, preferring '420v' (NV12, MJPEG-backed)
        — the only 1080p format that sustains 30fps over this cam's USB 2.0 link."""
        best = None
        for f in dev.formats():
            desc = f.formatDescription()
            dims = CM.CMVideoFormatDescriptionGetDimensions(desc)
            if dims.width != self.w or dims.height != self.h:
                continue
            maxfps = max((r.maxFrameRate() for r in f.videoSupportedFrameRateRanges()),
                         default=0.0)
            if maxfps + 0.5 < self.fps:
                continue
            sub = fourcc(CM.CMFormatDescriptionGetMediaSubType(desc))
            score = (sub == "420v", maxfps)
            if best is None or score > best[0]:
                best = (score, f, sub)
        if best is None:
            return None
        self.subtype = best[2]
        return best[1]

    def wait_until_streaming(self, timeout: float = LIVENESS_TIMEOUT) -> bool:
        """Prove frames are actually arriving before committing to the full
        duration. Pumps the run loop so delegate callbacks can land."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._finished.is_set():
                return False
            n = self.frames()
            if n is not None:
                if n > 0:
                    return True
            else:
                try:
                    if self.out_path.stat().st_size >= 64 * 1024:
                        return True
                except OSError:
                    pass
            CF.CFRunLoopRunInMode(CF.kCFRunLoopDefaultMode, 0.25, False)
        return False


def ensure_camera_authorized(prompt_wait: float = 90.0) -> bool:
    """Camera TCC gate. Anything but Authorized(3) means the capture starts but
    streams zero frames — a silently empty file. NotDetermined can only prompt
    when the responsible process is a real, promptable bundle (Terminal.app is
    camera-granted on this Mac; a bare detached python is not)."""
    status = AVF.AVCaptureDevice.authorizationStatusForMediaType_(AVF.AVMediaTypeVideo)
    log(f"camera TCC authorization = {status} ({AUTH_NAMES.get(status, '?')})")
    if status == 3:
        return True
    if status in (1, 2):
        log("ERROR: camera access is Denied/Restricted for this process. Grant it in "
            "System Settings → Privacy & Security → Camera for the app that launched "
            "this (e.g. Terminal), then re-run.")
        return False

    log("requesting camera access — click Allow in the prompt on the Mac's screen…")
    granted = threading.Event()
    result = {"ok": False}

    def handler(ok):
        result["ok"] = bool(ok)
        granted.set()

    AVF.AVCaptureDevice.requestAccessForMediaType_completionHandler_(
        AVF.AVMediaTypeVideo, handler)
    deadline = time.monotonic() + prompt_wait
    while not granted.is_set() and time.monotonic() < deadline:
        CF.CFRunLoopRunInMode(CF.kCFRunLoopDefaultMode, 0.25, False)
    if not result["ok"]:
        log("ERROR: camera access not granted (no prompt answered / not promptable). "
            "Run this from Terminal.app on the Mac — recording without the grant "
            "produces a silently EMPTY file.")
        return False
    log("camera access granted")
    return True


def probe(path: Path) -> str | None:
    """Container duration/size via ffprobe, if present. Purely informational."""
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0",
             "-show_entries", "format=duration:stream=width,height,avg_frame_rate",
             "-of", "default=nw=1", str(path)],
            capture_output=True, text=True, timeout=30)
        return out.stdout.strip() or None
    except Exception:
        return None


def main() -> int:
    global _LOG_FH

    ap = argparse.ArgumentParser(description="Record a fixed-length clip from the Brio 100.")
    ap.add_argument("--minutes", type=float, default=50.0, help="duration (default 50)")
    ap.add_argument("--out", default="~/Documents", help="output directory (default ~/Documents)")
    ap.add_argument("--name", default=None, help="output filename (default brio-<timestamp>.mov)")
    ap.add_argument("--camera", default=os.getenv("CAMERA_NAME", "Brio 100"),
                    help="camera name prefix (default 'Brio 100')")
    ap.add_argument("--size", default="1920x1080", help="capture size (default 1920x1080)")
    ap.add_argument("--fps", type=int, default=30, help="frame rate (default 30)")
    ap.add_argument("--bitrate", default="2500k", help="H.264 average bitrate (default 2500k)")
    args = ap.parse_args()

    try:
        w, h = (int(x) for x in args.size.lower().split("x"))
    except Exception:
        print(f"ERROR: bad --size {args.size!r}")
        return 2
    bitrate_bps = int(args.bitrate.rstrip("kK")) * 1000
    duration = args.minutes * 60.0
    if duration <= 0:
        print("ERROR: --minutes must be positive")
        return 2

    out_dir = Path(args.out).expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    out_path = out_dir / (args.name or f"brio-{stamp}.mov")

    log_dir = Path("/tmp/par-record")
    log_dir.mkdir(parents=True, exist_ok=True)
    _LOG_FH = open(log_dir / f"{out_path.stem}.log", "a", buffering=1)

    log(f"=== record_clip: {args.minutes:g} min → {out_path}")
    log(f"log file: {log_dir / (out_path.stem + '.log')}")

    est_mb = bitrate_bps / 8 * duration / 1e6
    free = os.statvfs(out_dir)
    free_mb = free.f_bavail * free.f_frsize / 1e6
    log(f"estimated size ≈ {est_mb:.0f} MB at {args.bitrate}; free space {free_mb/1000:.1f} GB")
    if free_mb < est_mb * 1.5:
        log("ERROR: not enough free space for the estimated recording")
        return 1

    if not ensure_camera_authorized():
        return 1

    rec = ClipRecorder(out_path, args.camera, w, h, args.fps, bitrate_bps)

    # A CFRunLoop raises no KeyboardInterrupt, so translate signals into a flag +
    # a loop kick; the stop then runs on this (main) thread and finalizes the moov.
    stop_evt = threading.Event()

    def on_signal(signum, _frame):
        log(f"signal {signum} — stopping early and finalizing the file")
        stop_evt.set()
        try:
            CF.CFRunLoopStop(CF.CFRunLoopGetMain())
        except Exception:
            pass

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    try:
        rec.start()
    except Exception as e:
        log(f"ERROR: could not start capture: {e!r}")
        log("If the camera is busy, the YT-Streamer daemon may be recording a print "
            "(a USB webcam allows only one process opener).")
        return 1

    log(f"recording from {rec.device_name!r} — format {rec.subtype} {w}x{h}@{args.fps}")

    if not rec.wait_until_streaming():
        log("ERROR: no frames within the liveness window — aborting instead of "
            "writing an empty file. (Camera not authorized for this process, or "
            "another process owns the device.)")
        rec.stop()
        out_path.unlink(missing_ok=True)
        return 1
    log("frames flowing — recording for real")

    t0 = time.monotonic()
    deadline = t0 + duration
    next_report = t0 + 60.0
    reason = "duration reached"

    while True:
        if stop_evt.is_set():
            reason = "interrupted"
            break
        now = time.monotonic()
        if now >= deadline:
            break
        if not rec.is_running():
            reason = f"capture ended early ({rec.error})"
            break
        if now >= next_report:
            next_report += 60.0
            size = out_path.stat().st_size if out_path.exists() else 0
            n = rec.frames()
            fps_now = (n / (now - t0)) if n else 0.0
            log(f"  t+{(now - t0)/60:5.1f} min / {args.minutes:g} — "
                f"{human_size(size)}" + (f", {fps_now:.1f} fps avg" if n else ""))
        CF.CFRunLoopRunInMode(CF.kCFRunLoopDefaultMode, 0.25, False)

    elapsed = time.monotonic() - t0
    log(f"stopping ({reason}) after {elapsed/60:.2f} min")
    rec.stop()

    if not out_path.exists() or out_path.stat().st_size < 64 * 1024:
        log(f"ERROR: {out_path} is missing or empty")
        return 1

    n = rec.frames()
    log(f"saved {out_path} ({human_size(out_path.stat().st_size)})"
        + (f", {n} frames ≈ {n/elapsed:.1f} fps" if n else ""))
    info = probe(out_path)
    if info:
        log("ffprobe: " + " | ".join(info.splitlines()))
    if rec.error:
        log(f"NOTE: capture reported {rec.error}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
