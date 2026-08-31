#include "persistent_log.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <LittleFS.h>

// ---------------------------------------------------------------------------
// ESP32-S3 (Arduino Nano ESP32 / NORA-W106) backing store.
//
// On the RP2040 this file carved its own 8 MB slice out of the raw QSPI flash
// (FlashIAPBlockDevice at XIP_BASE + 0x800000) because the mbed core has no
// partition table. The ESP32 does, so the FS size is governed by the partition
// table the sketch is BUILT with, not by constants here.
//
// WHICH PARTITION, AND WHY NOT "spiffs" — do not re-litigate.
// LittleFS.begin() defaults to the partition LABELLED "spiffs". That default is
// unreachable on this board: the Nano ESP32 is flashed over DFU, and the upload
// recipe is
//     dfu-util --device {vid}:{pid} -D {sketch}.bin -Q
// which writes ONLY the application image. It never writes partitions.bin or
// the bootloader. So selecting PartitionScheme=spiffs changes what the BUILD
// assumes while the chip keeps its factory partition table — whose data
// partition is "ffat" — and LittleFS then finds no "spiffs" partition, fails to
// mount, and plog silently no-ops. That cost us a full FlipAllTest run whose
// log was never written (the serial mirror made it look healthy; see log()).
//
// Fix: mount LittleFS on the partition that actually exists. Same offset, same
// size — only the label differs:
//     ffat, data, fat, 0x610000, 0x960000  = 9,830,400 bytes (9.375 MB)
// The name is a leftover from the factory table; the subtype is irrelevant
// because esp_littlefs matches on label. We format it as LittleFS on first
// mount (nothing else on this rig uses it). Build normally — NO PartitionScheme
// flag:
//     arduino-cli compile --fqbn arduino:esp32:nano_nora
//
// That is MORE room than the RP2040 slice had, so the rotation sizing below is
// carried over unchanged: 4 MB live log + a ~2 MB temp file coexisting during a
// rollover = ~6 MB peak, comfortably inside 9.375 MB with room for LittleFS
// metadata and block alignment.
// ---------------------------------------------------------------------------

// Factory data partition (see above). Not "spiffs" — that one does not exist.
#define PLOG_PARTITION_LABEL "ffat"

#define PLOG_MAX_LINE    96
// Rotation cap (checked at boot): once the log exceeds this, the OLDEST bytes
// are dropped down to PLOG_KEEP_BYTES — a rolling trim, not a wholesale wipe, so
// recent history survives a rollover. Sized to hold many jobs' per-cell scan
// dumps (SCAN_PX_LOG_ALL ≈ 80 KB/job) across the brownout reboots we're chasing.
#define PLOG_MAX_BYTES   (4 * 1024 * 1024)
// After a rollover, how much of the tail (most recent bytes) to retain. Half the
// cap leaves headroom before the next trim so we don't re-trim every boot.
#define PLOG_KEEP_BYTES  (2 * 1024 * 1024)
// Paths are relative to the LittleFS root (the Arduino File API resolves them
// against the mount base internally), so no "/plog" prefix here.
// The scratch file is also the recovery marker — see begin().
#define PLOG_TMP_PATH    "/log.tmp"
#define PLOG_PATH        "/log.txt"

// Previous-boot snapshot cap. The RP2040 had to keep this tiny (24 KB) because
// it came out of 264 KB of SRAM. The Nano ESP32 carries 8 MB of OPI PSRAM, so
// when PSRAM is present we allocate the snapshot there and keep 256 KB — enough
// that a full per-cell scan dump (~80 KB/job) plus the surrounding boot/WiFi
// lines survive into printBootDump(). If PSRAM is missing or ps_malloc() fails
// we fall back to the old 24 KB heap allocation, so the behaviour degrades
// rather than breaking. Either way we keep the TAIL of the prior log, which is
// the part nearest a crash.
#define PLOG_SNAPSHOT_MAX_PSRAM (256 * 1024)
#define PLOG_SNAPSHOT_MAX_HEAP  (24 * 1024)

static bool s_mounted = false;

// Buffered snapshot of log.txt as it existed at boot, so we can echo the
// previous session's log to Serial even though we keep appending to the same
// file during this session.
static char* s_bootSnapshot = nullptr;
static size_t s_bootSnapshotLen = 0;

static void captureBootSnapshot() {
  File f = LittleFS.open(PLOG_PATH, "r");
  if (!f) return;
  size_t sz = f.size();
  if (sz == 0) { f.close(); return; }

  // Keep only the tail (most recent) up to the RAM-bounded snapshot cap; the
  // full file is still available via dumpAll() (streams from flash).
  size_t readsz = sz;
  char* buf = nullptr;
#if defined(BOARD_HAS_PSRAM)
  if (psramFound()) {
    if (readsz > PLOG_SNAPSHOT_MAX_PSRAM) readsz = PLOG_SNAPSHOT_MAX_PSRAM;
    buf = (char*)ps_malloc(readsz + 1);
  }
#endif
  if (!buf) {  // no PSRAM, or the PSRAM allocation failed — fall back to heap
    if (readsz > PLOG_SNAPSHOT_MAX_HEAP) readsz = PLOG_SNAPSHOT_MAX_HEAP;
    buf = (char*)malloc(readsz + 1);
  }
  if (!buf) { f.close(); return; }

  if (readsz < sz) f.seek(sz - readsz);
  size_t got = f.read((uint8_t*)buf, readsz);
  buf[got] = '\0';
  s_bootSnapshot = buf;
  s_bootSnapshotLen = got;
  f.close();
}

static void rotateIfOversized() {
  File f = LittleFS.open(PLOG_PATH, "r");
  if (!f) return;
  size_t sz = f.size();
  if (sz <= PLOG_MAX_BYTES) { f.close(); return; }

  // Oversized: keep only the most recent PLOG_KEEP_BYTES so history survives a
  // rollover instead of being wiped wholesale. The tail is far too big to buffer
  // in RAM, so stream it to a temp file, then swap the temp in.
  f.seek(sz - PLOG_KEEP_BYTES);
  // Skip the partial first line so the trimmed log starts on a line boundary.
  int c;
  while ((c = f.read()) >= 0 && c != '\n') { /* discard to newline */ }

  File t = LittleFS.open(PLOG_TMP_PATH, "w");
  if (!t) { f.close(); return; }  // leave the original intact on any failure
  uint8_t buf[256];
  size_t got;
  while ((got = f.read(buf, sizeof(buf))) > 0) {
    t.write(buf, got);
  }
  t.close();
  f.close();

  // Swap: remove the old log, promote the temp. If a brownout hits between
  // these two, begin()'s recovery promotes the leftover temp on next boot.
  LittleFS.remove(PLOG_PATH);
  LittleFS.rename(PLOG_TMP_PATH, PLOG_PATH);
}

void plog::begin() {
  // formatOnFail=true: first boot (or a corrupted FS) reformats. Destructive,
  // but the log is by definition non-critical state — same policy as the old
  // mbed s_fs.reformat() fallback. A false return here means the "spiffs"
  // partition is missing entirely (wrong PartitionScheme — see the header
  // comment); plog then silently no-ops rather than blocking the rig.
  if (!LittleFS.begin(true, "/littlefs", 10, PLOG_PARTITION_LABEL)) {
#if PLOG_SERIAL_MIRROR
    if (Serial) Serial.println("plog: MOUNT FAILED — NOT logging to flash");
#endif
    return;
  }
  s_mounted = true;
#if PLOG_SERIAL_MIRROR
  if (Serial) {
    Serial.print("plog: mounted \"" PLOG_PARTITION_LABEL "\" ");
    Serial.print((unsigned long)LittleFS.totalBytes());
    Serial.print(" bytes total, ");
    Serial.print((unsigned long)LittleFS.usedBytes());
    Serial.println(" used");
  }
#endif

  // Recover from a brownout during rotateIfOversized()'s remove→rename swap: if
  // a trimmed temp exists but the real log is gone, promote the temp. If both
  // exist, the swap never started (or the old log outlived a failed write) —
  // drop the stale temp and keep the original.
  {
    bool haveLog = LittleFS.exists(PLOG_PATH);
    bool haveTmp = LittleFS.exists(PLOG_TMP_PATH);
    if (haveTmp && !haveLog) LittleFS.rename(PLOG_TMP_PATH, PLOG_PATH);
    else if (haveTmp)        LittleFS.remove(PLOG_TMP_PATH);
  }
  captureBootSnapshot();
  rotateIfOversized();
}

void plog::log(const char* line) {
  // Optional live mirror to USB serial, in the same "<millis> <line>" form the
  // flash log stores, so a serial capture and a PlogDump agree.
  //
  // OFF in PARMain (PLOG_SERIAL_MIRROR 0 in its persistent_log.h), ON in the
  // bench sketches. Two reasons it is compiled out rather than left running:
  // PARMain is the headless production firmware and should not spend time in
  // USB CDC, and — more importantly — this mirror USED to print before the
  // mount check below. That made a dead filesystem look perfectly healthy: a
  // whole FlipAllTest run streamed plausible "<millis> <text>" lines to serial
  // while nothing whatsoever reached flash. Whenever the mirror is enabled,
  // begin() now announces the mount result, so the two can never be confused
  // again.
  //
  // `if (Serial)` guards it: on the ESP32-S3 `Serial` is USB CDC, whose
  // operator bool() reports whether a host has the port open, so a headless rig
  // produces no output and never stalls waiting for one to drain.
#if PLOG_SERIAL_MIRROR
  if (Serial) {
    Serial.print((unsigned long)millis());
    Serial.print(' ');
    Serial.println(line);
  }
#endif

  if (!s_mounted) return;
  File f = LittleFS.open(PLOG_PATH, "a");
  if (!f) return;
  // Stamp every line so we can correlate with reset / timeout boundaries.
  f.printf("%lu %.*s\n", (unsigned long)millis(), PLOG_MAX_LINE, line);
  f.close();
}

void plog::logf(const char* fmt, ...) {
  char buf[PLOG_MAX_LINE + 1];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  plog::log(buf);
}

void plog::printBootDump(Print& out) {
  out.println("--- begin previous-boot log ---");
  if (s_bootSnapshot && s_bootSnapshotLen > 0) {
    out.write((const uint8_t*)s_bootSnapshot, s_bootSnapshotLen);
    if (s_bootSnapshot[s_bootSnapshotLen - 1] != '\n') out.println();
  } else {
    out.println("(empty)");
  }
  out.println("--- end previous-boot log ---");
}

void plog::dumpAll(Print& out) {
  out.println("--- begin flash log ---");
  if (!s_mounted) {
    out.println("(log fs not mounted)");
    out.println("--- end flash log ---");
    return;
  }
  File f = LittleFS.open(PLOG_PATH, "r");
  if (!f) {
    out.println("(empty)");
    out.println("--- end flash log ---");
    return;
  }
  uint8_t buf[128];
  size_t got;
  while ((got = f.read(buf, sizeof(buf))) > 0) {
    out.write(buf, got);
  }
  f.close();
  out.println();
  out.println("--- end flash log ---");
}

void plog::clear() {
  if (!s_mounted) return;
  LittleFS.remove(PLOG_PATH);
}
