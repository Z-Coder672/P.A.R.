// Tiny append-only log persisted to a slice of the RP2040's QSPI flash via
// mbed LittleFS. Use ONLY for connection-layer events (WiFi associate / drop,
// HTTPS connect/status/timeout, poll outcomes, /complete.php retries) — never
// for GRBL chatter or general status. The whole point is that across an MCU
// reset (e.g. the WiFi/GRBL stall watchdog firing) we can read back what was
// happening *just before* the reset.
//
// All entries are millis()-stamped at log time. The ring is bounded by
// rotation at boot: if log.txt is larger than PLOG_MAX_BYTES, we drop it on
// the floor and start fresh. Crude, but it stops the file from growing
// without bound across years of uptime.
#pragma once
#include <Arduino.h>

namespace plog {
  // Mount LittleFS, format on first boot, rotate if oversized, and capture
  // the previous boot's log into a RAM snapshot for printBootDump(). Must run
  // before any plog::log() call.
  void begin();

  // Append a single line. Truncated to PLOG_MAX_LINE chars. Safe to call
  // before begin() (silently drops).
  void log(const char* line);

  // printf-style variant for convenience at call sites.
  void logf(const char* fmt, ...);

  // Print the previous-boot snapshot captured in begin() to `out`. Safe to
  // call multiple times. Only contains entries written before this boot.
  void printBootDump(Print& out);

  // Re-read log.txt from flash and print everything it currently contains
  // (previous-boot entries + anything appended this boot). Use this for the
  // on-demand `dump` serial command — it's always current.
  void dumpAll(Print& out);

  // Wipe the on-flash log. Mostly useful when manually debugging.
  void clear();
}
