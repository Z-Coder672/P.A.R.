#!/usr/bin/env python3
"""Collect TCS3200 RGBC samples for blue-vs-black classifier training.

Reads CSV lines (r,g,b,c) from the Arduino over serial and stores them
into one of two labeled buckets ("blue" or "black") in a JSON file.

Keys (global hotkeys via pynput):
  b  -> start/resume recording into the BLUE bucket
  k  -> start/resume recording into the BLACK bucket
  p  -> manual pause (resume by pressing b or k)

Auto-pauses after 10,000 samples have been recorded in the current
labeling session (the counter resets each time you press b or k).

Usage:
  ./venv/bin/python collect.py [--port /dev/cu.usbmodem101] [--out data.json]
"""

import argparse
import glob
import json
import os
import shutil
import sys
import threading
import time

import serial
from pynput import keyboard

AUTO_PAUSE_LIMIT = 10_000
SAVE_INTERVAL_SEC = 5.0
BAUD = 115200


def autodetect_port():
    candidates = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*"))
    return candidates[0] if candidates else None


class Collector:
    def __init__(self, port, out_path):
        self.port = port
        self.out_path = out_path
        self.lock = threading.Lock()
        self.stop_flag = False

        # State machine: 'idle' -> ignore samples until first key.
        # 'blue' / 'black' -> record into matching bucket.
        # 'paused_manual' / 'paused_auto' -> ignore until b or k.
        self.state = "idle"
        self.session_count = 0  # samples since last b/k press
        self.dirty = False  # something to save?

        self.data = self._load()

    def _load(self):
        if os.path.exists(self.out_path):
            try:
                with open(self.out_path) as f:
                    d = json.load(f)
                if isinstance(d, dict) and "blue" in d and "black" in d:
                    return d
            except (json.JSONDecodeError, OSError) as e:
                print(f"warn: could not load {self.out_path}: {e}; starting fresh")
        return {"blue": [], "black": []}

    def save(self):
        with self.lock:
            if not self.dirty:
                return
            tmp = self.out_path + ".tmp"
            with open(tmp, "w") as f:
                json.dump(self.data, f)
            os.replace(tmp, self.out_path)
            self.dirty = False

    def on_press(self, key):
        try:
            ch = key.char
        except AttributeError:
            return
        if ch is None:
            return
        ch = ch.lower()
        if ch == "b":
            self.state = "blue"
            self.session_count = 0
            print(f"[REC blue]   total={len(self.data['blue'])}", flush=True)
        elif ch == "k":
            self.state = "black"
            self.session_count = 0
            print(f"[REC black]  total={len(self.data['black'])}", flush=True)
        elif ch == "p":
            if self.state in ("blue", "black"):
                self.state = "paused_manual"
                print("[paused]    press b or k to resume", flush=True)

    def feed_sample(self, rgbc):
        st = self.state
        if st == "blue" or st == "black":
            with self.lock:
                self.data[st].append(rgbc)
                self.dirty = True
            self.session_count += 1
            if self.session_count >= AUTO_PAUSE_LIMIT:
                color = st
                self.state = "paused_auto"
                print(
                    f"[auto-pause] {AUTO_PAUSE_LIMIT} {color} samples collected this session "
                    f"(total {color}={len(self.data[color])}); press b or k to continue",
                    flush=True,
                )

    def serial_loop(self):
        try:
            ser = serial.Serial(self.port, BAUD, timeout=1.0)
        except serial.SerialException as e:
            print(f"error: could not open {self.port}: {e}", file=sys.stderr)
            self.stop_flag = True
            return
        # Let the Arduino reset complete and discard any boot noise.
        time.sleep(2.0)
        ser.reset_input_buffer()
        while not self.stop_flag:
            try:
                raw = ser.readline()
            except serial.SerialException as e:
                print(f"serial error: {e}", file=sys.stderr)
                break
            if not raw:
                continue
            try:
                line = raw.decode("ascii", errors="ignore").strip()
            except UnicodeDecodeError:
                continue
            if not line:
                continue
            parts = line.split(",")
            if len(parts) != 4:
                continue
            try:
                rgbc = [int(p) for p in parts]
            except ValueError:
                continue
            self.feed_sample(rgbc)
        ser.close()

    def saver_loop(self):
        while not self.stop_flag:
            time.sleep(SAVE_INTERVAL_SEC)
            try:
                self.save()
            except OSError as e:
                print(f"save error: {e}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="serial port (auto-detect if omitted)")
    ap.add_argument("--out", default="color_data.json", help="output JSON file")
    args = ap.parse_args()

    port = args.port or autodetect_port()
    if port is None:
        print("error: no /dev/cu.usbmodem* found and no --port given", file=sys.stderr)
        sys.exit(1)

    out_path = os.path.abspath(args.out)
    c = Collector(port, out_path)
    print(f"port={port}  out={out_path}")
    print(
        f"loaded: blue={len(c.data['blue'])}  black={len(c.data['black'])}",
        flush=True,
    )
    print("keys: b=record blue   k=record black   p=pause   ctrl-c=quit")
    print("starting in IDLE; press b or k when ready", flush=True)

    t_ser = threading.Thread(target=c.serial_loop, daemon=True)
    t_save = threading.Thread(target=c.saver_loop, daemon=True)
    t_ser.start()
    t_save.start()

    listener = keyboard.Listener(on_press=c.on_press)
    listener.start()

    try:
        while not c.stop_flag:
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        c.stop_flag = True
        listener.stop()
        c.save()
        print(
            f"\nfinal: blue={len(c.data['blue'])}  black={len(c.data['black'])}  saved -> {out_path}"
        )


if __name__ == "__main__":
    main()
