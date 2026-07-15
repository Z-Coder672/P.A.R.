#include "persistent_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <FlashIAPBlockDevice.h>
#include <LittleFileSystem.h>

// Reserve the upper 8 MB of the 16 MB QSPI flash for the log file system.
// The Arduino mbed core links sketches near the start of flash, well under
// 2 MB in practice — the upper half is unused. 8 MB holds the 4 MB rotation cap
// PLUS the ~2 MB temp file that coexists with the live log during a rollover
// (see rotateIfOversized()), plus LittleFS metadata + erase-block alignment.
#define PLOG_FS_OFFSET   (0x800000)  // upper 8 MB (16 MB - 8 MB), relative to XIP_BASE
#define PLOG_FS_SIZE     (0x800000)  // 8 MB

#define PLOG_MAX_LINE    96
// Rotation cap (checked at boot): once the log exceeds this, the OLDEST bytes
// are dropped down to PLOG_KEEP_BYTES — a rolling trim, not a wholesale wipe, so
// recent history survives a rollover. Sized to hold many jobs' per-cell scan
// dumps (SCAN_PX_LOG_ALL ≈ 80 KB/job) across the brownout reboots we're chasing.
#define PLOG_MAX_BYTES   (4 * 1024 * 1024)
// After a rollover, how much of the tail (most recent bytes) to retain. Half the
// cap leaves headroom before the next trim so we don't re-trim every boot.
#define PLOG_KEEP_BYTES  (2 * 1024 * 1024)
// Scratch file for the streamed tail copy; also the recovery marker (see begin()).
#define PLOG_TMP_PATH    "/plog/log.tmp"
// The previous-boot snapshot is malloc'd into RP2040 RAM, so cap it small and
// independently of the rotation cap (we can't spare 384 KB of SRAM). We keep
// the TAIL of the prior log, which is the part nearest a crash.
#define PLOG_SNAPSHOT_MAX (24 * 1024)
#define PLOG_PATH        "/plog/log.txt"

static FlashIAPBlockDevice s_bd(XIP_BASE + PLOG_FS_OFFSET, PLOG_FS_SIZE);
static mbed::LittleFileSystem s_fs("plog");
static bool s_mounted = false;

// Buffered snapshot of log.txt as it existed at boot, so we can echo the
// previous session's log to Serial even though we keep appending to the same
// file during this session. Bounded by PLOG_MAX_BYTES.
static char* s_bootSnapshot = nullptr;
static size_t s_bootSnapshotLen = 0;

static void captureBootSnapshot() {
  FILE* f = fopen(PLOG_PATH, "r");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz <= 0) { fclose(f); return; }
  // Keep only the tail (most recent) up to the small RAM-bounded snapshot cap;
  // the full file is still available via dumpAll() (reads flash directly).
  long readsz = sz, startpos = 0;
  if (readsz > PLOG_SNAPSHOT_MAX) { readsz = PLOG_SNAPSHOT_MAX; startpos = sz - readsz; }
  fseek(f, startpos, SEEK_SET);
  s_bootSnapshot = (char*)malloc(readsz + 1);
  if (!s_bootSnapshot) { fclose(f); return; }
  size_t got = fread(s_bootSnapshot, 1, readsz, f);
  s_bootSnapshot[got] = '\0';
  s_bootSnapshotLen = got;
  fclose(f);
}

static void rotateIfOversized() {
  FILE* f = fopen(PLOG_PATH, "r");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz <= PLOG_MAX_BYTES) { fclose(f); return; }

  // Oversized: keep only the most recent PLOG_KEEP_BYTES so history survives a
  // rollover instead of being wiped wholesale. The tail is far too big for
  // RP2040 SRAM, so stream it to a temp file, then swap the temp in.
  fseek(f, sz - PLOG_KEEP_BYTES, SEEK_SET);
  // Skip the partial first line so the trimmed log starts on a line boundary.
  int c;
  while ((c = fgetc(f)) != EOF && c != '\n') { /* discard to newline */ }

  FILE* t = fopen(PLOG_TMP_PATH, "w");
  if (!t) { fclose(f); return; }  // leave the original intact on any failure
  char buf[256];
  size_t got;
  while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
    fwrite(buf, 1, got, t);
  }
  fclose(t);
  fclose(f);

  // Swap: remove the old log, promote the temp. If a brownout hits between
  // these two, begin()'s recovery promotes the leftover temp on next boot.
  remove(PLOG_PATH);
  rename(PLOG_TMP_PATH, PLOG_PATH);
}

void plog::begin() {
  int err = s_fs.mount(&s_bd);
  if (err) {
    // First boot (or corrupted): reformat. This is destructive but the log
    // is by definition non-critical state.
    s_fs.reformat(&s_bd);
    if (s_fs.mount(&s_bd) != 0) return;
  }
  s_mounted = true;
  // Recover from a brownout during rotateIfOversized()'s remove→rename swap: if
  // a trimmed temp exists but the real log is gone, promote the temp. If both
  // exist, the swap never started (or the old log outlived a failed write) —
  // drop the stale temp and keep the original.
  {
    FILE* logf = fopen(PLOG_PATH, "r");
    FILE* tmpf = fopen(PLOG_TMP_PATH, "r");
    if (logf) fclose(logf);
    if (tmpf) fclose(tmpf);
    if (tmpf && !logf) rename(PLOG_TMP_PATH, PLOG_PATH);
    else if (tmpf)     remove(PLOG_TMP_PATH);
  }
  captureBootSnapshot();
  rotateIfOversized();
}

void plog::log(const char* line) {
  if (!s_mounted) return;
  FILE* f = fopen(PLOG_PATH, "a");
  if (!f) return;
  // Stamp every line so we can correlate with reset / timeout boundaries.
  fprintf(f, "%lu %.*s\n", (unsigned long)millis(), PLOG_MAX_LINE, line);
  fclose(f);
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
  FILE* f = fopen(PLOG_PATH, "r");
  if (!f) {
    out.println("(empty)");
    out.println("--- end flash log ---");
    return;
  }
  char buf[128];
  size_t got;
  while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
    out.write((const uint8_t*)buf, got);
  }
  fclose(f);
  out.println();
  out.println("--- end flash log ---");
}

void plog::clear() {
  if (!s_mounted) return;
  remove(PLOG_PATH);
}
