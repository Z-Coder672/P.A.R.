// ScanColorAmbientTest — LED ambient-subtraction color read over a full grid scan
// ---------------------------------------------------------------------------
// Target: the REAL P.A.R. rig — Arduino Nano RP2040 Connect,
//         FQBN arduino:mbed_nano:nanorp2040connect.
//
// Diagnostic / data-collection sketch. It homes the CNC, sweeps the TCS3200
// sensor over EVERY cell of the 37×18 grid (mirroring PARMain's scanGrid
// traversal), and at each cell logs 5 ambient-subtracted RGBC samples over USB
// serial AND to the flash log (LittleFS, the region PlogDump reads back).
// There is NO WiFi, NO queue/HTTP, NO display/flipping. Recover a run after
// the fact by flashing PlogDump — no live serial capture needed.
//
// The flip arm is parked at SERVO_US_REST for the ENTIRE scan and never dropped
// to the SCAN angle, so the physical board pattern is left undisturbed (set a
// known pattern and compare the logged samples against it).
//
// The GRBL streaming machinery, the $H-first boot order, the 60 s no-progress
// stall watchdog (esp_restart), moveToYSafe / clampScanY, and the servo
// software-UART are all copied verbatim from PARMain.ino (only adapted to log
// over Serial instead of the flash log, and to skip the servo SCAN drop).
//
// OUTPUT FORMAT
//   Diagnostic / structural lines start with '#' (ignored by any CSV/JSON
//   parser). Sample lines are bare CSV "r,g,b,c" integers.
//     # ScanColorAmbientTest ready              (boot banner)
//     # pass <n> begin
//     # y=<y> x=<x>                             (before each cell's 5 samples)
//     <r>,<g>,<b>,<c>                           (one ambient-subtracted sample)
//     ... (5 sample lines per cell) ...
//     # pass <n> end
//   The scan loops continuously so the lighting can be varied across passes.
// ---------------------------------------------------------------------------

#include <stdarg.h>
#include <esp_system.h>  // esp_restart()

// Flash logging (LittleFS data partition, same one PlogDump reads). Every scan
// sample + marker is written to flash so the run can be recovered after the
// fact with PlogDump — no live serial capture needed. plog stamps each line
// with millis(), so a dumped line looks like "<millis> 1403,59637,7283,12802"
// (strip the leading number+space to get bare CSV) and "<millis> # y=0 x=0".
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

// NEW: LED illumination bank on D10, driving an NPN. D10 HIGH = LEDs ON.
const int LED_PIN = D10;
// Settle after toggling the LEDs before reading (ambient-subtraction cadence).
const int LED_SETTLE_MS = 20;

// S2/S3 select the photodiode filter bank. Datasheet names; whether they
// are physically right on this rig is unresolved and does not matter.
//
// !! CHANNEL: classifyDisc thresholds the `c` slot -- TcsFilter value 2,
// !! commanded (S2=H, S3=L). Reversed 2026-08-22 from the 2026-08-20 decision
// !! to threshold `b`; slot `b`'s two populations OVERLAP and cannot be
// !! separated by any cut. Measured on 666 cells of full-board ground truth:
// !! best achievable errors r=7 g=6 b=14 c=6, and 40 earlier jobs (26,640
// !! cells) thresholding `c` scored 0.33%. Threshold and rationale live in
// !! PARMain.ino -- keep this file in sync with it, do not re-derive here.
enum TcsFilter {
  TCS_RED = 0,    // S2=L,S3=L
  TCS_BLUE = 1,   // S2=L,S3=H -- populations OVERLAP, unusable
  TCS_CLEAR = 2,  // S2=H,S3=L <- the channel classifyDisc reads (slot `c`)
  TCS_GREEN = 3   // S2=H,S3=H -- separates ~as well as value 2, unused
};

// Servo control offloaded to a dedicated 5V Arduino Nano over a bit-banged TX
// line on Arduino D9 → 5V Nano D2 RX (SoftwareSerial; D0 is its USB debug echo), shared GND, one-way. 9600-baud software
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
  tcsSelect(TCS_CLEAR);  // idle on the channel classifyDisc reads
}

// OUT is a 50%-duty square wave whose frequency tracks light intensity for the
// active filter. pulseIn times one half-period; doubling gives the full period.
// 100 ms timeout keeps a dark / disconnected sensor from hanging.
unsigned long tcsReadFrequencyHz() {
  unsigned long halfUs = pulseIn(TCS_OUT, HIGH, 100000UL);
  if (halfUs == 0) return 0;
  return 500000UL / halfUs;
}

// Read all four channels once at the current LED/light state, 2 ms-paced (the
// train-time cadence).
void tcsReadRGBC(unsigned long& r, unsigned long& g,
                 unsigned long& b, unsigned long& c) {
  tcsSelect(TCS_RED);   delay(2); r = tcsReadFrequencyHz();
  tcsSelect(TCS_GREEN); delay(2); g = tcsReadFrequencyHz();
  tcsSelect(TCS_CLEAR); delay(2); c = tcsReadFrequencyHz();
  tcsSelect(TCS_BLUE);  delay(2); b = tcsReadFrequencyHz();
}

// One ambient-subtracted RGBC sample:
//   1. LEDs OFF, settle, read RGBC (ambient).
//   2. LEDs ON, settle, read RGBC (lit).
//   3. per channel = lit - ambient, negatives clamped to 0.
// LEDs are left OFF on exit.
void readAmbientSubtracted(long& outR, long& outG, long& outB, long& outC) {
  unsigned long ar, ag, ab, ac;  // ambient (LEDs off)
  unsigned long lr, lg, lb, lc;  // lit     (LEDs on)

  digitalWrite(LED_PIN, LOW);
  delay(LED_SETTLE_MS);
  tcsReadRGBC(ar, ag, ab, ac);

  digitalWrite(LED_PIN, HIGH);
  delay(LED_SETTLE_MS);
  tcsReadRGBC(lr, lg, lb, lc);

  digitalWrite(LED_PIN, LOW);  // leave LEDs off when idle

  outR = (long)lr - (long)ar; if (outR < 0) outR = 0;
  outG = (long)lg - (long)ag; if (outG < 0) outG = 0;
  outB = (long)lb - (long)ab; if (outB < 0) outB = 0;
  outC = (long)lc - (long)ac; if (outC < 0) outC = 0;
}

// Sensor head sits offset from the flip actuator on the gantry.
// NOTE: nudged for THIS test program only vs PARMain. SCAN_OFFSET_X = -24.005
// was CALIBRATED by jogging the head over the top-left squisk (y=0,x=0) — its
// cell center is at -752.695, the head sat at -776.700, so the offset is their
// difference. SCAN_OFFSET_Y = +8 (up 4 mm) relies on the raised Y ceiling (GRBL
// $131 / Y_TRAVEL 402 → 406 → 412); without it the top row would clamp at Y=0.
// With Y_TRAVEL=412 the top-row scan target is -14.2 + 8 = -6.2, inside the
// envelope.
const float SCAN_OFFSET_X = -24.005f;
const float SCAN_OFFSET_Y = 8.0f;

// Keep the scan target inside the Y=0 soft-limit edge (GRBL rejects target > 0
// with ALARM:2 under $20=1). Safety net so offset/pitch tweaks can't push a
// scan target past 0.
const float SCAN_Y_MAX = -0.05f;
static inline float clampScanY(float y) { return y > SCAN_Y_MAX ? SCAN_Y_MAX : y; }

// Serpentine scan order as a flat index 0..GRID_W*GRID_H-1: even rows L→R, odd
// rows R→L (row 0 starts L→R, matching the old nested-loop `ltr`). Lets the scan
// pipeline refer to "the next cell" without tracking direction inline.
static inline void cellAt(int i, int& y, int& x) {
  y = i / GRID_W;
  int col = i % GRID_W;
  x = (y & 1) ? (GRID_W - 1 - col) : col;
}
static inline float cellX(int y, int x) { return grid[y][x].x + SCAN_OFFSET_X; }
static inline float cellY(int y, int x) { return clampScanY(grid[y][x].y + SCAN_OFFSET_Y); }

// One full scan sweep. The flip arm stays parked at SERVO_US_REST the entire
// time — NEVER dropped to SCAN — so the physical board pattern is undisturbed.
// Serpentine top-to-bottom (alternating row direction so cross-row Y travel
// happens at an X soft-limit via moveToYSafe), with a mid-scan re-home after
// row 8. Per cell: 5 ambient-subtracted samples logged as bare CSV, preceded by
// a `# y=<y> x=<x>` comment line.
//
// PIPELINED to hide the (slow) flash writes behind GRBL motion: at each cell we
// (1) sense into RAM while stationary, (2) START the move to the NEXT cell —
// sendGcode hands the short G0(s) straight to GRBL, so it's already moving — and
// then (3) write this cell's samples to the flash log while that move runs,
// (4) waitForMotion before sensing the next cell. Sensing still happens only
// when the head is stationary; just the plog write overlaps the travel.
void doScan() {
  rehome();
  // Arm stays at REST for the whole scan — no servo drop. rehome() does not
  // touch the servo, so the arm never leaves REST.
  const int N = GRID_W * GRID_H;

  // Position at the first cell before the pipeline starts.
  int y0, x0;
  cellAt(0, y0, x0);
  moveToYSafe(cellX(y0, x0), cellY(y0, x0));
  waitForMotion();

  for (int i = 0; i < N; i++) {
    int y, x;
    cellAt(i, y, x);

    // 1. Sense this cell (stationary): 5 ambient-subtracted samples → RAM.
    long buf[5][4];
    for (int s = 0; s < 5; s++)
      readAmbientSubtracted(buf[s][0], buf[s][1], buf[s][2], buf[s][3]);

    // 2. Start the move to the next cell (non-blocking — GRBL begins moving).
    bool last = (i == N - 1);
    if (!last) {
      int ny, nx;
      cellAt(i + 1, ny, nx);
      if (ny == y) {
        moveTo(cellX(ny, nx), cellY(ny, nx));       // intra-row (pure X)
      } else {
        // Re-home midway (after row 8) so accumulated step drift can't skew the
        // rest of the scan; then the safe edge-leg move to the next row's start.
        if (y == 8) rehome();
        moveToYSafe(cellX(ny, nx), cellY(ny, nx));  // inter-row edge legs
      }
    }

    // 3. Save this cell's samples to flash WHILE GRBL travels to the next cell.
    plog::logf("# y=%d x=%d", y, x);
    for (int s = 0; s < 5; s++)
      plog::logf("%ld,%ld,%ld,%ld", buf[s][0], buf[s][1], buf[s][2], buf[s][3]);

    // 4. Make sure the move finished before sensing the next cell.
    if (!last) waitForMotion();
  }
}

void setup() {
  Serial.begin(115200);

  // Flash log. Do NOT clear at boot — a mid-session stall-reset (or a
  // power-cycle to stop a run) would wipe completed passes before they're ever
  // dumped (this bit us once). The log APPENDS across boots; the `booting` /
  // `pass N begin|end` / `# y= x=` markers delineate runs, and PlogDump reads
  // the accumulated tail. plog mirrors to serial (guarded) for a live monitor.
  plog::begin();
  plog::log("ScanColorAmbientTest booting — logging to flash");

  // Sensor pins + 20% scaling + LED pin (LEDs off).
  initColorSensor();

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

  plog::log("ScanColorAmbientTest ready");
}

void loop() {
  // Three passes = your control / varied / dark runs. After the third, halt so
  // the flash log holds exactly these three (power off, flash PlogDump, dump).
  const unsigned long MAX_PASSES = 3;
  static unsigned long passN = 0;

  if (passN >= MAX_PASSES) {
    plog::log("all 3 passes done — power off, flash PlogDump, and dump the log");
    while (true) delay(1000);
  }

  passN++;
  plog::logf("pass %lu begin", passN);
  doScan();
  plog::logf("pass %lu end", passN);
  delay(2000);  // pause between passes — change the lighting for the next run here
}
