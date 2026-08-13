// One-off sketch: mounts the same LittleFS partition PARMain's plog uses and
// dumps /log.txt to USB Serial. Flash this, open the serial monitor at
// 115200, read your log, then flash PARMain back.
//
// NEVER formats — if the mount fails, we bail. Formatting would erase the very
// data we're trying to recover. (LittleFS.begin(false): formatOnFail is off.)
//
// PORT (Arduino Nano ESP32): the RP2040 build carved its own block device out
// of raw QSPI flash (FlashIAPBlockDevice at XIP_BASE + 0x800000, 8 MB, mounted
// as "plog") because the mbed core has no partition table. The ESP32 has one,
// so the FS is a real partition. Two things must match persistent_log.cpp:
//   partition label = "ffat"     (NOT LittleFS.begin()'s "spiffs" default)
//   log path        = /log.txt   (relative to the LittleFS root)
//
// !! Why "ffat" and not "spiffs" — do not re-litigate. This board is flashed
// !! over DFU, and that recipe writes ONLY the app image:
// !!     dfu-util --device {vid}:{pid} -D {sketch}.bin -Q
// !! It never writes partitions.bin. So PartitionScheme=spiffs changes what the
// !! BUILD assumes while the chip keeps its factory table, whose data partition
// !! is "ffat" — LittleFS finds no "spiffs" partition and cannot mount. We
// !! therefore mount the partition that exists. Build with NO PartitionScheme
// !! flag:  arduino-cli compile --fqbn arduino:esp32:nano_nora
//
// !! Re-flashing does NOT erase the data partition — neither DFU (app image
// !! only) nor `esptool write_flash` of the app touches it — so swapping
// !! between PARMain and PlogDump preserves the log, exactly as before.

#include <Arduino.h>
#include <LittleFS.h>

#define PLOG_PATH "/log.txt"
// MUST match persistent_log.cpp's PLOG_PARTITION_LABEL.
#define PLOG_PARTITION_LABEL "ffat"

static void dumpLog() {
  Serial.println("--- begin flash log ---");
  File f = LittleFS.open(PLOG_PATH, "r");
  if (!f) {
    Serial.println("(log file not found)");
    Serial.println("--- end flash log ---");
    return;
  }
  uint8_t buf[128];
  size_t got;
  while ((got = f.read(buf, sizeof(buf))) > 0) {
    Serial.write(buf, got);
  }
  f.close();
  Serial.println();
  Serial.println("--- end flash log ---");
}

void setup() {
  Serial.begin(115200);
  // Wait for a host monitor, but BOUNDED. The RP2040 build spun here forever
  // (`while (!Serial) {}`) on the theory that we always want a monitor open.
  // On the ESP32-S3 that is a trap: Serial is native USB CDC, so `!Serial`
  // stays true until the host raises DTR — and a plain reader on /dev/cu.*
  // never raises it (that is precisely what separates cu.* from tty.*). The
  // sketch would park here and emit nothing, which looks identical to a failed
  // mount or a lost log. Time out and dump anyway; a scripted capture that
  // missed the first lines can still hit Enter for a re-dump.
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 10000) { delay(10); }
  delay(500);  // let the host settle after enumeration so line 1 isn't clipped

  Serial.println();
  Serial.println("PlogDump — mounting flash log filesystem...");

  // formatOnFail = false. This is the whole point of the sketch: a failed mount
  // must NOT be "fixed" by wiping the partition we came here to read.
  if (!LittleFS.begin(false, "/littlefs", 10, PLOG_PARTITION_LABEL)) {
    Serial.println("Mount failed. Refusing to format — flash data may be intact");
    Serial.println("but the filesystem is unreadable from here.");
    Serial.println("Either nothing has ever mounted/formatted \"" PLOG_PARTITION_LABEL "\","
                   " or the FS is corrupt. Halting.");
    while (true) delay(1000);
  }
  Serial.print("Mounted \"" PLOG_PARTITION_LABEL "\" — ");
  Serial.print((unsigned long)LittleFS.totalBytes());
  Serial.print(" bytes total, ");
  Serial.print((unsigned long)LittleFS.usedBytes());
  Serial.println(" used");

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
