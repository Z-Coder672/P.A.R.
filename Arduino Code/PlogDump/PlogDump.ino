// One-off sketch: mounts the same LittleFS slice PARMain's plog uses and
// dumps /plog/log.txt to USB Serial. Flash this, open the serial monitor at
// 115200, read your log, then flash PARMain back.
//
// NEVER calls reformat() — if the mount fails, we bail. Reformatting would
// erase the very data we're trying to recover.
//
// The flash slice constants here MUST match persistent_log.cpp:
//   PLOG_FS_OFFSET = 0xFC0000
//   PLOG_FS_SIZE   = 0x040000
//   mount name     = "plog"
//   log path       = /plog/log.txt

#include <Arduino.h>
#include <FlashIAPBlockDevice.h>
#include <LittleFileSystem.h>

#define PLOG_FS_OFFSET (0xFC0000)
#define PLOG_FS_SIZE   (0x040000)
#define PLOG_PATH      "/plog/log.txt"

static FlashIAPBlockDevice s_bd(XIP_BASE + PLOG_FS_OFFSET, PLOG_FS_SIZE);
static mbed::LittleFileSystem s_fs("plog");

static void dumpLog() {
  Serial.println("--- begin flash log ---");
  FILE* f = fopen(PLOG_PATH, "r");
  if (!f) {
    Serial.println("(log file not found)");
    Serial.println("--- end flash log ---");
    return;
  }
  uint8_t buf[128];
  size_t got;
  while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
    Serial.write(buf, got);
  }
  fclose(f);
  Serial.println();
  Serial.println("--- end flash log ---");
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {}  // gate on host this time — we WANT a monitor open

  Serial.println();
  Serial.println("PlogDump — mounting flash log filesystem...");

  int err = s_fs.mount(&s_bd);
  if (err) {
    Serial.print("Mount failed (err=");
    Serial.print(err);
    Serial.println("). Refusing to reformat — flash data may be intact but");
    Serial.println("the filesystem is unreadable from here. Halting.");
    while (true) delay(1000);
  }
  Serial.println("Mounted.");

  dumpLog();

  Serial.println();
  Serial.println("Type any line to re-dump, or just unplug.");
}

void loop() {
  // Re-dump whenever the user hits Enter — handy if the first dump scrolled
  // off the monitor and you want it again.
  static bool sawNewline = false;
  while (Serial.available()) {
    if (Serial.read() == '\n') sawNewline = true;
  }
  if (sawNewline) {
    sawNewline = false;
    dumpLog();
  }
  delay(50);
}
