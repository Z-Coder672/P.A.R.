#include <WiFiNINA.h>
#include <ArduinoHttpClient.h>
#include <base64.hpp>
#include "env.h"
#include "classifier.h"      // tiny ternary transformer for blue/black RGBC
#include "persistent_log.h"  // flash-backed log for WiFi/HTTP/poll events only
#include "hardware/spi.h"

const char* SSID = "ab guest";
const char* PASSWORD = WIFI_PASSWORD;
const char* SERVER = "par.zimmzimm.com";
const int PORT = 443;

WiFiSSLClient wifi;
HttpClient client(wifi, SERVER, PORT);

const int GRID_W = 37;
const int GRID_H = 18;

// CNC homes to full negatives, so the work area lives in negative coordinates.
const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 399.695f;

// TCS3200 color sensor: S0-S3 + OUT on D4..D8.
// Defined before any function so the Arduino IDE's auto-prototype generator
// (which injects forward declarations just above the first function) sees
// TcsFilter as a valid type when it forward-declares tcsSelect(TcsFilter).
const int TCS_S0 = 4;
const int TCS_S1 = 5;
const int TCS_S2 = 6;
const int TCS_S3 = 7;
const int TCS_OUT = 8;

// S2/S3 select the photodiode filter bank.
enum TcsFilter {
  TCS_RED = 0,    // S2=L, S3=L
  TCS_BLUE = 1,   // S2=L, S3=H
  TCS_CLEAR = 2,  // S2=H, S3=L
  TCS_GREEN = 3   // S2=H, S3=H
};

const int SERVO_PIN = 9;
// Pulse widths match the standard Servo lib mapping
// (MIN_PULSE_WIDTH=544, MAX_PULSE_WIDTH=2400 over 0–180°) so the angles the
// rig was tuned for stay the same: REST≈0° (parked), RELEASE≈38° (pushes the
// half-rotated squisk during the X slide-back), ENGAGE≈90° (initial 90° flip).
const int SERVO_US_REST = 544;
const int SERVO_US_RELEASE = 936;
const int SERVO_US_ENGAGE = 1471;
const int SERVO_90_DEG_SETTLE_MS = 300;
const int SERVO_50_DEG_SETTLE_MS = 100;
// Second-catch arm angle for the error-reduction pass: ~10° below RELEASE.
// Pulse mapping is 544–2400µs over 0–180° (~10.3µs/°), so 10° ≈ 103µs. Derived
// from RELEASE minus a computed offset rather than a hardcoded µs value, so
// retuning RELEASE carries this with it.
const int SERVO_US_10_DEG = 103;
const int SERVO_US_RELEASE2 = SERVO_US_RELEASE - SERVO_US_10_DEG;
const int SERVO_10_DEG_SETTLE_MS = 100;
const float FLIP_OFFSET_X = 16.8f;

// Step-3 second-catch pass: after the main flip+catch, drop the arm a further
// ~10° (to RELEASE2, ~28°) and sweep +X once more to push back any disc the
// first catch left over/under-rotated. Comment this out to remove the
// second-catch back-move (the main flip then runs without the extra pass).
//#define FLIP_SECOND_CATCH

// Servo control offloaded to a dedicated 5V Arduino Nano over a bit-banged TX
// line on Arduino D9 (the old SERVO_PIN, freed once the SG90 moved to the 5V
// Nano) → 5V Nano D0 RX, shared GND, one-way. Going through mbed's UART class
// on an arbitrary PinName crashed the chip, so the safer path is a software
// UART: 9600 baud is forgiving enough for bit-bang on the mbed core where ISR
// jitter from WiFiNINA SPI and GRBL Serial1 RX would chew up the bit timing at
// faster rates. The companion sketch (ServoNano.ino) listens on its hardware
// UART at the same baud and parses an integer µs value per line.
const int SERVO_TX_PIN = 9;
// 1/9600 ≈ 104.17 µs. mbed digitalWrite costs ~2 µs, so trim the delay to keep
// total bit width close to 104 µs and avoid cumulative drift across the frame.
const int SERVO_TX_BIT_US = 102;

void servoTxByte(uint8_t b) {
  noInterrupts();
  digitalWrite(SERVO_TX_PIN, LOW);
  delayMicroseconds(SERVO_TX_BIT_US);
  for (int i = 0; i < 8; i++) {
    digitalWrite(SERVO_TX_PIN, (b >> i) & 1);
    delayMicroseconds(SERVO_TX_BIT_US);
  }
  digitalWrite(SERVO_TX_PIN, HIGH);
  interrupts();
  delayMicroseconds(SERVO_TX_BIT_US);  // stop bit (idle high)
}

void servoTxLine(int us) {
  char buf[12];
  int n = snprintf(buf, sizeof(buf), "%d\n", us);
  for (int i = 0; i < n; i++) servoTxByte((uint8_t)buf[i]);
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

// Currently-displayed disc colors: 0 = black, 1 = blue.
uint8_t gridState[GRID_H][GRID_W];

// Fixed-size buffer (digits-only id from the server) — avoids Arduino String
// heap fragmentation across many loop iterations. Empty when no display is
// pending confirmation.
char pendingGalleryId[16] = "";

void initGrid() {
  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      //           starting offset⌄
      // Starting X offset is 25 mm; the addressable grid covers physical
      // cols 0..36 as logical x=0..36.
      grid[y][x].x = -X_TRAVEL + 25.0f + 20.045f * x;
      grid[y][x].y = -Y_TRAVEL + 0.0f + 23.40f * ((GRID_H - 1) - y);
      //                                  ⌃grid spacing
      gridState[y][x] = 0;
    }
  }
}

// GRBL character-counting streaming protocol. GRBL's RX buffer is 128 bytes;
// keep a few bytes of slack so we never overrun.
const int RX_BUFFER_SAFE = 120;
const int QUEUE_SIZE = 32;

// If sendGcode/waitForIdle stall this long without seeing any GRBL progress
// (no `ok` consumed, no buffer drain), assume comms with the Mega are wedged
// and force an MCU reset so the device recovers on its own. 60s is well above
// any legitimate motion or homing time we issue at this granularity.
const unsigned long GRBL_STALL_TIMEOUT_MS = 60000;

// We also keep the command text per slot so error:N can be retried (re-sent)
// without the caller knowing. 40 bytes matches the largest snprintf buffer
// used by moveTo()/flipDisc()/etc. Anything longer than that is a bug.
const int MAX_CMD_LEN = 40;
char cmdTexts[QUEUE_SIZE][MAX_CMD_LEN];
int cmdLengths[QUEUE_SIZE];
int qHead = 0;
int qTail = 0;
int bufferFill = 0;

// error:N retry bookkeeping. When the same command errors repeatedly we cap
// at MAX_ERROR_RETRIES then MCU-reset as a last resort.
const int MAX_ERROR_RETRIES = 10;
const unsigned long ERROR_RETRY_DELAY_MS = 3000;
int errorRetryCount = 0;
char lastErrorCmd[MAX_CMD_LEN] = "";

// During setup's homing phase, an `error`/`ALARM` from GRBL is recoverable —
// bounce Serial1 and retry rather than wedging forever. Outside setup we keep
// the legacy hard-halt behavior so a fault mid-job stops the rig immediately.
bool inStartupPhase = false;
volatile bool grblStartupFault = false;

// True when gridState[] reflects a scan that's still trustworthy — i.e.
// nothing has happened since that could have disturbed the board (no $1=0
// idle, no power cycle). Set after each scanGrid() completes, cleared right
// before the motors-off idle at the end of a job. Lets us skip the redundant
// re-scan on the first job after boot.
bool gridStateFresh = false;

// Park the servo at REST before any reset so the gantry doesn't reboot with the
// arm mid-flip — leaving it engaged can foul the next homing pass. The 5V Nano
// keeps driving the servo across our reset, so a single µs command is enough.
void parkServoForReset() {
  servoTxLine(SERVO_US_REST);
  delay(SERVO_90_DEG_SETTLE_MS);
}

// RP2040: NVIC_SystemReset() is exposed by the mbed core's CMSIS headers.
void grblStallReset(const char* where) {
  plog::logf("GRBL stall in %s -> MCU reset", where);
  parkServoForReset();
  delay(50);
  NVIC_SystemReset();
}

// WiFi reconnect: per-attempt association timeout, infinite retries, with a
// total stall watchdog that mirrors the GRBL pattern — if we can't get back on
// the network within 60s, reset the MCU so a wedged WiFiNINA module recovers
// on its own instead of silently failing every HTTPS call forever.
const unsigned long WIFI_ATTEMPT_TIMEOUT_MS = 10000;
const unsigned long WIFI_STALL_TIMEOUT_MS = 60000;

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  plog::log("wifi (re)connecting");
  unsigned long stallT0 = millis();
  for (int attempt = 1;; attempt++) {
    WiFi.disconnect();
    WiFi.end();
    _spi_init(spi1, 8000000);
    delay(1000);  // give Nina ESP32 time to fully shut down before re-init
    WiFi.begin(SSID, PASSWORD);
    unsigned long t0 = millis();
    while (millis() - t0 < WIFI_ATTEMPT_TIMEOUT_MS) {
      int s = WiFi.status();
      plog::logf("a=%d t=%lu s=%d", attempt, millis() - t0, s);
      if (s == WL_CONNECTED) {
        plog::logf("wifi connected attempt=%d rssi=%d",
                   attempt, (int)WiFi.RSSI());
        return;
      }
      delay(500);
    }
    // Log the actual status code so we know what state it's stuck in
    plog::logf("wifi attempt %d timeout status=%d", attempt, (int)WiFi.status());
    if (millis() - stallT0 > WIFI_STALL_TIMEOUT_MS) {
      plog::log("wifi stall -> MCU reset");
      parkServoForReset();
      delay(50);
      NVIC_SystemReset();
    }
  }
}

void enqueue(const char* cmd, int len) {
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
// recovery paths (drainResponses retry, ALARM recovery) which manage buffer
// accounting themselves.
void rawSerial1Line(const char* s) {
  Serial1.print(s);
  Serial1.write('\n');
}

// Synchronous "send + wait for ok" used during ALARM recovery. Does not use
// the queue — recovery runs with bufferFill=0 and we want one ack at a time.
// Returns true on ok, false on timeout/error/ALARM (caller will MCU-reset).
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
// reassert modal state. On any sub-step failure we MCU-reset as a fallback —
// matches the existing stall-watchdog policy. Clears the command queue on
// success and lets the in-flight sendGcode/waitForIdle return as if the
// pending motion completed; whatever the caller was mid-way through is lost,
// but the rig stays alive.
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
    NVIC_SystemReset();
  }
  if (!recoverySendAndWait("$1=255", 5000) ||
      !recoverySendAndWait("G21", 5000) ||
      !recoverySendAndWait("G90", 5000)) {
    plog::log("ALARM recovery modal-set failed -> MCU reset");
    parkServoForReset();
    delay(50);
    NVIC_SystemReset();
  }

  // The board state is unknown after an ALARM — force a re-scan next job.
  gridStateFresh = false;
  plog::log("ALARM recovery complete");
}

// (USB Serial removed — flash log is the only debug trail. Serial1 is the
// GRBL UART and is retained.)

void drainResponses() {
  while (Serial1.available()) {
    String resp = Serial1.readStringUntil('\n');
    resp.trim();
    if (resp.length() == 0) continue;

    if (resp == "ok") {
      // grbl-Mega can emit a duplicate `ok` after $H (one when alarm clears,
      // one when homing completes). Without this guard the spurious ack
      // dequeues a stale slot, desyncing bufferFill so waitForIdle hangs.
      if (qHead != qTail) {
        bufferFill -= dequeue();
        // A clean ok in between errors resets the consecutive-error counter —
        // we only MCU-reset when the *same* command keeps failing.
        errorRetryCount = 0;
        lastErrorCmd[0] = '\0';
      }
    } else if (resp.startsWith("ALARM")) {
      plog::logf("GRBL ALARM: %.40s", resp.c_str());
      if (inStartupPhase) {
        grblStartupFault = true;
        return;
      }
      grblAlarmRecover();
      // After recovery the queue is empty. The caller's send-buffer-wait or
      // waitForIdle loop will see bufferFill==0 and exit cleanly.
      return;
    } else if (resp.startsWith("error")) {
      if (inStartupPhase) {
        plog::logf("GRBL startup error: %.40s", resp.c_str());
        grblStartupFault = true;
        return;
      }
      // error:N is non-fatal — GRBL still consumed the line (char-counting
      // protocol acks error the same as ok), so drop the queue slot, then
      // re-send the same command after a delay. Cap at MAX_ERROR_RETRIES for
      // the *same* command in a row before giving up and MCU-resetting.
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
        NVIC_SystemReset();
      }

      delay(ERROR_RETRY_DELAY_MS);

      // Re-send inline (don't recurse through sendGcode — we're already in
      // its drain loop). Subsequent commands queued behind this one were
      // sent earlier and may already be in flight; re-enqueuing at tail
      // means the retry runs *after* them. For the small atomic commands
      // that typically error (modal switches, $-settings) that ordering is
      // benign; motion-sensitive sequences would have already faulted via
      // ALARM rather than error.
      int rlen = strlen(failedCmd) + 1;
      rawSerial1Line(failedCmd);
      bufferFill += rlen;
      strncpy(cmdTexts[qTail], failedCmd, MAX_CMD_LEN - 1);
      cmdTexts[qTail][MAX_CMD_LEN - 1] = '\0';
      cmdLengths[qTail] = rlen;
      qTail = (qTail + 1) % QUEUE_SIZE;
    }
    // Anything else — status reports `<...>` from `?`, settings lines `$N=...`
    // from `$$`, welcome banner `Grbl ...`, `[MSG:...]`, `ALARM:...` — is not
    // tied to a queued command, so don't dequeue and don't halt.
  }
}

void sendGcode(const char* cmd) {
  int cmdLen = strlen(cmd) + 1;  // +1 for the newline GRBL counts

  // Reset the stall timer every time the buffer actually drains so a
  // long-but-progressing motion sequence doesn't trip the watchdog.
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

  // Send `\n` only, not `\r\n` — grbl-Mega treats `\r` as a line end then
  // acks the trailing `\n` as an empty line, producing a duplicate ok per
  // command. The duplicate desyncs cmdLengths queue accounting.
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

// Travel to (targetX, targetY) such that any vertical (Y) component happens
// with X pinned to the nearest absolute machine limit (X = 0 or X = -X_TRAVEL).
// Emits pure-X → pure-Y → pure-X, so the Y leg never drags the head across
// the disc area at a non-limit X. Use for entry into a phase or any cross-row
// transition; within-row moves can use plain moveTo (Y is constant there).
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

void tcsSelect(TcsFilter f) {
  digitalWrite(TCS_S2, (f & 0x02) ? HIGH : LOW);
  digitalWrite(TCS_S3, (f & 0x01) ? HIGH : LOW);
}

// OUT is a 50%-duty square wave whose frequency tracks light intensity for
// the active filter. pulseIn times one half-period; doubling gives the full
// period. 100 ms timeout keeps a dark / disconnected sensor from hanging.
unsigned long tcsReadFrequencyHz() {
  unsigned long halfUs = pulseIn(TCS_OUT, HIGH, 100000UL);
  if (halfUs == 0) return 0;
  return 500000UL / halfUs;
}

// Average 5 consecutive RGBC frames at the current head position. The model
// was trained on 5-step running averages of 2ms-paced reads, so we match
// that distribution here. End-to-end this is ~60-80 ms per cell — actually
// faster than the previous single 4×50ms read, while feeding the classifier
// the input distribution it expects.
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

// Sensor head sits offset from the flip actuator on the gantry.
const float SCAN_OFFSET_X = -23.0f;
const float SCAN_OFFSET_Y = 4.0f;

// Squisk faces: 1 = blue (#40ccdb), 0 = black.
// Tiny ternary transformer (~99.85% test acc on 5-step averaged RGBC). Replaces
// the prior B/C ratio threshold; weights live in model_weights.h, regenerate
// via `Color Sensor ML/export_header.py` after retraining.
uint8_t classifyDisc(unsigned long r, unsigned long g,
                     unsigned long b, unsigned long c) {
  float rgbc[4] = { (float)r, (float)g, (float)b, (float)c };
  return classifier_is_blue(rgbc) ? 1 : 0;
}

// Re-home + reassert mm/absolute modes. $1=255 lives in EEPROM so it would
// survive on its own, but matching the boot sequence keeps state predictable.
void rehome() {
  sendGcode("$H");
  waitForIdle();
  sendGcode("$1=255");
  sendGcode("G21");
  sendGcode("G90");
  waitForIdle();
}

void scanGrid() {
  rehome();
  // Serpentine top-to-bottom: alternating row direction so the only Y travel
  // between rows happens at an X soft-limit (handled by moveToYSafe).
  bool ltr = true;
  moveToYSafe(grid[0][0].x + SCAN_OFFSET_X,
              grid[0][0].y + SCAN_OFFSET_Y);
  for (int y = 0; y < GRID_H; y++) {
    int startCol = ltr ? 0 : GRID_W - 1;
    int endCol = ltr ? GRID_W - 1 : 0;
    int step = ltr ? +1 : -1;
    for (int x = startCol; x != endCol + step; x += step) {
      moveTo(grid[y][x].x + SCAN_OFFSET_X,
             grid[y][x].y + SCAN_OFFSET_Y);
      waitForMotion();

      unsigned long r, g, b, c;
      tcsReadRGBC(r, g, b, c);
      uint8_t color = classifyDisc(r, g, b, c);
      gridState[y][x] = color;
    }
    if (y + 1 < GRID_H) {
      // Re-home midway (after the 9th row) so accumulated step drift can't
      // skew the rest of the scan.
      if (y == 8) rehome();
      moveToYSafe(grid[y + 1][endCol].x + SCAN_OFFSET_X,
                  grid[y + 1][endCol].y + SCAN_OFFSET_Y);
      ltr = !ltr;
    }
  }
  gridStateFresh = true;
}

// `G4 P0` is a dwell that GRBL syncs through the planner before acking, so
// once its `ok` lands every queued motion has actually finished — not just
// been planned. Pair with waitForIdle() before any non-GRBL action.
void waitForMotion() {
  sendGcode("G4 P0");
  waitForIdle();
}

// Two-stage 180° flip, plus a second error-reduction catch pass:
//   1) Servo to ENGAGE (~90°) above the disc — this rotates the squisk 90°.
//      Servo back to REST. The squisk is now half-flipped and stays put.
//   2) Slide X by +16.8 mm so the arm clears the disc column, drop the arm to
//      RELEASE (~38°), then slide X back. The arm catches the half-rotated
//      squisk and pushes it through the final 90° during the −dx return slide.
//   3) Second catch: drop the arm a further ~10° (RELEASE2) and sweep once
//      more in the +X direction (opposite the −dx return) over ≥16.8 mm, to
//      push back any disc the first catch left over/under-rotated.
//
// `catchByNextMove` lets the caller fold step 3 into a move it was already
// going to make. The required sweep is always +X by ≥FLIP_OFFSET_X; when the
// caller knows the next motion already travels +X by at least that much (an
// LTR row with another flip ahead, since the cell pitch 20.045 mm > 16.8 mm),
// pass true: we set the arm to RELEASE2 and return with it still down, so the
// caller's next move performs the catch — no extra G-code, no extra stop.
// Otherwise (RTL rows, row ends, last flip) pass false and we emit an explicit
// +X stroke and re-park the arm at REST.
void flipDisc(int gx, int gy, bool catchByNextMove) {
  moveTo(grid[gy][gx].x, grid[gy][gx].y);
  waitForMotion();

  writeServoUs(SERVO_US_ENGAGE, SERVO_90_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_REST, SERVO_90_DEG_SETTLE_MS);

  // Cap the X excursion so the repositioning slide and the matching finish
  // slide never command a position past X=0 (the work area is entirely
  // negative).
  float dx = FLIP_OFFSET_X;
  if (grid[gy][gx].x + dx > 0.0f) dx = -grid[gy][gx].x;

  char cmd[32];
  sendGcode("G91");
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", dx);
  sendGcode(cmd);
  sendGcode("G90");
  waitForMotion();

  writeServoUs(SERVO_US_RELEASE, SERVO_50_DEG_SETTLE_MS);

  sendGcode("G91");
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", -dx);
  sendGcode(cmd);
  sendGcode("G90");
  waitForMotion();

#ifdef FLIP_SECOND_CATCH
  // Step 3 — second catch pass. Drop the arm ~10° below RELEASE first either
  // way; the difference is only whether we emit our own +X stroke.
  writeServoUs(SERVO_US_RELEASE2, SERVO_10_DEG_SETTLE_MS);
  if (!catchByNextMove) {
    // Same X=0 cap so the +X stroke never commands past the soft limit.
    float dx2 = FLIP_OFFSET_X;
    if (grid[gy][gx].x + dx2 > 0.0f) dx2 = -grid[gy][gx].x;
    sendGcode("G91");
    snprintf(cmd, sizeof(cmd), "G0 X%.3f", dx2);
    sendGcode(cmd);
    sendGcode("G90");
    waitForMotion();
    writeServoUs(SERVO_US_REST, SERVO_10_DEG_SETTLE_MS);
  }
#else
  // Second catch disabled — there's no extra pass to leave the arm down for, so
  // park it at REST regardless of catchByNextMove. The main flip+catch stands
  // alone and the caller's next move runs with the arm parked.
  (void)catchByNextMove;
  writeServoUs(SERVO_US_REST, SERVO_50_DEG_SETTLE_MS);
#endif
}

uint8_t bitmapBit(const uint8_t* bitmap, int x, int y) {
  int idx = y * GRID_W + x;
  return (bitmap[idx / 8] >> (7 - (idx % 8))) & 1;
}

// Bottom-to-top (bitmap y = GRID_H-1 → 0) over the band of rows containing at
// least one flip. Rows alternate sweep direction (serpentine), so the only Y
// motion between rows happens at an X soft-limit via moveToYSafe — never a
// diagonal or pure-Y at non-limit X.
void displayBitmap(uint8_t* bitmap) {
  int firstY = -1;  // bottom-most changing row (largest bitmap-y)
  int lastY = -1;   // top-most changing row (smallest bitmap-y)
  for (int y = GRID_H - 1; y >= 0; y--) {
    for (int x = 0; x < GRID_W; x++) {
      if (bitmapBit(bitmap, x, y) != gridState[y][x]) {
        if (firstY < 0) firstY = y;
        lastY = y;
        break;
      }
    }
  }
  if (firstY < 0) return;

  bool ltr = true;
  moveToYSafe(grid[firstY][0].x, grid[firstY][0].y);
  for (int y = firstY; y >= lastY; y--) {
    int startCol = ltr ? 0 : GRID_W - 1;
    int endCol = ltr ? GRID_W - 1 : 0;
    int step = ltr ? +1 : -1;
    // Collect the columns needing a flip in sweep order so each flip knows
    // whether another one follows it in this row. On an LTR row the next flip
    // sits at a larger X, so the next flipDisc's opening move already travels
    // +X by ≥ one cell pitch (20.045 mm > 16.8 mm) — exactly the second-catch
    // sweep — and we let flipDisc fold its catch into that move.
    int flipCols[GRID_W];
    int nFlips = 0;
    for (int x = startCol; x != endCol + step; x += step) {
      if (bitmapBit(bitmap, x, y) != gridState[y][x]) flipCols[nFlips++] = x;
    }
    for (int i = 0; i < nFlips; i++) {
      int x = flipCols[i];
      bool catchByNextMove = ltr && (i + 1 < nFlips);
      flipDisc(x, y, catchByNextMove);
      gridState[y][x] = bitmapBit(bitmap, x, y);
    }
    moveTo(grid[y][endCol].x, grid[y][endCol].y);
    waitForMotion();
    if (y > lastY) {
      moveToYSafe(grid[y - 1][endCol].x, grid[y - 1][endCol].y);
      ltr = !ltr;
    }
  }
}

void setup() {
  // Servo is driven by a dedicated 5V Arduino Nano over Serial2 (D21 TX → Nano
  // D0 RX, shared GND). One-way link; the companion sketch parses integer µs
  // values per line. Bring the UART up first so the very first park command
  // below is actually received.
  pinMode(SERVO_TX_PIN, OUTPUT);
  digitalWrite(SERVO_TX_PIN, HIGH);  // UART idle = high
  delay(100);
  servoTxLine(SERVO_US_REST);

  delay(10000);
  Serial1.begin(115200);
  // The 2-second delay below covers the GRBL/Serial1 boot wait. USB Serial
  // is intentionally not initialized — when the rig runs headless on wall
  // power, an undrained CDC stream stalls writes; the flash log is the only
  // debug trail.

  // Mount the flash log so this boot's WiFi/HTTP/poll events get recorded.
  // The log persists across resets.
  plog::begin();
  plog::log("boot");

  initGrid();

  pinMode(TCS_S0, OUTPUT);
  pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT);
  pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT, INPUT);
  // S0/S1 = 1/0 → 20% output frequency scaling. Full-speed (HIGH/HIGH) tops
  // out near 600 kHz, which is past what pulseIn can resolve cleanly on the
  // RP2040 here; 20% keeps us well inside that envelope.
  digitalWrite(TCS_S0, HIGH);
  digitalWrite(TCS_S1, LOW);
  tcsSelect(TCS_CLEAR);

  delay(2000);  // GRBL boot wait (Serial1) + servo settle

  // Retry homing on GRBL error/ALARM: bounce Serial1 (forces GRBL to re-init
  // its half of the link) and re-run the full startup sequence. Without this,
  // a power-on alarm during $H would wedge the rig until a manual reset.
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

    // $1=255 keeps steppers energized while idle so the gantry holds position
    // between motions; we drop back to $1=0 + a tiny jog at job end to release.
    sendGcode("$1=255");
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
    Serial1.begin(115200);
    delay(2000);  // GRBL boot wait after re-opening Serial1
  }
  inStartupPhase = false;

  // scanGrid();

  client.setHttpResponseTimeout(15000);
}

// Try /complete.php with exponential backoff for up to 5 minutes. If the
// server is reachable but returns a non-2xx, or the network is flaky, we keep
// retrying so a transient blip doesn't strand the gallery entry as pending.
// After the 5-minute budget is exhausted we give up and clear pendingGalleryId
// — the entry stays as pending.json server-side; same behavior as a verify-fix
// exhaustion. Caller is expected to always reach this only after a clean
// verify pass.
void onDisplayComplete() {
  if (pendingGalleryId[0] == '\0') return;
  plog::logf("complete.php start id=%s", pendingGalleryId);

  const unsigned long TOTAL_BUDGET_MS = 5UL * 60UL * 1000UL;
  unsigned long backoff = 1000;
  unsigned long start = millis();

  while (millis() - start < TOTAL_BUDGET_MS) {
    bool attemptOk = false;
    WiFiSSLClient ssl;
    if (ssl.connect(SERVER, PORT)) {
      ssl.print("GET /complete.php?id=");
      ssl.print(pendingGalleryId);
      ssl.print(" HTTP/1.1\r\n");
      ssl.print("Host: ");
      ssl.print(SERVER);
      ssl.print("\r\n");
      ssl.print("User-Agent: P.A.R./1.0\r\n");
      ssl.print("Accept: */*\r\n");
      ssl.print("X-Snapshot-Secret: ");
      ssl.print(SNAPSHOT_SECRET);
      ssl.print("\r\n");
      ssl.print("Connection: close\r\n");
      ssl.print("\r\n");

      String statusLine = ssl.readStringUntil('\n');
      ssl.stop();
      // statusLine looks like "HTTP/1.1 200 OK". Treat any 2xx as success.
      int sp = statusLine.indexOf(' ');
      if (sp >= 0 && statusLine.charAt(sp + 1) == '2') attemptOk = true;
    } else {
      plog::log("complete.php connect() failed");
    }

    if (attemptOk) {
      plog::logf("complete.php ok id=%s", pendingGalleryId);
      pendingGalleryId[0] = '\0';
      return;
    }

    plog::logf("complete.php retry in %lu ms", backoff);
    delay(backoff);
    backoff *= 2;
    if (backoff > 60000UL) backoff = 60000UL;
  }

  plog::logf("complete.php abandoned id=%s", pendingGalleryId);
  pendingGalleryId[0] = '\0';
}

// Fire-and-forget POST to /snapshot-request.php telling the Mac Mini that the
// board is now settled and a snapshot should be captured for this gallery id.
// Called after the check pass (scan + re-fix) so the photo reflects the final
// corrected state, not the first-pass draw. No retries — a missed snapshot
// just leaves gallery/<id>/image.* absent; the modal already falls back to
// "No P.A.R. image available."
void sendSnapshotRequest(const char* galleryId) {
  if (!galleryId || galleryId[0] == '\0') return;
  plog::logf("snapshot-request start id=%s", galleryId);

  WiFiSSLClient ssl;
  if (!ssl.connect(SERVER, PORT)) {
    plog::log("snapshot-request connect() failed");
    return;
  }

  char body[64];
  int bodyLen = snprintf(body, sizeof(body), "id=%s", galleryId);
  if (bodyLen <= 0 || bodyLen >= (int)sizeof(body)) {
    ssl.stop();
    return;
  }

  ssl.print("POST /snapshot-request.php HTTP/1.1\r\n");
  ssl.print("Host: ");
  ssl.print(SERVER);
  ssl.print("\r\n");
  ssl.print("User-Agent: P.A.R./1.0\r\n");
  ssl.print("Accept: */*\r\n");
  ssl.print("Content-Type: application/x-www-form-urlencoded\r\n");
  ssl.print("Content-Length: ");
  ssl.print(bodyLen);
  ssl.print("\r\n");
  ssl.print("X-Snapshot-Secret: ");
  ssl.print(SNAPSHOT_SECRET);
  ssl.print("\r\n");
  ssl.print("Connection: close\r\n");
  ssl.print("\r\n");
  ssl.print(body);

  unsigned long t0 = millis();
  while (ssl.connected() && !ssl.available()) {
    if (millis() - t0 > 10000) break;
    delay(10);
  }
  String statusLine = ssl.readStringUntil('\n');
  ssl.stop();
  int sp = statusLine.indexOf(' ');
  if (sp >= 0 && statusLine.charAt(sp + 1) == '2') {
    plog::logf("snapshot-request ok id=%s", galleryId);
  } else {
    plog::logf("snapshot-request unexpected: %.40s", statusLine.c_str());
  }
}

// Fire-and-forget POST to /stream-end-set.php telling the Mac Mini to stop
// the recording it started when this print began. Called after the 10-min
// post-display idle. No retries — if it fails, the Mac's 1.5h hard cap
// closes the recording anyway.
void sendStreamEnd(const char* galleryId) {
  if (!galleryId || galleryId[0] == '\0') return;
  plog::logf("stream-end-set start id=%s", galleryId);

  WiFiSSLClient ssl;
  if (!ssl.connect(SERVER, PORT)) {
    plog::log("stream-end-set connect() failed");
    return;
  }

  char body[64];
  int bodyLen = snprintf(body, sizeof(body), "id=%s", galleryId);
  if (bodyLen <= 0 || bodyLen >= (int)sizeof(body)) {
    ssl.stop();
    return;
  }

  ssl.print("POST /stream-end-set.php HTTP/1.1\r\n");
  ssl.print("Host: ");
  ssl.print(SERVER);
  ssl.print("\r\n");
  ssl.print("User-Agent: P.A.R./1.0\r\n");
  ssl.print("Accept: */*\r\n");
  ssl.print("Content-Type: application/x-www-form-urlencoded\r\n");
  ssl.print("Content-Length: ");
  ssl.print(bodyLen);
  ssl.print("\r\n");
  ssl.print("X-Snapshot-Secret: ");
  ssl.print(SNAPSHOT_SECRET);
  ssl.print("\r\n");
  ssl.print("Connection: close\r\n");
  ssl.print("\r\n");
  ssl.print(body);

  // Drain only the status line so we know it actually got there.
  unsigned long t0 = millis();
  while (ssl.connected() && !ssl.available()) {
    if (millis() - t0 > 10000) break;
    delay(10);
  }
  String statusLine = ssl.readStringUntil('\n');
  ssl.stop();
  int sp = statusLine.indexOf(' ');
  if (sp >= 0 && statusLine.charAt(sp + 1) == '2') {
    plog::logf("stream-end-set ok id=%s", galleryId);
  } else {
    plog::logf("stream-end-set unexpected: %.40s", statusLine.c_str());
  }
}

void loop() {
  ensureWiFi();
  plog::log("poll start");
  int status = 0;
  String galleryId = "";
  String body = "";
  bool ok = fetchNext(status, galleryId, body);
  if (!ok) {
    plog::log("poll fetchNext failed");
    delay(10000);
    return;
  }
  plog::logf("poll status=%d bodyLen=%u", status, (unsigned)body.length());

  if (status == 200 && body != "NONE" && body.length() > 0) {
    // 37 cols × 18 rows = 666 bits → 84 bytes (last byte has 6 padding bits).
    // 84 bytes encodes to exactly 112 base64 chars. decode_base64() does no
    // output-bounds-check, so reject anything longer before we hand it the
    // buffer, and decode into a scratch buffer that tolerates a small amount
    // of extra input as defense-in-depth.
    const size_t MAX_BODY_CHARS = 112;
    if (body.length() > MAX_BODY_CHARS) {
      plog::logf("body too long: %u", (unsigned)body.length());
      delay(10000);
      return;
    }
    uint8_t bitmap[128];
    int decoded = decode_base64((unsigned char*)body.c_str(), bitmap);

    if (decoded == 84) {
      plog::log("bitmap rx ok");

      // Validate gallery id is digits-only and fits the buffer before storing
      // it — it gets interpolated straight into the /complete.php URL, so a
      // stray \r\n or other junk would let the server inject extra HTTP.
      if (!isDigitsOnlyId(galleryId) || galleryId.length() >= sizeof(pendingGalleryId)) {
        plog::log("bad gallery id, ignoring");
        pendingGalleryId[0] = '\0';
      } else {
        strncpy(pendingGalleryId, galleryId.c_str(), sizeof(pendingGalleryId) - 1);
        pendingGalleryId[sizeof(pendingGalleryId) - 1] = '\0';
      }

      // Re-scan the board before each job IF the board state could have
      // drifted since we last scanned. The post-display idle drops motors
      // ($1=0) and sleeps 10 min — during that window the gantry can lose
      // position and discs can be touched, so gridStateFresh gets cleared.
      // The very first job after boot reuses setup()'s scan and skips this.
      if (!gridStateFresh) {
        plog::log("scanGrid begin");
        scanGrid();
      } else {
        plog::log("scanGrid skipped (state fresh)");
      }

      plog::log("displayBitmap begin");
      displayBitmap(bitmap);
      waitForIdle();

      // Check pass: re-scan the board (which reseeds gridState[] from the
      // physical discs, catching any that didn't flip cleanly), then run
      // displayBitmap again so its diff-against-gridState logic re-flips just
      // the cells that are still wrong. One pass — misclassifications or
      // mechanical misses get a second chance without a full redraw.
      plog::log("check pass: scanGrid begin");
      scanGrid();
      plog::log("check pass: displayBitmap begin");
      displayBitmap(bitmap);
      waitForIdle();

      plog::log("display done");
      // Trigger the snapshot now that the board reflects the final corrected
      // state (after the check pass), rather than relying on next.php to have
      // armed it at job start — that earlier armed window let the snapshot
      // poller grab a photo mid-draw.
      sendSnapshotRequest(pendingGalleryId[0] ? pendingGalleryId : galleryId.c_str());
      onDisplayComplete();
      // Motors are about to go off and the gantry will be idle for 10 min —
      // anything could happen to the board in that window, so the next job
      // must re-scan.
      gridStateFresh = false;
      // Release steppers for the long idle. $1=0 only takes effect on the
      // next idle transition, so kick a tiny jog (X-0.1, away from the X=0
      // soft limit) to trigger the disable. Reassert G21/G90 first — error:2
      // (unsupported word) often traces back to inches/mm or relative/absolute
      // mode being out of sync after a recovery path.
      sendGcode("G21");
      sendGcode("G90");
      sendGcode("$1=0");
      sendGcode("G91");
      sendGcode("G0 X-0.1");
      sendGcode("G90");
      waitForIdle();
      delay(10UL * 60UL * 1000UL);
      // After the post-display linger, tell the Mac Mini to stop recording
      // this print. Sent here (not in onDisplayComplete) so the recording
      // covers the full display + 10-min settle window.
      sendStreamEnd(pendingGalleryId[0] ? pendingGalleryId : galleryId.c_str());
    } else {
      plog::logf("bad decode length: %d", decoded);
    }
  } else {
    delay(10000);
  }
}

bool isDigitsOnlyId(const String& s) {
  if (s.length() == 0) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char ch = s.charAt(i);
    if (ch < '0' || ch > '9') return false;
  }
  return true;
}

bool fetchNext(int& outStatus, String& outGalleryId, String& outBody) {
  WiFiSSLClient ssl;
  if (!ssl.connect(SERVER, PORT)) {
    plog::log("next.php connect() failed");
    return false;
  }

  // POST (not GET) so Cloudflare / any intermediate proxy won't replay the
  // request on origin error — next.php pops a queue item per call, so a
  // silent retry drains items the Arduino never sees.
  ssl.print("POST /next.php HTTP/1.1\r\n");
  ssl.print("Host: ");
  ssl.print(SERVER);
  ssl.print("\r\n");
  ssl.print("User-Agent: P.A.R./1.0\r\n");
  ssl.print("Accept: */*\r\n");
  ssl.print("Content-Length: 0\r\n");
  ssl.print("Connection: close\r\n");
  ssl.print("\r\n");

  // Wait for first byte (with timeout)
  unsigned long t0 = millis();
  while (ssl.connected() && !ssl.available()) {
    if (millis() - t0 > 15000) {
      plog::log("next.php response timeout");
      ssl.stop();
      return false;
    }
    delay(10);
  }

  // Status line: "HTTP/1.1 200 OK"
  String statusLine = ssl.readStringUntil('\n');
  int sp1 = statusLine.indexOf(' ');
  int sp2 = statusLine.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) {
    ssl.stop();
    return false;
  }
  outStatus = statusLine.substring(sp1 + 1, sp2).toInt();

  // Headers — cap total header bytes at 4 KB so a misbehaving server (or a
  // malicious response) can't grow the String until we OOM. Cloudflare's real
  // response headers come in well under this.
  const size_t MAX_HEADER_BYTES = 4096;
  size_t headerBytes = 0;
  bool chunked = false;
  while (ssl.connected()) {
    String line = ssl.readStringUntil('\n');
    headerBytes += line.length() + 1;
    if (headerBytes > MAX_HEADER_BYTES) {
      plog::log("next.php headers too large");
      ssl.stop();
      return false;
    }
    line.trim();
    if (line.length() == 0) break;
    int colon = line.indexOf(':');
    if (colon < 0) continue;
    String name = line.substring(0, colon);
    String value = line.substring(colon + 1);
    name.trim();
    value.trim();
    if (name.equalsIgnoreCase("X-Gallery-Id")) outGalleryId = value;
    if (name.equalsIgnoreCase("Transfer-Encoding") && value.equalsIgnoreCase("chunked")) chunked = true;
  }

  // Body: read until connection closes (Connection: close), decoding chunked
  // if needed. Both paths bail after BODY_TIMEOUT_MS without any forward
  // progress — without this, a stuck TLS connection mid-body hangs forever.
  const unsigned long BODY_TIMEOUT_MS = 60000;
  outBody = "";
  if (chunked) {
    unsigned long lastProgress = millis();
    while (ssl.connected() || ssl.available()) {
      if (millis() - lastProgress > BODY_TIMEOUT_MS) {
        plog::log("next.php chunked body timeout");
        ssl.stop();
        return false;
      }
      String sizeLine = ssl.readStringUntil('\n');
      sizeLine.trim();
      if (sizeLine.length() == 0) continue;
      int chunkSize = (int)strtol(sizeLine.c_str(), nullptr, 16);
      if (chunkSize <= 0) break;
      lastProgress = millis();
      while (chunkSize > 0) {
        if (ssl.available()) {
          outBody += (char)ssl.read();
          chunkSize--;
          lastProgress = millis();
        } else if (millis() - lastProgress > BODY_TIMEOUT_MS) {
          plog::log("next.php chunk read timeout");
          ssl.stop();
          return false;
        }
      }
      // Trailing \r\n — read with a short timeout so a slow server can't
      // wedge us, and a missing CRLF doesn't desync the next chunk header.
      unsigned long tr = millis();
      int got = 0;
      while (got < 2 && millis() - tr < 1000) {
        if (ssl.available()) {
          ssl.read();
          got++;
        }
      }
    }
  } else {
    unsigned long lastProgress = millis();
    while (ssl.connected() || ssl.available()) {
      if (ssl.available()) {
        outBody += (char)ssl.read();
        lastProgress = millis();
      } else if (millis() - lastProgress > BODY_TIMEOUT_MS) {
        plog::log("next.php body timeout");
        ssl.stop();
        return false;
      }
    }
  }

  outBody.trim();
  ssl.stop();
  return true;
}
