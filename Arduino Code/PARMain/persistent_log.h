// Tiny append-only log persisted to the ESP32-S3's LittleFS data partition.
// (Was a hand-carved slice of the RP2040's QSPI flash via mbed LittleFS; the
// ESP32 has a real partition table, so we mount the factory "ffat" partition —
// 9.375 MB. Build normally, with NO PartitionScheme flag; see the long comment
// in persistent_log.cpp for why "spiffs" is unreachable over DFU.)
// Use ONLY for connection-layer events (WiFi associate / drop,
// HTTPS connect/status/timeout, poll outcomes, /complete.php retries) — never
// for GRBL chatter or general status. The whole point is that across an MCU
// reset (e.g. the WiFi/GRBL stall watchdog firing) we can read back what was
// happening *just before* the reset.
//
// All entries are millis()-stamped at log time. The ring is bounded by
// rotation at boot: if log.txt is larger than PLOG_MAX_BYTES, the OLDEST bytes
// are trimmed away down to PLOG_KEEP_BYTES (a rolling trim, not a wipe — recent
// history survives), which stops the file from growing without bound across
// years of uptime.
#pragma once
#include <Arduino.h>

// Live mirror of every logged line to USB serial (see plog::log()).
//   0 = flash only  — PARMain: headless production firmware, no USB CDC work.
//   1 = mirror on   — the bench sketches, where you watch a run live.
// THIS IS THE ONLY LINE THAT DIFFERS between the copies of this file. Keep the
// rest byte-identical across PARMain / FlipAllTest / FlipAllMaskedTest /
// ScanColorLidarTest / ScanColorAmbientTest.
#define PLOG_SERIAL_MIRROR 0

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
