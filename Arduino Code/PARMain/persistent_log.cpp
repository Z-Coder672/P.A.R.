#include "persistent_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <FlashIAPBlockDevice.h>
#include <LittleFileSystem.h>

// Reserve the last 256 KB of the 16 MB QSPI flash for the log file system.
// The Arduino mbed core links sketches near the start of flash, well under
// 2 MB in practice — the upper region is unused. 256 KB is plenty for the
// 64 KB rotation cap below plus LittleFS metadata + erase-block alignment.
#define PLOG_FS_OFFSET   (0xFC0000)  // 16 MB - 256 KB, relative to XIP_BASE
#define PLOG_FS_SIZE     (0x040000)  // 256 KB

#define PLOG_MAX_LINE    96
#define PLOG_MAX_BYTES   (64 * 1024)
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
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) { fclose(f); return; }
  if (sz > PLOG_MAX_BYTES) sz = PLOG_MAX_BYTES;
  s_bootSnapshot = (char*)malloc(sz + 1);
  if (!s_bootSnapshot) { fclose(f); return; }
  size_t got = fread(s_bootSnapshot, 1, sz, f);
  s_bootSnapshot[got] = '\0';
  s_bootSnapshotLen = got;
  fclose(f);
}

static void rotateIfOversized() {
  FILE* f = fopen(PLOG_PATH, "r");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fclose(f);
  if (sz > PLOG_MAX_BYTES) {
    remove(PLOG_PATH);
  }
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
