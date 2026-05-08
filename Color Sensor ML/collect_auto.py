#!/usr/bin/env python3
"""Collect labeled TCS3200 RGBC samples from CollectColorTrainingData.ino.

The Arduino sweeps a known-pattern board and prints lines of the form:
  b,r,g,b,c   -> blue sample
  k,r,g,b,c   -> black sample
  DONE        -> sweep finished
  # ...       -> diagnostic, ignored

Samples are appended to color_data.json (same schema as collect.py:
  {"blue": [[r,g,b,c], ...], "black": [[r,g,b,c], ...]}).
Saves periodically and on exit. Press Ctrl-C to quit.

Usage:
  ./venv/bin/python collect_auto.py [--port /dev/cu.usbmodem101] [--out color_data.json]
"""

import argparse
import glob
import json
import os
import sys
import threading
import time

import serial

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
        self.dirty = False
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

    def feed(self, label, rgbc):
        with self.lock:
            self.data[label].append(rgbc)
            self.dirty = True

    def serial_loop(self):
        try:
            ser = serial.Serial(self.port, BAUD, timeout=1.0)
        except serial.SerialException as e:
            print(f"error: could not open {self.port}: {e}", file=sys.stderr)
            self.stop_flag = True
            return
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
            if line.startswith("#"):
                print(line, flush=True)
                continue
            if line == "DONE":
                print("[DONE] arduino finished sweep", flush=True)
                continue
            parts = line.split(",")
            if len(parts) != 5:
                continue
            tag = parts[0].strip().lower()
            if tag == "b":
                label = "blue"
            elif tag == "k":
                label = "black"
            else:
                continue
            try:
                rgbc = [int(p) for p in parts[1:]]
            except ValueError:
                continue
            self.feed(label, rgbc)
            n_blue = len(self.data["blue"])
            n_black = len(self.data["black"])
            total = n_blue + n_black
            if total % 100 == 0:
                print(f"  blue={n_blue}  black={n_black}", flush=True)
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
    print(f"loaded: blue={len(c.data['blue'])}  black={len(c.data['black'])}", flush=True)
    print("listening for prefixed samples; ctrl-c to quit")

    t_ser = threading.Thread(target=c.serial_loop, daemon=True)
    t_save = threading.Thread(target=c.saver_loop, daemon=True)
    t_ser.start()
    t_save.start()

    try:
        while not c.stop_flag:
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        c.stop_flag = True
        c.save()
        print(
            f"\nfinal: blue={len(c.data['blue'])}  black={len(c.data['black'])}  saved -> {out_path}"
        )


if __name__ == "__main__":
    main()
