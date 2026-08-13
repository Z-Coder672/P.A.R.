// ScanColorLidarTest — per-cell color (3-flash ambient-subtracted) + VL53L4CD
// distance over a full grid scan
// ---------------------------------------------------------------------------
// Target: the REAL P.A.R. rig — Arduino Nano RP2040 Connect,
//         FQBN arduino:mbed_nano:nanorp2040connect.
//
// Diagnostic / data-collection sketch. It homes the CNC and, for every cell of
// the 37×18 grid, logs BOTH:
//   - one 3-flash LED ambient-subtracted RGBC read (PARMain's production
//     readAmbientSubtracted: AMBIENT_FLASHES off/on flashes, each flash a
//     5-frame-averaged LEDs-OFF read subtracted from a 5-frame-averaged
//     LEDs-ON read), and
//   - one trimmed-mean distance from the VL53L4CD ToF ranger (I2C on A4/A5,
//     Pololu VL53L4CD library, free-running at a 50 ms timing budget). The
//     averaging window starts FRESH once the move to the cell has finished,
//     records for LIDAR_WINDOW_MS (4 s, ~80 samples), then is cleared — no
//     running average carries across cells. The estimator is VL53L4CDTest's
//     trimmed mean: sort, drop the top/bottom 20%, average the rest.
//     The calibration offset is COLOR-DEPENDENT — that's one reason the color
//     scan runs at all: both sensors view the disc's BACK, and the ToF reading
//     lands differently on the two back colors. The same-row color read
//     classifies each cell's back (clear < SCAN_ON_BLUE_MAX → black back).
//     The class is logged on every lidar cell marker.
//
// RAW MODE (LIDAR_APPLY_CALIBRATION 0, the current setting): the firmware
// offsets are NOT applied — every logged distance is uncorrected. Instead the
// pass carries its own references: two fixed CALIBRATION SQUARES off the left
// edge of the board (gantry X = -775.695, i.e. 2 mm off the left travel limit;
// black at Y -336.000, blue 20 mm above at Y -316.000 — all jog-confirmed
// 2026-07-28), sampled with
// the same 4 s trimmed-mean window at three stages — before row 0 ("start"),
// at the top of row 9 ("mid", the existing re-home boundary), and after the
// last cell ("end"). Six cal readings per pass, two colors × three stages, so
// drift across the ~71 min run is measurable and each row can be corrected
// against the nearest reference in time. The compensation target is then
// derived offline as (square reading) + a constant.
// Everything goes over USB serial AND to the flash log (LittleFS, the region
// PlogDump reads back), so a run survives a power-off with no live serial
// capture. There is NO WiFi, NO queue/HTTP, NO display/flipping.
//
// SENSOR OFFSETS: the lidar sits ~6 mm up and ~55 mm right of the color
// sensor. Its X offset is CALIBRATED so the rightmost column (col 36, cell.x
// -31.075) targets exactly the X=0 machine limit, lining the sweep up:
//   color  (-24.005, +8.0)   — PARMain's calibrated SCAN_OFFSET_X/Y
//   lidar  (+31.075, +14.0)
// Physically confirmed centered on the top-right squisk at (X0, Y-0.2) in the
// $131=412 frame. The +14 Y offset relies on the 406→412 $131/Y_TRAVEL bump:
// the top row's lidar target is -14.2 + 14 = -0.2, just inside the envelope.
//
// TRAVERSAL: each squisk is color-scanned first, then distance-scanned, but
// row-wise, not cell-wise — each row gets a color sub-sweep L→R at the color
// Y, then a lidar sub-sweep R→L at the lidar Y (+6 mm). The two sub-sweeps are
// pure-X; the small Y hop between them (and every cross-row leg) is routed
// through moveToYSafe, honoring the pure-Y-only-at-X-soft-limits convention.
// A per-cell color→lidar ordering would need a mid-field 6 mm Y move at every
// cell, which that convention forbids.
//
// The flip arm is parked at SERVO_US_REST for the ENTIRE scan and never
// dropped, so the physical board pattern is left undisturbed (set a known
// pattern and compare the logged samples against it).
//
// The GRBL streaming machinery, the $H-first boot order, the 60 s no-progress
// stall watchdog (esp_restart), moveToYSafe / clampScanY, and the servo
// software-UART are all copied verbatim from ScanColorAmbientTest.ino /
// PARMain.ino.
//
// OUTPUT FORMAT (each line also millis()-stamped in the flash log)
//   Diagnostic / structural lines start with '#'. Sample lines are bare CSV.
//     # ScanColorLidarTest ready            (boot banner)
//     # pass <n> begin
//     # offsets RAW — ...                   (or the applied offsets, if enabled)
//     # cal <stage> <black|blue> cmd=(<x>,<y>)
//                                           (stage = start|mid|end; cmd= the
//                                            commanded gantry coords of the square)
//     <avg>,<n>,<min>,<max>                 (same 4 s trimmed-mean form as a cell,
//                                            ALWAYS raw — a reference must be)
//     # row <y> color
//     # y=<y> x=<x>                         (cell marker)
//     <r>,<g>,<b>,<c>                       (ONE 3-flash ambient-subtracted read)
//     ...37 cells...
//     # row <y> lidar
//     # y=<y> x=<x> back=<black|cyan>       (cell marker, R→L order; back color
//                                            from this row's color sweep → which
//                                            distance offset was applied)
//     <avg>,<n>,<min>,<max>                 (trimmed-mean corrected mm over the
//                                            4 s window, ONE-DECIMAL (e.g. 38.8 —
//                                            avg of int samples in tenths, not
//                                            rounded to whole mm), sample count,
//                                            window min/max (int mm);
//                                            all-timeout window → 0.0,0,0,0)
//     ...37 cells...
//     # cal end black ... / # cal end blue ...
//     # pass <n> end
//   Join color and distance offline on (y,x); the `# row` markers disambiguate.
//   Every line above is written to the FLASH LOG as well as serial, so a run is
//   fully recoverable with PlogDump even if no serial capture was attached.
// ---------------------------------------------------------------------------

#include <stdarg.h>
#include <esp_system.h>  // esp_restart()
#include <Wire.h>
#include <VL53L4CD.h>

// Flash logging (LittleFS data partition, same one PlogDump reads). Do NOT clear
// at boot — the log APPENDS across boots (see setup()).
#include "persistent_log.h"

const int GRID_W = 37;
const int GRID_H = 18;

// CNC homes to full negatives, so the work area lives in negative coordinates.
const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 412.0f;  // MUST equal GRBL $131 — homing pins the -Y switch at -$131, so this anchors the grid

// TCS3200 color sensor: S0-S3 + OUT on D4..D8 (unchanged from PARMain).
const int TCS_S0 = D4;
const int TCS_S1 = D5;
const int TCS_S2 = D6;
const int TCS_S3 = D7;
const int TCS_OUT = D8;

// LED illumination bank on D10, driving an NPN. D10 HIGH = LEDs ON.
const int LED_PIN = D10;
// Settle after toggling the LEDs before reading (ambient-subtraction cadence).
const int LED_SETTLE_MS = 20;
// Number of LED off/on flashes averaged per cell (PARMain's production value).
const int AMBIENT_FLASHES = 3;

// ---- VL53L4CD lidar (I2C on the Nano's fixed A4/A5) ------------------------
// XSHUT: pull LOW to hold the sensor in reset; -1 if unwired / tied to VDD.
#define LIDAR_XSHUT_PIN -1
// Calibration offsets (mm), added to every raw reading. COLOR-DEPENDENT: the
// ToF reading lands differently on the two BACK colors (both sensors view the
// disc's back), so the offset is picked per cell from the same-row color read.
// In TENTHS of a mm. Applied to the trimmed MEAN rather than to each sample, so
// a fractional offset survives (per-sample integer math would round it away).
//
// The black-back value was refit from the pass-1 scan: comparing black-back and
// cyan-back means within each row (which controls for the board's vertical tilt)
// showed black reading 0.19 mm CLOSER than cyan under the old integer -13/-20
// pair. -13 was the best whole-mm value available; with tenths the refit value
// -12.8 is now expressible directly, so the correction no longer has to be
// applied by hand in post-processing.
const int16_t DIST_OFFSET_BLACK_BACK_TENTHS = -128;  // -12.8 mm; black back (displayed front = cyan/on)
const int16_t DIST_OFFSET_CYAN_BACK_TENTHS  = -200;  // -20.0 mm; cyan back  (displayed front = black/off)

// RAW MODE. When 0, the offsets above are NOT applied — every logged distance
// is the uncorrected trimmed mean straight off the sensor. That is the point of
// a calibration-square run: the squares are fixed references at a known
// standoff, so the correction is derived offline as (square reading) + a
// constant, rather than baked into the firmware before the data exists. The
// per-cell `back=black|cyan` marker is still logged, so the offline join can
// still split the two populations. Set to 1 to go back to corrected output.
#define LIDAR_APPLY_CALIBRATION 0
// Per-cell averaging window: started fresh AFTER the move to the cell has
// finished, recorded for this long, then cleared (nothing carries across
// cells). ~80 samples at the 50 ms timing budget.
const unsigned long LIDAR_WINDOW_MS = 4000;
const int LIDAR_MAX_SAMPLES = 100;  // cap; 4 s / 50 ms ≈ 80 + slack

VL53L4CD lidar;

// S2/S3 select the photodiode filter bank.
enum TcsFilter {
  TCS_RED = 0,    // S2=L, S3=L
  TCS_BLUE = 1,   // S2=L, S3=H
  TCS_CLEAR = 2,  // S2=H, S3=L
  TCS_GREEN = 3   // S2=H, S3=H
};

// Servo control offloaded to a dedicated 5V Arduino Nano over a bit-banged TX
// line on Arduino D9 → 5V Nano D0 RX, shared GND, one-way. 9600-baud software
// UART (mbed's UART class on an arbitrary PinName crashed the chip). See
// PARMain.ino / ServoNano.ino for the full rationale.
const int SERVO_TX_PIN = D9;

const int SERVO_US_REST = 565;  // ≈2°, arm parked (the ONLY angle this test uses)
const int SERVO_90_DEG_SETTLE_MS = 300;
const int SERVO_50_DEG_SETTLE_MS = 100;

// PORT (Arduino Nano ESP32): the RP2040 bit-banged this 9600-baud frame on D9
// with interrupts disabled (servoTxByte + SERVO_TX_BIT_US, both deleted). The
// ESP32-S3 GPIO matrix routes a real UART to any pin, so the link is now
// hardware Serial2 TX on the SAME physical D9 wire -- same 9600 8N1 framing, no
// ISR blackout, no bit-period tuning. RX is unused (-1): the link is still
// one-way; the ack is the separate D2 level line.

// The link is ONE-WAY with no ack, so a dropped byte silently LOSES a command
// and the arm simply stays where it was. That broke the flip arm once: a lost
// REST left it at ENGAGE, and flipDisc then ran both X strokes with the arm
// buried in the board. A receiver-side check cannot help -- a command that never
// arrives cannot be rejected -- so every command is sent SERVO_TX_REPEATS times.
// writeMicroseconds() is idempotent, so the repeats are free: re-commanding the
// position the servo already holds does nothing. Losing a command now takes
// SERVO_TX_REPEATS independent dropouts instead of one.
//
// The repeats also fix LATE application: if only the trailing newline is lost,
// the stranded digits sit in the ServoNano's buffer until the NEXT command's
// leading newline flushes them -- which without repeats is up to a full settle
// period later, i.e. after the stroke has already started. The next repeat
// flushes them SERVO_TX_REPEAT_GAP_MS later instead.
const int SERVO_TX_REPEATS = 3;
const int SERVO_TX_REPEAT_GAP_MS = 6;

void servoTxLine(int us) {
  char buf[12];
  snprintf(buf, sizeof(buf), "\n%d\n", us);
  for (int r = 0; r < SERVO_TX_REPEATS; r++) {
    Serial2.print(buf);
    // Block until the last stop bit is actually on the wire, so this call
    // stays synchronous like the old bit-bang did -- callers time their
    // settle delay from here.
    Serial2.flush();
    if (r + 1 < SERVO_TX_REPEATS) delay(SERVO_TX_REPEAT_GAP_MS);
  }
}

void writeServoUs(int us, int settle_ms) {
  servoTxLine(us);
  delay(settle_ms);
}

struct Coord {
  float x;
  float y;
};

// grid[GRID_H-1][0] is bottom-left at the home-relative origin; physical y
// increases upward while bitmap y=0 is the top row, so y is mirrored when
// computing gridY.
Coord grid[GRID_H][GRID_W];

void initGrid() {
  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      //           starting offset⌄
      // Starting X offset is 25 mm; the addressable grid covers physical
      // cols 0..36 as logical x=0..36.
      grid[y][x].x = -X_TRAVEL + 25.0f + 20.045f * x;
      grid[y][x].y = -Y_TRAVEL + 0.0f + 23.40f * ((GRID_H - 1) - y);
      //                                  ⌃grid spacing
    }
  }
}

// GRBL character-counting streaming protocol. GRBL's RX buffer is 128 bytes;
// keep a few bytes of slack so we never overrun.
const int RX_BUFFER_SAFE = 120;
const int QUEUE_SIZE = 32;

// If sendGcode/waitForIdle stall this long without seeing any GRBL progress,
// assume comms with the Mega are wedged and force an MCU reset so the device
// recovers on its own. This rig has had homing stalls — keep the watchdog.
const unsigned long GRBL_STALL_TIMEOUT_MS = 60000;

// Keep the command text per slot so error:N can be retried (re-sent) without
// the caller knowing. 40 bytes matches the largest snprintf buffer used by
// moveTo()/moveToYSafe(); anything longer is a bug.
const int MAX_CMD_LEN = 40;
char cmdTexts[QUEUE_SIZE][MAX_CMD_LEN];
int cmdLengths[QUEUE_SIZE];
int qHead = 0;
int qTail = 0;
int bufferFill = 0;

// Diagnostic counters for the streaming path.
unsigned long gCmdsSent = 0;
unsigned long gOksAcked = 0;

// error:N retry bookkeeping.
const int MAX_ERROR_RETRIES = 10;
const unsigned long ERROR_RETRY_DELAY_MS = 3000;
int errorRetryCount = 0;
char lastErrorCmd[MAX_CMD_LEN] = "";

// During setup's homing phase an error/ALARM from GRBL is recoverable — bounce
// Serial1 and retry rather than wedging forever. Outside setup, ALARM triggers
// grblAlarmRecover() and error:N is retried.
bool inStartupPhase = false;
volatile bool grblStartupFault = false;

// Park the servo at REST before any reset so the gantry doesn't reboot with the
// arm mid-flip. The 5V Nano keeps driving the servo across our reset.
void parkServoForReset() {
  servoTxLine(SERVO_US_REST);
  delay(SERVO_90_DEG_SETTLE_MS);
}

// ESP32-S3: esp_restart() from <esp_system.h> (was esp_restart() on the
// RP2040's CMSIS headers).
void grblStallReset(const char* where) {
  plog::logf("GRBL stall in %s -> MCU reset", where);
  parkServoForReset();
  delay(50);
  esp_restart();
}

void enqueue(const char* cmd, int len) {
  if ((qTail + 1) % QUEUE_SIZE == qHead) {
    plog::logf("QUEUE FULL overwrite qH=%d qT=%d buf=%d", qHead, qTail, bufferFill);
  }
  strncpy(cmdTexts[qTail], cmd, MAX_CMD_LEN - 1);
  cmdTexts[qTail][MAX_CMD_LEN - 1] = '\0';
  cmdLengths[qTail] = len;
  qTail = (qTail + 1) % QUEUE_SIZE;
}

int dequeue() {
  int len = cmdLengths[qHead];
  qHead = (qHead + 1) % QUEUE_SIZE;
  return len;
}

// Direct UART send that bypasses the char-counting queue — used only inside
// recovery paths which manage buffer accounting themselves.
void rawSerial1Line(const char* s) {
  Serial1.print(s);
  Serial1.write('\n');
}

// Synchronous "send + wait for ok" used during ALARM recovery. Does not use the
// queue — recovery runs with bufferFill=0 and we want one ack at a time.
bool recoverySendAndWait(const char* cmd, unsigned long timeout_ms) {
  while (Serial1.available()) Serial1.read();
  rawSerial1Line(cmd);
  unsigned long t0 = millis();
  while (millis() - t0 < timeout_ms) {
    if (Serial1.available()) {
      String r = Serial1.readStringUntil('\n');
      r.trim();
      if (r.length() == 0) continue;
      if (r == "ok") return true;
      if (r.startsWith("error") || r.startsWith("ALARM")) {
        plog::logf("recovery '%s' got %.40s", cmd, r.c_str());
        return false;
      }
      // Ignore status reports, [MSG:...], banner lines, $$ output.
    }
  }
  plog::logf("recovery '%s' timeout", cmd);
  return false;
}

// ALARM recovery: soft-reset GRBL (Ctrl-X / 0x18), wait for boot, re-home,
// reassert modal state. On any sub-step failure we MCU-reset as a fallback.
void grblAlarmRecover() {
  plog::log("ALARM recovery: soft reset + rehome");
  Serial1.write(0x18);  // Ctrl-X — GRBL soft reset
  delay(2000);          // GRBL boot wait
  while (Serial1.available()) Serial1.read();

  qHead = qTail = 0;
  bufferFill = 0;
  errorRetryCount = 0;
  lastErrorCmd[0] = '\0';

  if (!recoverySendAndWait("$H", 60000)) {
    plog::log("ALARM recovery $H failed -> MCU reset");
    parkServoForReset();
    delay(50);
    esp_restart();
  }
  if (!recoverySendAndWait("$1=255", 5000) ||
      !recoverySendAndWait("G21", 5000) ||
      !recoverySendAndWait("G90", 5000)) {
    plog::log("ALARM recovery modal-set failed -> MCU reset");
    parkServoForReset();
    delay(50);
    esp_restart();
  }

  plog::log("ALARM recovery complete");
}

void drainResponses() {
  while (Serial1.available()) {
    String resp = Serial1.readStringUntil('\n');
    resp.trim();
    if (resp.length() == 0) continue;

    if (resp == "ok") {
      // grbl-Mega can emit a duplicate `ok` after $H. Without this guard the
      // spurious ack dequeues a stale slot, desyncing bufferFill.
      if (qHead != qTail) {
        bufferFill -= dequeue();
        gOksAcked++;
        errorRetryCount = 0;
        lastErrorCmd[0] = '\0';
      } else {
        plog::log("GRBL ok but queue empty (spurious ack)");
      }
    } else if (resp.startsWith("ALARM")) {
      if (qHead != qTail) {
        plog::logf("GRBL ALARM: %.16s on '%.18s'", resp.c_str(), cmdTexts[qHead]);
      } else {
        plog::logf("GRBL ALARM: %.34s (queue empty)", resp.c_str());
      }
      if (inStartupPhase) {
        grblStartupFault = true;
        return;
      }
      grblAlarmRecover();
      return;
    } else if (resp.startsWith("error")) {
      if (inStartupPhase) {
        plog::logf("GRBL startup error: %.40s", resp.c_str());
        grblStartupFault = true;
        return;
      }
      // error:N is non-fatal — GRBL still consumed the line, so drop the queue
      // slot, then re-send the same command after a delay. Cap at
      // MAX_ERROR_RETRIES for the *same* command in a row before MCU-resetting.
      if (qHead == qTail) {
        plog::logf("GRBL error with empty queue: %.40s", resp.c_str());
        continue;
      }
      char failedCmd[MAX_CMD_LEN];
      strncpy(failedCmd, cmdTexts[qHead], MAX_CMD_LEN);
      failedCmd[MAX_CMD_LEN - 1] = '\0';
      bufferFill -= dequeue();

      if (strcmp(failedCmd, lastErrorCmd) == 0) {
        errorRetryCount++;
      } else {
        strncpy(lastErrorCmd, failedCmd, MAX_CMD_LEN);
        lastErrorCmd[MAX_CMD_LEN - 1] = '\0';
        errorRetryCount = 1;
      }

      plog::logf("GRBL %.20s on '%.20s' retry %d/%d",
                 resp.c_str(), failedCmd, errorRetryCount, MAX_ERROR_RETRIES);

      if (errorRetryCount > MAX_ERROR_RETRIES) {
        plog::logf("error retries exhausted for '%s' -> MCU reset", failedCmd);
        parkServoForReset();
        delay(50);
        esp_restart();
      }

      delay(ERROR_RETRY_DELAY_MS);

      int rlen = strlen(failedCmd) + 1;
      rawSerial1Line(failedCmd);
      bufferFill += rlen;
      strncpy(cmdTexts[qTail], failedCmd, MAX_CMD_LEN - 1);
      cmdTexts[qTail][MAX_CMD_LEN - 1] = '\0';
      cmdLengths[qTail] = rlen;
      qTail = (qTail + 1) % QUEUE_SIZE;
    } else {
      // Status reports `<...>`, settings `$N=...`, `[MSG:...]`, welcome banner
      // `Grbl ...` — not tied to a queued command. A mid-sweep `Grbl ` banner
      // means the Mega itself reset (brownout/watchdog).
      plog::logf("GRBL other: %.30s", resp.c_str());
    }
  }
}

void sendGcode(const char* cmd) {
  int cmdLen = strlen(cmd) + 1;  // +1 for the newline GRBL counts

  unsigned long stallT0 = millis();
  int lastFill = bufferFill;
  while (bufferFill + cmdLen > RX_BUFFER_SAFE) {
    drainResponses();
    if (inStartupPhase && grblStartupFault) return;
    if (bufferFill != lastFill) {
      lastFill = bufferFill;
      stallT0 = millis();
    }
    if (millis() - stallT0 > GRBL_STALL_TIMEOUT_MS) grblStallReset("sendGcode");
  }

  // Send `\n` only, not `\r\n` — grbl-Mega treats `\r` as a line end then acks
  // the trailing `\n` as an empty line, producing a duplicate ok per command.
  Serial1.print(cmd);
  Serial1.write('\n');
  bufferFill += cmdLen;
  enqueue(cmd, cmdLen);
  gCmdsSent++;
}

void waitForIdle() {
  unsigned long stallT0 = millis();
  int lastFill = bufferFill;
  while (bufferFill > 0) {
    drainResponses();
    if (inStartupPhase && grblStartupFault) return;
    if (bufferFill != lastFill) {
      lastFill = bufferFill;
      stallT0 = millis();
    }
    if (millis() - stallT0 > GRBL_STALL_TIMEOUT_MS) grblStallReset("waitForIdle");
  }
}

void moveTo(float x, float y) {
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "G0 X%.3f Y%.3f", x, y);
  sendGcode(cmd);
}

// Travel to (targetX, targetY) such that any vertical (Y) component happens with
// X pinned to the nearest absolute machine limit (X = 0 or X = -X_TRAVEL). Emits
// pure-X → pure-Y → pure-X, so the Y leg never drags the head across the disc
// area at a non-limit X.
void moveToYSafe(float targetX, float targetY) {
  char cmd[40];
  float xLimit = (targetX > -X_TRAVEL / 2.0f) ? 0.0f : -X_TRAVEL;
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", xLimit);
  sendGcode(cmd);
  snprintf(cmd, sizeof(cmd), "G0 Y%.3f", targetY);
  sendGcode(cmd);
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", targetX);
  sendGcode(cmd);
}

// `G4 P0` is a dwell that GRBL syncs through the planner before acking, so once
// its `ok` lands every queued motion has actually finished — not just planned.
void waitForMotion() {
  sendGcode("G4 P0");
  waitForIdle();
}

// Re-home + reassert mm/absolute modes.
void rehome() {
  sendGcode("$H");
  waitForIdle();
  sendGcode("$1=255");
  waitForIdle();  // sync past the $1 EEPROM write before pipelining more — grbl
                  // disables interrupts during the commit and drops Serial1 RX
                  // bytes.
  sendGcode("G21");
  sendGcode("G90");
  waitForIdle();
}

void tcsSelect(TcsFilter f) {
  digitalWrite(TCS_S2, (f & 0x02) ? HIGH : LOW);
  digitalWrite(TCS_S3, (f & 0x01) ? HIGH : LOW);
}

// (Re-)assert the color-sensor pin config and the LED driver pin. S0/S1 =
// HIGH/LOW selects 20% output frequency scaling — the regime PARMain reads in.
void initColorSensor() {
  pinMode(TCS_S0, OUTPUT);
  pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT);
  pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);  // LEDs off when idle
  digitalWrite(TCS_S0, HIGH);
  digitalWrite(TCS_S1, LOW);
  tcsSelect(TCS_CLEAR);
}

// OUT is a 50%-duty square wave whose frequency tracks light intensity for the
// active filter. pulseIn times one half-period; doubling gives the full period.
// 100 ms timeout keeps a dark / disconnected sensor from hanging.
unsigned long tcsReadFrequencyHz() {
  unsigned long halfUs = pulseIn(TCS_OUT, HIGH, 100000UL);
  if (halfUs == 0) return 0;
  return 500000UL / halfUs;
}

// Average 5 consecutive RGBC frames at the current head position (2ms-paced),
// at whatever the current illumination is. Used twice per flash by
// readAmbientSubtracted() — once LEDs-off, once LEDs-on. (PARMain's production
// version.)
void tcsReadRGBC(unsigned long& r, unsigned long& g,
                 unsigned long& b, unsigned long& c) {
  uint32_t sr = 0, sg = 0, sb = 0, sc = 0;
  for (int i = 0; i < 5; i++) {
    tcsSelect(TCS_RED);
    delay(2);
    sr += tcsReadFrequencyHz();
    tcsSelect(TCS_GREEN);
    delay(2);
    sg += tcsReadFrequencyHz();
    tcsSelect(TCS_BLUE);
    delay(2);
    sb += tcsReadFrequencyHz();
    tcsSelect(TCS_CLEAR);
    delay(2);
    sc += tcsReadFrequencyHz();
  }
  r = sr / 5;
  g = sg / 5;
  b = sb / 5;
  c = sc / 5;
}

// One ambient-subtracted RGBC read, averaged over AMBIENT_FLASHES off/on
// flashes (PARMain's production 3-flash method). Each flash subtracts a
// 5-frame LEDs-OFF (ambient) read from a 5-frame LEDs-ON (lit) read: whatever
// room light is present shows up in both and cancels, so the result depends
// only on the disc + our own LEDs. Per-flash negatives clamped to 0. LEDs left
// off.
void readAmbientSubtracted(long& r, long& g, long& b, long& c) {
  long sr = 0, sg = 0, sb = 0, sc = 0;
  for (int i = 0; i < AMBIENT_FLASHES; i++) {
    unsigned long ar, ag, ab, ac, lr, lg, lb, lc;
    digitalWrite(LED_PIN, LOW);  delay(LED_SETTLE_MS); tcsReadRGBC(ar, ag, ab, ac);
    digitalWrite(LED_PIN, HIGH); delay(LED_SETTLE_MS); tcsReadRGBC(lr, lg, lb, lc);
    long dr = (long)lr - (long)ar; if (dr < 0) dr = 0;
    long dg = (long)lg - (long)ag; if (dg < 0) dg = 0;
    long db = (long)lb - (long)ab; if (db < 0) db = 0;
    long dc = (long)lc - (long)ac; if (dc < 0) dc = 0;
    sr += dr; sg += dg; sb += db; sc += dc;
  }
  digitalWrite(LED_PIN, LOW);
  r = sr / AMBIENT_FLASHES; g = sg / AMBIENT_FLASHES;
  b = sb / AMBIENT_FLASHES; c = sc / AMBIENT_FLASHES;
}

// Back-color classification cut on the ambient-subtracted clear channel
// (PARMain's production threshold). POLARITY: the sensor views the disc's
// BACK — clear BELOW the cut means it's looking at a black back (displayed
// front = cyan/on); clear above means a cyan back (displayed front = off).
// Classification threshold. Physically the BLUE channel, not clear — the S2/S3
// select lines are crossed on this rig, so tcsReadRGBC's `c` output holds blue.
// That is deliberate (blue separates the disc faces 10.21x vs clear's 2.22x).
// Full explanation and the measurements are in PARMain.ino at this constant.
// 3535 = geometric mean of the measured populations; was 6000, which was
// lopsided (3.69x / 1.28x) toward the failure side.
const long SCAN_ON_BLUE_MAX = 3535;

// One per-cell distance estimate. Collects blocking reads of the free-running
// ranger for LIDAR_WINDOW_MS (window starts fresh here — the caller invokes
// this only once the head is stationary at the cell), applies the cell's
// color-dependent calibration offset (clamped to 0), then reduces to
// VL53L4CDTest's trimmed mean: sort, drop the top/bottom 20%, average the
// rest. avg10 is the trimmed mean in TENTHS of a mm (integer-tenths math, no
// float printf needed) — 1-decimal resolution instead of nearest-int, since
// per-cell noise is sub-mm and whole-mm rounding quantizes the analysis.
// Returns the sample count (0 = every read timed out; avg10/mn/mx = 0).
int lidarWindowRead(int16_t offsetTenths, uint32_t& avg10, uint16_t& mn, uint16_t& mx) {
  static uint16_t s[LIDAR_MAX_SAMPLES];  // scratch, overwritten every call
  int n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < LIDAR_WINDOW_MS) {
    uint16_t raw = lidar.read();
    if (lidar.timeoutOccurred()) {
      plog::log("# lidar timeout");
      continue;
    }
    if (n < LIDAR_MAX_SAMPLES) s[n++] = raw;   // raw; offset applied to the mean
  }
  if (n == 0) {
    avg10 = 0;
    mn = mx = 0;
    return 0;
  }
  for (int i = 1; i < n; i++) {  // insertion sort
    uint16_t v = s[i];
    int j = i - 1;
    while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
    s[j + 1] = v;
  }
  int trim = n / 5;  // 20% off each end
  int lo = trim, hi = n - trim;
  if (hi <= lo) { lo = 0; hi = n; }
  uint32_t sum = 0;
  for (int i = lo; i < hi; i++) sum += s[i];
  int32_t mean10 = (int32_t)((sum * 10 + (uint32_t)(hi - lo) / 2) / (uint32_t)(hi - lo));
  mean10 += offsetTenths;                 // fractional offset, applied once
  if (mean10 < 0) mean10 = 0;
  avg10 = (uint32_t)mean10;
  // Keep the window min/max on the SAME scale as the mean. They are raw samples
  // at this point (the offset now lands on the mean), so shift them too --
  // otherwise avg and min/max would be in different reference frames.
  int32_t offMm = (offsetTenths >= 0) ? (offsetTenths + 5) / 10 : (offsetTenths - 5) / 10;
  int32_t lo10 = (int32_t)s[0] + offMm, hi10 = (int32_t)s[n - 1] + offMm;
  mn = (uint16_t)(lo10 < 0 ? 0 : lo10);
  mx = (uint16_t)(hi10 < 0 ? 0 : hi10);
  return n;
}

// Sensor head offsets from the flip actuator on the gantry. Color = PARMain's
// calibrated SCAN_OFFSET_X/Y.
const float SCAN_OFFSET_X = -24.005f;
const float SCAN_OFFSET_Y = 8.0f;
// Lidar X: calibrated so col 36 (cell.x -31.075) targets exactly X=0 — the
// rightmost column rides the machine limit. (~55.08 mm right of the color
// sensor; an earlier 60 mm estimate overshot.)
const float LIDAR_OFFSET_X = 31.075f;
// Lidar Y: 6 mm above the color sensor. Top row needs $131/Y_TRAVEL = 412.
const float LIDAR_OFFSET_Y = SCAN_OFFSET_Y + 6.0f;  // +14.0

// Keep scan targets inside the 0-edge soft limits (GRBL rejects target > 0
// with ALARM:2 under $20=1; the limit itself is allowed). Y clamp is
// PARMain's safety net. The X clamp is a no-op at the current calibration —
// col 36 targets exactly X=0 — kept as a safety net for offset tweaks.
const float SCAN_Y_MAX = -0.05f;
const float SCAN_X_MAX = 0.0f;
static inline float clampScanY(float y) { return y > SCAN_Y_MAX ? SCAN_Y_MAX : y; }
static inline float clampScanX(float x) { return x > SCAN_X_MAX ? SCAN_X_MAX : x; }

static inline float colorX(int y, int x) { return grid[y][x].x + SCAN_OFFSET_X; }
static inline float colorY(int y, int x) { return clampScanY(grid[y][x].y + SCAN_OFFSET_Y); }
static inline float lidarX(int y, int x) { return clampScanX(grid[y][x].x + LIDAR_OFFSET_X); }
static inline float lidarY(int y, int x) { return clampScanY(grid[y][x].y + LIDAR_OFFSET_Y); }

// ---- Fixed calibration squares --------------------------------------------
// Two reference squares mounted off the left edge of the board, one BLACK and
// one CYAN/BLUE 20 mm above it, at a fixed known standoff. They are sampled
// with the same 4 s trimmed-mean window as a normal cell, so a pass carries its
// own reference for BOTH back colors. The compensation target is then derived
// offline as (square reading) + a constant, instead of trusting the firmware
// offsets — which is why LIDAR_APPLY_CALIBRATION is 0 for this run.
//
// These are COMMANDED MACHINE COORDINATES — feed them straight to moveToYSafe.
// They are deliberately NOT expressed as lidar-physical positions with
// LIDAR_OFFSET_X/Y added back on, the way a cell target is: these two points
// were established by jogging the gantry with SerialBridge and eyeballing the
// lidar over each square (2026-07-28), so the confirmed artifact IS the
// commanded coordinate. Re-deriving it through the head offset would only
// reintroduce the ambiguity that made the first jog land 31 mm off.
//
// To re-confirm after a board move: flash SerialBridge, `$H`, `G21 G90`, then
// `G0 X<CAL_X> Y<CAL_BLACK_Y>` and look. For reference the lidar itself then
// sits at (cmd.x - LIDAR_OFFSET_X, cmd.y - LIDAR_OFFSET_Y) = (-806.770,
// -350.000) for black — well left of col 0 (-752.695), i.e. off-board.
//
//   X: 2 mm right of the left travel limit  (-X_TRAVEL + 2  = -775.695)
//   black Y: 76 mm up from the bottom limit (-Y_TRAVEL + 76 = -336.000)
const float CAL_X       = -X_TRAVEL + 2.0f;   // -775.695
const float CAL_BLACK_Y = -Y_TRAVEL + 76.0f;  // -336.000
// The blue/cyan square sits 20 mm ABOVE the black one (physical +Y is up).
// Jog-confirmed independently at -316.000, which is exactly 20 mm up.
const float CAL_BLUE_Y  = CAL_BLACK_Y + 20.0f;  // -316.000

// Sample both calibration squares and log them under a stage tag
// ("start" | "mid" | "end"). Six of these per pass — two squares × three
// stages — so drift across the ~71 min run is measurable, and each row can be
// corrected against the nearest reference in time.
//
// Travel is routed through moveToYSafe for BOTH squares, including the 20 mm
// black→blue hop: the squares are off-board but the gantry reference is not
// (commanded X lands between cols 0 and 1), so a bare pure-Y move there would
// drag the head across the disc field at a non-limit X.
void sampleCalibrationSquares(const char* stage) {
  struct { const char* name; float x, y; } sq[2] = {
    { "black", CAL_X, CAL_BLACK_Y },
    { "blue",  CAL_X, CAL_BLUE_Y  },
  };

  for (int i = 0; i < 2; i++) {
    moveToYSafe(sq[i].x, sq[i].y);
    waitForMotion();

    // Same discard-then-window cadence as a cell: the in-flight free-running
    // measurement may straddle the tail of the move.
    lidar.read();
    uint32_t avg10;
    uint16_t mn, mx;
    int n = lidarWindowRead(0, avg10, mn, mx);  // always RAW — a reference must be uncorrected

    plog::logf("# cal %s %s cmd=(%.3f,%.3f)", stage, sq[i].name, sq[i].x, sq[i].y);
    plog::logf("%lu.%lu,%d,%u,%u",
               (unsigned long)(avg10 / 10), (unsigned long)(avg10 % 10), n, mn, mx);
  }
}

// ---------------------------------------------------------------- scan order
// WHY THIS EXISTS. Passes 1-4 all walked rows top-to-bottom, one row every
// ~3.9 min, which makes row index and elapsed time the SAME VARIABLE
// (measured on pass 4: r = 0.999987). Sensor drift then lands exactly along the
// axis the scan is trying to measure, and on pass 4 the drift correction
// (3.40 mm) was 1.5x LARGER than the vertical board signal it was meant to
// reveal (2.31 mm row-to-row span). The raw data alone implies only 0.85 mm of
// slope if the board were flat; the cal square asserts 3.40 mm of drift, and
// the leftover 2.55 mm becomes "board tilt" by subtraction. That is not a
// measurement, and it is why the bottom rows commanded too much reach.
//
//   0 = top-to-bottom  (rows 0..GRID_H-1) — the historical order
//   1 = bottom-to-top  (rows GRID_H-1..0) — REVERSED, for the decisive run
//
// Run the board once at 1 and compare the vertical gradient against a 0 pass:
// if the tilt FLIPS SIGN, it was drift and the old table's tilt is an artifact.
// If it holds, the tilt is real. Everything else about the scan is identical,
// so the two are directly comparable.
//
// Longer term this should become an INTERLEAVED order (0,9,1,10,2,11,...) so
// drift shows up as high-frequency scatter the per-column fit averages out,
// rather than a monotonic tilt the fit absorbs as shape.
#define SCAN_ROW_ORDER 1

// Maps visit index -> bitmap row. Every loop-order decision below (mid-scan
// rehome, last-row test, next-row move) keys off the INDEX i, never off y, so
// reversing the order cannot silently break them.
static inline int rowAt(int i) {
#if SCAN_ROW_ORDER
  return (GRID_H - 1) - i;
#else
  return i;
#endif
}

// One full scan sweep. The flip arm stays parked at SERVO_US_REST the entire
// time, so the physical board pattern is undisturbed. Each row is covered
// twice: a COLOR sub-sweep L→R at the color-sensor Y, then a LIDAR sub-sweep
// R→L at the lidar Y (+3 mm). Sub-sweeps are pure-X; the color→lidar Y hop and
// every cross-row leg go through moveToYSafe (pure-Y only at X soft-limits).
// A mid-scan re-home runs after row 8 so accumulated step drift can't skew the
// rest of the scan.
//
// PIPELINED to hide the (slow) flash writes behind GRBL motion: at each cell we
// (1) sense into RAM while stationary, (2) START the move to the next target —
// sendGcode hands the short G0(s) straight to GRBL, so it's already moving —
// then (3) write this cell's lines to the flash log while that move runs,
// (4) waitForMotion before sensing at the next target.
void doScan() {
  rehome();

  // Reference reading before any cell is touched.
  sampleCalibrationSquares("start");

  // Per-row back-color classification from the color sub-sweep, consumed by
  // the same row's lidar sub-sweep to pick each cell's distance offset.
  bool rowBackBlack[GRID_W];

  for (int i = 0; i < GRID_H; i++) {
    const int y     = rowAt(i);
    const int yNext = (i + 1 < GRID_H) ? rowAt(i + 1) : -1;
    // Per-row drift reference. Pass 4 sampled the squares only 3x across
    // 71.5 min, so the drift curve rested on two chords and its magnitude was
    // never independently checked against the cells. One sample per row turns 3
    // observations into GRID_H+2, which both resolves the curve and lets the
    // analysis test whether cal-square drift actually tracks cell drift.
    // Costs ~11 s/row (two windows + the moves) — about 3.3 min on a 71 min pass.
    //
    // Taken at the TOP of the row, as the old y==9 sample was: the color
    // sub-sweep below repositions with moveToYSafe, so starting from the cal
    // square is free, and nothing is pipelined in flight here.
    {
      char stage[12];
      snprintf(stage, sizeof(stage), "row%d", y);
      sampleCalibrationSquares(stage);
    }

    // ---- COLOR sub-sweep, L→R at the color-sensor Y ----
    plog::logf("# row %d color", y);
    moveToYSafe(colorX(y, 0), colorY(y, 0));
    waitForMotion();

    for (int x = 0; x < GRID_W; x++) {
      long r, g, b, c;
      readAmbientSubtracted(r, g, b, c);
      rowBackBlack[x] = (c < SCAN_ON_BLUE_MAX);

      if (x < GRID_W - 1) {
        moveTo(colorX(y, x + 1), colorY(y, x + 1));  // intra-row (pure X)
      } else {
        // Cross to this row's lidar sweep start: the +3 mm Y hop rides the
        // X=0 edge leg (nearest limit — we're at the row's right end).
        moveToYSafe(lidarX(y, GRID_W - 1), lidarY(y, GRID_W - 1));
      }

      plog::logf("# y=%d x=%d", y, x);
      plog::logf("%ld,%ld,%ld,%ld", r, g, b, c);

      waitForMotion();
    }

    // ---- LIDAR sub-sweep, R→L at the lidar Y ----
    plog::logf("# row %d lidar", y);
    for (int x = GRID_W - 1; x >= 0; x--) {
#if LIDAR_APPLY_CALIBRATION
      int16_t offsetTenths = rowBackBlack[x] ? DIST_OFFSET_BLACK_BACK_TENTHS
                                             : DIST_OFFSET_CYAN_BACK_TENTHS;
#else
      const int16_t offsetTenths = 0;  // RAW run — correction derived offline from the cal squares
#endif
      // Discard one read — the free-running measurement in flight may straddle
      // the tail of the move — then record the 4 s window while stationary.
      lidar.read();
      uint32_t avg10;
      uint16_t mn, mx;
      int n = lidarWindowRead(offsetTenths, avg10, mn, mx);

      bool lastCell = (x == 0);
      bool lastRow = (i == GRID_H - 1);
      if (!lastCell) {
        moveTo(lidarX(y, x - 1), lidarY(y, x - 1));  // intra-row (pure X)
      } else if (!lastRow) {
        // Re-home midway (after row 8) so accumulated step drift can't skew
        // the rest of the scan; then the safe edge-leg move to the next row's
        // color-sweep start (nearest limit is -X_TRAVEL — we're at the left).
        if (i == 8) rehome();  // midway through the VISIT order, not row 8
        moveToYSafe(colorX(yNext, 0), colorY(yNext, 0));
      }

      plog::logf("# y=%d x=%d back=%s", y, x, rowBackBlack[x] ? "black" : "cyan");
      plog::logf("%lu.%lu,%d,%u,%u", (unsigned long)(avg10 / 10), (unsigned long)(avg10 % 10), n, mn, mx);

      if (!(lastCell && lastRow)) waitForMotion();
    }
  }

  // Closing reference — pairs with "start" to bound drift across the pass.
  sampleCalibrationSquares("end");
}

void setup() {
  Serial.begin(115200);

  // Flash log. Do NOT clear at boot — a mid-session stall-reset (or a
  // power-cycle to stop a run) would wipe completed passes before they're ever
  // dumped (this bit us once). The log APPENDS across boots; the `booting` /
  // `pass N begin|end` / `# y= x=` markers delineate runs, and PlogDump reads
  // the accumulated tail. plog mirrors to serial (guarded) for a live monitor.
  plog::begin();
  plog::log("ScanColorLidarTest booting — logging to flash");

  // Sensor pins + 20% scaling + LED pin (LEDs off).
  initColorSensor();

  // VL53L4CD before the (slow) GRBL bring-up, so a wiring fault is reported
  // immediately instead of after a 60 s homing cycle.
  if (LIDAR_XSHUT_PIN >= 0) {
    pinMode(LIDAR_XSHUT_PIN, OUTPUT);
    digitalWrite(LIDAR_XSHUT_PIN, LOW);
    delay(10);
    digitalWrite(LIDAR_XSHUT_PIN, HIGH);
    delay(10);
  }
  Wire.begin();
  Wire.setClock(400000);  // 400 kHz fast mode
  lidar.setTimeout(500);
  // Boot init is flaky (failed ~half of observed power-ons, then ran a full
  // 71-min pass flawlessly once up) — retry with an I2C re-init between
  // attempts before giving up.
  {
    int attempt = 1;
    while (!lidar.init()) {
      plog::logf("VL53L4CD init failed (attempt %d)", attempt);
      if (++attempt > 10) {
        plog::log("VL53L4CD init FAILED after 10 attempts — check wiring/I2C. Halting.");
        while (true) delay(1000);
      }
      Wire.end();
      delay(1000);
      Wire.begin();
      Wire.setClock(400000);
    }
  }
  // 50 ms timing budget, back-to-back (free-running); longer = lower noise.
  lidar.setRangeTiming(50, 0);
  lidar.startContinuous();
  plog::log("VL53L4CD ready");

  // Bring up the servo hardware UART (Serial2 TX on D9) and park the arm at REST.
  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);  // TX-only servo link on D9
  delay(100);
  writeServoUs(SERVO_US_REST, SERVO_90_DEG_SETTLE_MS);

  // GRBL link. $H FIRST (GRBL boots into alarm and rejects modal G-code with
  // error:9 until homed), then G21/G90 — PARMain's proven boot order.
  // Serial0 owns D0/D1 by default on the Nano ESP32; hand them to Serial1 so
  // the GRBL link keeps its identifier and its physical wires.
  Serial0.end();
  Serial1.begin(115200, SERIAL_8N1, D0, D1);
  delay(2000);  // GRBL boot wait (Serial1)

  inStartupPhase = true;
  for (int attempt = 1;; attempt++) {
    grblStartupFault = false;
    qHead = qTail = 0;
    bufferFill = 0;
    while (Serial1.available()) Serial1.read();

    plog::logf("homing attempt %d", attempt);
    sendGcode("$H");
    waitForIdle();
    if (grblStartupFault) goto restart_grbl;

    // $1=255 keeps steppers energized while idle so the gantry holds position.
    sendGcode("$1=255");
    waitForIdle();  // sync past the $1 EEPROM write before pipelining more
    if (grblStartupFault) goto restart_grbl;
    sendGcode("G21");
    sendGcode("G90");
    waitForIdle();
    if (grblStartupFault) goto restart_grbl;

    plog::log("homed");
    break;

restart_grbl:
    plog::log("GRBL restart + retry homing");
    Serial1.end();
    delay(200);
    Serial1.begin(115200, SERIAL_8N1, D0, D1);
    delay(2000);  // GRBL boot wait after re-opening Serial1
  }
  inStartupPhase = false;

  initGrid();

  plog::log("ScanColorLidarTest ready");
}

void loop() {
  // One pass then halt (power off, flash PlogDump, dump). Raise MAX_PASSES to
  // repeat under varied conditions — the 2 s pause is the change window.
  const unsigned long MAX_PASSES = 1;
  static unsigned long passN = 0;

  if (passN >= MAX_PASSES) {
    plog::log("all passes done — power off, flash PlogDump, and dump the log");
    while (true) delay(1000);
  }

  passN++;
  plog::logf("pass %lu begin", passN);
  // Stamp the calibration into the dump so the data is self-describing -- a dump
  // no longer has to be matched to a firmware build from memory to know whether
  // the back-colour offsets were already applied.
#if LIDAR_APPLY_CALIBRATION
  plog::logf("# offsets black=%d.%d cyan=%d.%d mm (tenths, applied to the mean)",
             DIST_OFFSET_BLACK_BACK_TENTHS / 10,
             (DIST_OFFSET_BLACK_BACK_TENTHS < 0 ? -DIST_OFFSET_BLACK_BACK_TENTHS : DIST_OFFSET_BLACK_BACK_TENTHS) % 10,
             DIST_OFFSET_CYAN_BACK_TENTHS / 10,
             (DIST_OFFSET_CYAN_BACK_TENTHS < 0 ? -DIST_OFFSET_CYAN_BACK_TENTHS : DIST_OFFSET_CYAN_BACK_TENTHS) % 10);
#else
  plog::log("# offsets RAW — no calibration applied; derive from the cal squares");
  plog::logf("# row order: %s (SCAN_ROW_ORDER %d)",
             SCAN_ROW_ORDER ? "BOTTOM-to-TOP (reversed)" : "top-to-bottom", SCAN_ROW_ORDER);
#endif
  doScan();
  plog::logf("pass %lu end", passN);
  delay(2000);  // pause between passes
}
