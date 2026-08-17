// ScanBottomRowsTest — colour-classifier fault diagnosis on rows 16 & 17 only
// ---------------------------------------------------------------------------
// Target: the REAL P.A.R. rig — Arduino Nano ESP32 (nano_nora),
//         FQBN arduino:esp32:nano_nora.
//
// WHY THIS EXISTS
//   Two full-board ScanColorLidarTest passes over a PHYSICALLY UNIFORM board
//   produced classifier FALSE POSITIVES (ambient-subtracted BLUE < 6000 on a
//   cell whose back is not black):
//     pass 4 (top-first)    : 12 false positives, scattered; 155/666 cells in a
//                             5000..9000 grey zone; clear min 3291.
//     pass 5 (bottom-first) : 2 false positives, both on row 17 (the row
//                             scanned FIRST in that pass); clear min 3898.
//   Cross-referencing the two passes cell-by-cell: NONE of pass 4's 12 bad
//   cells is even mildly low in pass 5 (relative-to-row-median 0.64..1.08), and
//   neither of pass 5's 2 bad cells is low in pass 4 (1.41 and 0.93). So the
//   failures do NOT repeat on the same cells — but ROW 17 has the lowest row
//   median of all 18 rows in BOTH passes (9381 and 12285), regardless of
//   whether it was scanned first or last. Two effects, then: a positional one
//   (row 17 baseline sits low) and a non-repeating per-cell one that pushes an
//   already-low cell under the threshold.
//
//   The two passes also fail DIFFERENTLY, per channel. Correlating each cell's
//   channels against its own row median: pass 5 gives corr(clear, red) = 0.94 and
//   corr(clear, green) = 0.93 — a low cell is low in ALL channels, i.e. the whole
//   returned signal is down (illumination / geometry / reflectivity). Pass 4
//   gives 0.12 and -0.08 — the channels move independently, which no amount of
//   lighting or geometry can do; that is per-read measurement noise. Pass 4 also
//   ran 2.3x wider within-row spread (IQR/median 0.264 vs 0.117), red 3.4x
//   higher, and blue clamped to 0 on ~half its cells. So there are probably TWO
//   faults, and the raw off/on logging below is what tells them apart.
//
//   A full board costs ~70 min per pass, so it can only ever produce ONE
//   observation per cell. This sketch scans ONLY rows 16 and 17 (74 cells) so
//   the SAME cells can be measured N_PASSES times in ~15 min. That is the
//   experiment: a cell that fails EVERY pass is physical/positional; different
//   cells failing each pass is read noise / timing.
//
// WHAT IT DOES NOT DO
//   Scan only. NO flipping, NO display, NO WiFi, NO queue/HTTP, NO lidar. The
//   flip arm is parked at SERVO_US_REST for the entire run and is never
//   commanded to ENGAGE / RELEASE / SCAN, so the physical board pattern is left
//   exactly as found. Nothing here writes to the queue or the gallery.
//
//   It does not change SCAN_ON_BLUE_MAX or the classification maths — the
//   threshold and classifyDisc() are copied from PARMain verbatim so the logged
//   verdict is the production verdict. This sketch MEASURES; retuning is a
//   separate decision made from its output.
//
// WHAT IS LOGGED (and why each field exists)
//   Everything goes to the flash log (plog, ffat partition — recover with
//   PlogDump) AND is mirrored live to USB serial (PLOG_SERIAL_MIRROR 1 in
//   persistent_log.h). plog prefixes every line with millis(), so the whole
//   record is millisecond-stamped for free — warm-up / thermal drift is a live
//   hypothesis and time is the axis it lives on.
//
//   Per cell, per flash i (i = 0..AMBIENT_FLASHES-1):
//     o<i> <r>,<g>,<b>,<c> z<n> <bmin>,<bmax>   LEDs-OFF (ambient) 5-frame means
//     l<i> <r>,<g>,<b>,<c> z<n> <bmin>,<bmax>   LEDs-ON  (lit)     5-frame means
//   `readAmbientSubtracted()` in PARMain returns ONLY the difference, which
//   cannot distinguish "ambient term drifted up" from "lit term fell". Logging
//   the two raw terms separately is the whole point of this sketch. Also:
//     z<n>       = how many of the 20 pulseIn() calls in that state timed out
//                  and returned 0 (tcsReadFrequencyHz -> 0). A zeroed frame
//                  drags the 5-frame mean down 20%, which is exactly the size
//                  of the observed excursions. Non-zero z is the smoking gun
//                  for the pulseIn-on-FreeRTOS hypothesis.
//     bmin,bmax  = min/max of the 5 individual BLUE frames in that state, so
//                  per-frame spread is visible even when no frame times out. If
//                  single frames differ by ~2x, pulseIn is missing edges (a
//                  missed edge measures ~2x the true pulse width, i.e. ~0.5x the
//                  frequency) rather than jittering slightly.
//
//   ALL FOUR CHANNELS are logged even though only BLUE is classified, because
//   both existing full-board passes report GREEN ≈ 5-6x CLEAR (e.g. 70000 vs
//   13000). Clear is the UNFILTERED channel, so it cannot be smaller than any
//   filtered one — one of those two reads is already provably wrong, which means
//   the read path itself is suspect and not just its noise level. Whichever
//   channel is broken, the other three are the cross-check.
//   Then per cell:
//     = <r>,<g>,<b>,<c> cls<0|1> mar<b-SCAN_ON_BLUE_MAX>
//   the production ambient-subtracted result, the production verdict
//   (cls1 = "front is cyan/on", i.e. BLUE < threshold), and the signed margin
//   to the threshold, so near-misses are visible without post-processing.
//
//   Two structural knobs make the run self-interpreting:
//     STATIC_BURST      re-reads ONE cell STATIC_BURST times without moving the
//                       gantry, at the start of every pass. Any spread there is
//                       pure read noise — it cannot be positional, because the
//                       head never moves. Compare it against the spread of the
//                       same cell across passes (which does include re-approach
//                       positioning) to split read noise from positioning.
//     ALTERNATE_ROW_ORDER
//                       flips which row is scanned first on odd passes. Row 17
//                       being low in both full-board passes is consistent with
//                       BOTH "row 17 is physically low" and "the first-scanned
//                       row is low (cold)". Alternating the order inside one run
//                       separates them: if row 17 stays low when scanned
//                       second, it is positional.
//
// SHARED CODE
//   The GRBL streaming machinery, $H-first boot order, the 60 s no-progress
//   stall watchdog (esp_restart), moveToYSafe / clampScanY, the scan offsets and
//   the servo hardware-UART link are copied from PARMain.ino / the sibling scan
//   sketches. tcsReadRGBC is PARMain's 5-frame version, extended ONLY to also
//   report the timeout count and the BLUE-frame min/max (the returned means are
//   bit-identical to PARMain's).
// ---------------------------------------------------------------------------

#include <stdarg.h>
#include <esp_system.h>  // esp_restart()

#include "persistent_log.h"

const int GRID_W = 37;
const int GRID_H = 18;

// ------------------------------------------------------------ experiment knobs
// Rows under test. 16 and 17 are the bottom two bitmap rows (bitmap y=17 is the
// bottom physical row — y is mirrored when computing coordinates).
const int ROW_A = 16;
const int ROW_B = 17;

// How many times the SAME 74 cells are re-measured. This is the point of the
// sketch: repeat-count turns one observation per cell into N, which is what
// separates a cell that always fails (physical) from a different cell failing
// each time (noise / timing). ~2.5 min per pass, so 6 ≈ 15 min.
const unsigned long N_PASSES = 6;

// 0 = ROW_A first on every pass (constant order, best for pure repeatability).
// 1 = alternate: even passes ROW_A→ROW_B, odd passes ROW_B→ROW_A. Separates
//     "this row reads low" from "the first-scanned row reads low".
#define ALTERNATE_ROW_ORDER 1

// Stationary re-reads of one cell at the start of every pass. The gantry does
// not move between them, so their spread is read noise with positioning
// removed. 0 disables.
const int STATIC_BURST = 8;
const int STATIC_BURST_Y = 17;
const int STATIC_BURST_X = 18;

// Pause between passes. Kept short — the run should be one continuous thermal
// history, not N cold starts.
const unsigned long PASS_GAP_MS = 2000;

// ------------------------------------------------------------------- geometry
// CNC homes to full negatives, so the work area lives in negative coordinates.
const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 412.0f;  // MUST equal GRBL $131 — homing pins the -Y switch at -$131

// TCS3200 color sensor: S0-S3 + OUT on D4..D8 (unchanged from PARMain).
const int TCS_S0 = D4;
const int TCS_S1 = D5;
const int TCS_S2 = D6;
const int TCS_S3 = D7;
const int TCS_OUT = D8;

// LED illumination bank on D10, driving an NPN. D10 HIGH = LEDs ON.
const int LED_PIN = D10;
const int LED_SETTLE_MS = 20;

// S2/S3 select the photodiode filter bank.
//
// These labels name the filter that is PHYSICALLY selected. The S2/S3 lines are
// crossed on this rig, so the datasheet's (S2,S3) -> filter table does not hold
// for values 1 and 2 -- the value assignments below already account for that.
// Everything downstream then reads straight: `b` holds blue, `c` holds clear,
// and classifyDisc thresholds `b`.
//
// !! classifyDisc MUST THRESHOLD BLUE. Blue separates the cyan disc face from
// !! the black one by 10.22x; CLEAR manages only 2.22x, so reading CLEAR would
// !! cut usable margin from 2.17x each way to 1.24x. These two values encode the
// !! wiring, so if S2/S3 are ever rewired straight they must move with it.
enum TcsFilter {
  TCS_RED = 0,    // commanded S2=L,S3=L -> RED
  TCS_CLEAR = 1,  // commanded S2=L,S3=H -> CLEAR (datasheet says BLUE)
  TCS_BLUE = 2,   // commanded S2=H,S3=L -> BLUE  (datasheet says CLEAR)
  TCS_GREEN = 3   // commanded S2=H,S3=H -> GREEN
};

// Servo link: hardware Serial2 TX on D9 → 5V ServoNano RX, 9600 8N1, one-way.
// The ONLY pulse width this sketch ever sends is SERVO_US_REST.
const int SERVO_TX_PIN = D9;
const int SERVO_US_REST = 565;  // ≈2°, arm parked — the only angle used here
const int SERVO_90_DEG_SETTLE_MS = 300;

// The link is one-way with no ack, so a dropped byte silently LOSES a command.
// Every command is sent SERVO_TX_REPEATS times; writeMicroseconds() is
// idempotent so the repeats are free. Do not drop them.
const int SERVO_TX_REPEATS = 3;
const int SERVO_TX_REPEAT_GAP_MS = 6;

void servoTxLine(int us) {
  char buf[12];
  snprintf(buf, sizeof(buf), "\n%d\n", us);
  for (int r = 0; r < SERVO_TX_REPEATS; r++) {
    Serial2.print(buf);
    Serial2.flush();  // keep the call synchronous — callers time their settle from here
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

// Per-flash raw record, so the log can show BOTH terms of the subtraction. The
// production helper returns only (lit - ambient), which cannot distinguish an
// ambient term drifting up from a lit term falling — the central question here.
// Declared UP HERE, above every function: the Arduino preprocessor injects
// function prototypes at the top of the file, so a struct used in a signature
// must be defined before the first function or the generated prototype won't
// compile ("'FlashRec' has not been declared").
struct FlashRec {
  unsigned long ar, ag, ab, ac;  // LEDs-OFF (ambient) 5-frame means
  unsigned long lr, lg, lb, lc;  // LEDs-ON  (lit)     5-frame means
  int azeros, lzeros;            // pulseIn timeouts in each state
  unsigned long abmn, abmx;      // BLUE-frame spread, LEDs off
  unsigned long lbmn, lbmx;      // BLUE-frame spread, LEDs on
};

// grid[GRID_H-1][0] is bottom-left at the home-relative origin; physical y
// increases upward while bitmap y=0 is the top row, so y is mirrored.
Coord grid[GRID_H][GRID_W];

void initGrid() {
  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      grid[y][x].x = -X_TRAVEL + 25.0f + 20.045f * x;
      grid[y][x].y = -Y_TRAVEL + 0.0f + 23.40f * ((GRID_H - 1) - y);
    }
  }
}

// --------------------------------------------------------- GRBL streaming
// GRBL's RX buffer is 128 bytes; keep slack so we never overrun.
const int RX_BUFFER_SAFE = 120;
const int QUEUE_SIZE = 32;
const unsigned long GRBL_STALL_TIMEOUT_MS = 60000;

const int MAX_CMD_LEN = 40;
char cmdTexts[QUEUE_SIZE][MAX_CMD_LEN];
int cmdLengths[QUEUE_SIZE];
int qHead = 0;
int qTail = 0;
int bufferFill = 0;

const int MAX_ERROR_RETRIES = 10;
const unsigned long ERROR_RETRY_DELAY_MS = 3000;
int errorRetryCount = 0;
char lastErrorCmd[MAX_CMD_LEN] = "";

bool inStartupPhase = false;
volatile bool grblStartupFault = false;

// Park the servo at REST before any reset so the gantry never reboots with the
// arm mid-flip (the 5V Nano keeps driving the servo across our reset).
void parkServoForReset() {
  servoTxLine(SERVO_US_REST);
  delay(SERVO_90_DEG_SETTLE_MS);
}

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

void rawSerial1Line(const char* s) {
  Serial1.print(s);
  Serial1.write('\n');
}

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
    }
  }
  plog::logf("recovery '%s' timeout", cmd);
  return false;
}

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
      // grbl-Mega can emit a duplicate `ok` after $H; without this guard the
      // spurious ack dequeues a stale slot and desyncs bufferFill.
      if (qHead != qTail) {
        bufferFill -= dequeue();
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
      // Status reports, `$N=...`, `[MSG:...]`, welcome banner. A mid-sweep
      // `Grbl ` banner means the Mega itself reset (brownout/watchdog).
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

  // `\n` only, not `\r\n` — grbl-Mega would ack the trailing `\n` as an empty
  // line and produce a duplicate ok per command.
  Serial1.print(cmd);
  Serial1.write('\n');
  bufferFill += cmdLen;
  enqueue(cmd, cmdLen);
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

// Travel so any Y component happens with X pinned to the nearest absolute
// machine limit (X = 0 or X = -X_TRAVEL): pure-X → pure-Y → pure-X, so the Y leg
// never drags the head across the disc area at a non-limit X.
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

// `G4 P0` is a dwell GRBL syncs through the planner before acking, so once its
// `ok` lands every queued motion has actually FINISHED, not just been planned.
void waitForMotion() {
  sendGcode("G4 P0");
  waitForIdle();
}

void rehome() {
  sendGcode("$H");
  waitForIdle();
  sendGcode("$1=255");
  waitForIdle();  // sync past the $1 EEPROM write — grbl disables interrupts
                  // during the commit and drops Serial1 RX bytes
  sendGcode("G21");
  sendGcode("G90");
  waitForIdle();
}

// ----------------------------------------------------------------- the sensor
void tcsSelect(TcsFilter f) {
  digitalWrite(TCS_S2, (f & 0x02) ? HIGH : LOW);
  digitalWrite(TCS_S3, (f & 0x01) ? HIGH : LOW);
}

// S0/S1 = HIGH/LOW → 20% output frequency scaling, the regime PARMain reads in.
// Full-speed tops out near 600 kHz, past what pulseIn resolves cleanly here.
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
  tcsSelect(TCS_BLUE);  // idle on the channel classifyDisc reads
}

// Count of pulseIn() calls that hit the timeout and returned 0 since the last
// reset of the counter. UNVERIFIED ON ESP32-S3 whether this ever fires — that is
// precisely what the log is for: pulseIn() is a busy-wait on a GPIO and the
// ESP32 runs FreeRTOS, unlike the bare-metal RP2040 this code came from.
int tcsZeroReads = 0;

// OUT is a 50%-duty square wave whose frequency tracks light intensity for the
// active filter. pulseIn times one half-period; doubling gives the full period.
// 100 ms timeout keeps a dark / disconnected sensor from hanging.
unsigned long tcsReadFrequencyHz() {
  unsigned long halfUs = pulseIn(TCS_OUT, HIGH, 100000UL);
  if (halfUs == 0) {
    tcsZeroReads++;
    return 0;
  }
  return 500000UL / halfUs;
}

// PARMain's 5-frame average at the current illumination — the returned means are
// bit-identical to production. Additionally reports, for THIS state only:
//   zeros   : pulseIn timeouts across all 20 reads (5 frames × 4 channels)
//   bmn/bmx : min and max of the 5 individual BLUE frames — BLUE is the channel
//             classifyDisc thresholds, so this is the spread that matters
// so a single bad frame is visible instead of being hidden inside the mean.
void tcsReadRGBC(unsigned long& r, unsigned long& g,
                 unsigned long& b, unsigned long& c,
                 int& zeros, unsigned long& bmn, unsigned long& bmx) {
  uint32_t sr = 0, sg = 0, sb = 0, sc = 0;
  bmn = 0xFFFFFFFFUL;
  bmx = 0;
  tcsZeroReads = 0;
  for (int i = 0; i < 5; i++) {
    tcsSelect(TCS_RED);   delay(2); sr += tcsReadFrequencyHz();
    tcsSelect(TCS_GREEN); delay(2); sg += tcsReadFrequencyHz();
    tcsSelect(TCS_CLEAR); delay(2); sc += tcsReadFrequencyHz();
    tcsSelect(TCS_BLUE);  delay(2);
    unsigned long bf = tcsReadFrequencyHz();
    sb += bf;
    if (bf < bmn) bmn = bf;
    if (bf > bmx) bmx = bf;
  }
  r = sr / 5;
  g = sg / 5;
  b = sb / 5;
  c = sc / 5;
  zeros = tcsZeroReads;
}

const int AMBIENT_FLASHES = 3;

// Raw terms of the most recent readAmbientSubtractedVerbose() call. A FILE-SCOPE
// buffer rather than an out-parameter on purpose: the Arduino preprocessor
// injects generated prototypes ABOVE everything in the .ino, so a struct type in
// a function signature fails to compile no matter where the struct is defined
// ("'FlashRec' has not been declared"). One read is in flight at a time, so a
// single shared buffer is safe.
FlashRec gFlash[AMBIENT_FLASHES];

// PARMain's readAmbientSubtracted(), byte-for-byte in its arithmetic (per-flash
// clamp at 0, mean over AMBIENT_FLASHES), but it also leaves the raw per-flash
// LEDs-off / LEDs-on terms in gFlash[]. LEDs left OFF on exit.
void readAmbientSubtractedVerbose(long& r, long& g, long& b, long& c) {
  long sr = 0, sg = 0, sb = 0, sc = 0;
  for (int i = 0; i < AMBIENT_FLASHES; i++) {
    FlashRec& f = gFlash[i];

    digitalWrite(LED_PIN, LOW);
    delay(LED_SETTLE_MS);
    tcsReadRGBC(f.ar, f.ag, f.ab, f.ac, f.azeros, f.abmn, f.abmx);

    digitalWrite(LED_PIN, HIGH);
    delay(LED_SETTLE_MS);
    tcsReadRGBC(f.lr, f.lg, f.lb, f.lc, f.lzeros, f.lbmn, f.lbmx);

    long dr = (long)f.lr - (long)f.ar; if (dr < 0) dr = 0;
    long dg = (long)f.lg - (long)f.ag; if (dg < 0) dg = 0;
    long db = (long)f.lb - (long)f.ab; if (db < 0) db = 0;
    long dc = (long)f.lc - (long)f.ac; if (dc < 0) dc = 0;
    sr += dr; sg += dg; sb += db; sc += dc;
  }
  digitalWrite(LED_PIN, LOW);  // leave LEDs off when idle

  r = sr / AMBIENT_FLASHES; g = sg / AMBIENT_FLASHES;
  b = sb / AMBIENT_FLASHES; c = sc / AMBIENT_FLASHES;
}

// Production threshold and verdict, copied from PARMain — do NOT retune here.
// Classification threshold. Named for the channel it ACTUALLY reads: physically
// BLUE, not clear — see the crossed-S2/S3 block at the enum. classifyDisc
// returns the FRONT/displayed colour: 1 = cyan/on (black back, which reads LOW),
// 0 = black/off.
//
// 3535 replaces the historical 6000, computed as the GEOMETRIC MEAN of the two
// measured populations (black-back ceiling 1628, cyan-back floor 7677) so the
// margin is symmetric in ratio terms:
//     6000 -> 3.69x above the black-back ceiling but only 1.28x below the
//             cyan-back floor. Lopsided, and the thin side is where the earlier
//             full-board false positives (which read 3291..5955) came from.
//     3535 -> 2.17x margin in BOTH directions. 0/444 misclassified on the
//             two-class validation run at either value.
const long SCAN_ON_BLUE_MAX = 3535;
static inline uint8_t classifyDisc(long b) { return (b < SCAN_ON_BLUE_MAX) ? 1 : 0; }

// Sensor head offset from the flip actuator, identical to PARMain.
const float SCAN_OFFSET_X = -24.005f;
const float SCAN_OFFSET_Y = 8.0f;

// Keep the scan target inside the Y=0 soft-limit edge (GRBL rejects target > 0
// with ALARM:2 under $20=1).
const float SCAN_Y_MAX = -0.05f;
static inline float clampScanY(float y) { return y > SCAN_Y_MAX ? SCAN_Y_MAX : y; }

static inline float cellX(int y, int x) { return grid[y][x].x + SCAN_OFFSET_X; }
static inline float cellY(int y, int x) { return clampScanY(grid[y][x].y + SCAN_OFFSET_Y); }

// ------------------------------------------------------------------- the scan
// Write one cell's record: header, then per flash the LEDs-OFF (`o<i>`) and
// LEDs-ON (`l<i>`) raw 5-frame means with that state's pulseIn-timeout count and
// BLUE-frame min/max, then the production result + verdict + threshold margin.
// plog stamps every line with millis(), so the header line carries the cell's
// timestamp and `t` is not repeated. Every line stays under PLOG_MAX_LINE (96).
// `visit`: 0 = first row scanned this pass, 1 = second, -1 = stationary burst.
void logCellRecord(unsigned long pass, int visit, int y, int x,
                   long r, long g, long b, long c) {
  plog::logf("# p%lu v%d y%d x%d", pass, visit, y, x);
  for (int i = 0; i < AMBIENT_FLASHES; i++) {
    const FlashRec& f = gFlash[i];
    plog::logf("o%d %lu,%lu,%lu,%lu z%d %lu,%lu",
               i, f.ar, f.ag, f.ab, f.ac, f.azeros, f.abmn, f.abmx);
    plog::logf("l%d %lu,%lu,%lu,%lu z%d %lu,%lu",
               i, f.lr, f.lg, f.lb, f.lc, f.lzeros, f.lbmn, f.lbmx);
  }
  plog::logf("= %ld,%ld,%ld,%ld cls%u mar%ld",
             r, g, b, c, (unsigned)classifyDisc(b), b - SCAN_ON_BLUE_MAX);
}

// Measure the cell the head is already parked over and log it (no motion).
void measureAndLog(unsigned long pass, int visit, int y, int x) {
  long r, g, b, c;
  readAmbientSubtractedVerbose(r, g, b, c);
  logCellRecord(pass, visit, y, x, r, g, b, c);
}

// One row, swept in the given direction. Pure-X moves only; the caller has
// already positioned the head on the first cell via moveToYSafe.
//
// PIPELINED like the other scan sketches: sense while stationary, START the move
// to the next cell, write the log lines while GRBL travels, then waitForMotion.
// Sensing still only ever happens with the head stationary.
void scanRow(unsigned long pass, int visit, int y, bool ltr, bool moveOffAtEnd,
             int nextY, int nextX) {
  for (int k = 0; k < GRID_W; k++) {
    int x = ltr ? k : (GRID_W - 1 - k);

    long r, g, b, c;
    readAmbientSubtractedVerbose(r, g, b, c);

    bool lastInRow = (k == GRID_W - 1);
    if (!lastInRow) {
      int nx = ltr ? (x + 1) : (x - 1);
      moveTo(cellX(y, nx), cellY(y, nx));            // intra-row (pure X)
    } else if (moveOffAtEnd) {
      moveToYSafe(cellX(nextY, nextX), cellY(nextY, nextX));  // cross-row edge legs
    }

    logCellRecord(pass, visit, y, x, r, g, b, c);

    if (!lastInRow || moveOffAtEnd) waitForMotion();
  }
}

// One pass over both rows. The arm never leaves REST — rehome() does not touch
// the servo and nothing else here commands it.
void doPass(unsigned long pass) {
  rehome();

#if ALTERNATE_ROW_ORDER
  bool aFirst = ((pass & 1UL) == 1UL);  // pass 1 → ROW_A first, pass 2 → ROW_B first, ...
#else
  bool aFirst = true;
#endif
  const int firstRow  = aFirst ? ROW_A : ROW_B;
  const int secondRow = aFirst ? ROW_B : ROW_A;

  plog::logf("# pass %lu rows %d then %d", pass, firstRow, secondRow);

  // Stationary burst: N reads of ONE cell with the gantry parked. Any spread
  // here is read noise only — positioning is held constant by construction.
  if (STATIC_BURST > 0) {
    moveToYSafe(cellX(STATIC_BURST_Y, STATIC_BURST_X), cellY(STATIC_BURST_Y, STATIC_BURST_X));
    waitForMotion();
    plog::logf("# static burst p%lu y%d x%d n%d",
               pass, STATIC_BURST_Y, STATIC_BURST_X, STATIC_BURST);
    for (int s = 0; s < STATIC_BURST; s++) measureAndLog(pass, -1, STATIC_BURST_Y, STATIC_BURST_X);
    plog::logf("# static burst end p%lu", pass);
  }

  // First row: L→R. Ends at x=36 (X near the 0 limit), so the cross-row leg
  // starts from the nearest limit and the second row is swept R→L — serpentine,
  // exactly as the production scan does it.
  moveToYSafe(cellX(firstRow, 0), cellY(firstRow, 0));
  waitForMotion();
  scanRow(pass, 0, firstRow, true, true, secondRow, GRID_W - 1);

  // Second row: R→L, ending at x=0 (X near the -X_TRAVEL limit).
  scanRow(pass, 1, secondRow, false, false, -1, -1);
}

void setup() {
  Serial.begin(115200);

  // Do NOT clear the log at boot — a stall-reset or a power-cycle mid-run would
  // wipe completed passes before they were ever dumped (this bit us once). The
  // log APPENDS across boots; the `booting` / `pass N begin|end` markers
  // delineate runs and PlogDump reads the accumulated tail.
  plog::begin();
  plog::log("ScanBottomRowsTest booting — logging to flash");
  plog::logf("# rows %d,%d passes %lu flashes %d thresh %ld alt %d burst %d",
             ROW_A, ROW_B, N_PASSES, AMBIENT_FLASHES, SCAN_ON_BLUE_MAX,
             (int)ALTERNATE_ROW_ORDER, STATIC_BURST);

  // Reset cause: a brownout/WDT mid-run changes how the log should be read.
  {
    esp_reset_reason_t rr = esp_reset_reason();
    plog::logf("reset cause: %d", (int)rr);
  }

  initColorSensor();

  // Servo link up, arm parked at REST. This is the ONLY servo command in the
  // whole sketch — no ENGAGE, no RELEASE, no SCAN drop.
  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);
  delay(100);
  writeServoUs(SERVO_US_REST, SERVO_90_DEG_SETTLE_MS);

  // GRBL link. $H FIRST — GRBL boots into alarm and rejects modal G-code with
  // error:9 until homed — then G21/G90. Serial0 owns D0/D1 by default on the
  // Nano ESP32; hand them to Serial1 so the GRBL link keeps its wires.
  Serial0.end();
  Serial1.begin(115200, SERIAL_8N1, D0, D1);
  delay(2000);  // GRBL boot wait

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

    sendGcode("$1=255");  // keep steppers energized while idle
    waitForIdle();        // sync past the EEPROM write before pipelining more
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

  plog::log("ScanBottomRowsTest ready");
}

void loop() {
  static unsigned long passN = 0;

  if (passN >= N_PASSES) {
    plog::logf("all %lu passes done — power off, flash PlogDump, and dump the log", N_PASSES);
    while (true) delay(1000);
  }

  passN++;
  plog::logf("pass %lu begin", passN);
  doPass(passN);
  plog::logf("pass %lu end", passN);
  delay(PASS_GAP_MS);
}
