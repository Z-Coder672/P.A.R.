#include <WiFiNINA.h>
#include <ArduinoHttpClient.h>
#include <base64.hpp>
#include <Servo.h>
#include "env.h"
#include "classifier.h"  // tiny ternary transformer for blue/black RGBC

const char* SSID     = "ab guest";
const char* PASSWORD = WIFI_PASSWORD;
const char* SERVER   = "par.zimmzimm.com";
const int   PORT     = 443;

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
const int TCS_S0  = 4;
const int TCS_S1  = 5;
const int TCS_S2  = 6;
const int TCS_S3  = 7;
const int TCS_OUT = 8;

// S2/S3 select the photodiode filter bank.
enum TcsFilter {
  TCS_RED   = 0,  // S2=L, S3=L
  TCS_BLUE  = 1,  // S2=L, S3=H
  TCS_CLEAR = 2,  // S2=H, S3=L
  TCS_GREEN = 3   // S2=H, S3=H
};

const int   SERVO_PIN     = 9;
// Pulse widths match the standard Servo lib mapping
// (MIN_PULSE_WIDTH=544, MAX_PULSE_WIDTH=2400 over 0–180°) so the angles the
// rig was tuned for stay the same: REST≈0° (parked), RELEASE≈30° (pushes the
// half-rotated squisk during the X slide-back), ENGAGE≈80° (initial 90° flip).
const int   SERVO_US_REST    = 544;
const int   SERVO_US_RELEASE = 853;
const int   SERVO_US_ENGAGE  = 1369;
const int   SERVO_80_DEG_SETTLE_MS = 300;
const int   SERVO_30_DEG_SETTLE_MS = 100;
const float FLIP_OFFSET_X = 16.8f;
const float FLIP_OFFSET_Y = 20.0f;

// Back to the standard Servo library — it's the same path Sweep uses, which
// the user has confirmed moves the servo on this rig. Bit-bang (digitalWrite)
// and mbed::PwmOut both produced no movement, so whatever the issue is, it's
// specific to those alternative drivers, not the Servo library or wiring.
Servo flipServo;

void writeServoUs(int us, int settle_ms) {
  flipServo.writeMicroseconds(us);
  delay(settle_ms);
}

struct Coord { float x; float y; };

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
      grid[y][x].x = -X_TRAVEL + 25.0f + 20.0f * x;
      grid[y][x].y = -Y_TRAVEL +  0.0f + 22.0f * ((GRID_H - 1) - y);
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

int cmdLengths[QUEUE_SIZE];
int qHead = 0;
int qTail = 0;
int bufferFill = 0;

// RP2040: NVIC_SystemReset() is exposed by the mbed core's CMSIS headers.
void grblStallReset(const char* where) {
  Serial.print("!!! GRBL stall in ");
  Serial.print(where);
  Serial.println(", forcing MCU reset");
  Serial.flush();
  delay(50);
  NVIC_SystemReset();
}

void enqueue(int len) {
  cmdLengths[qTail] = len;
  qTail = (qTail + 1) % QUEUE_SIZE;
}

int dequeue() {
  int len = cmdLengths[qHead];
  qHead = (qHead + 1) % QUEUE_SIZE;
  return len;
}

void drainResponses() {
  while (Serial1.available()) {
    String resp = Serial1.readStringUntil('\n');
    resp.trim();
    if (resp.length() == 0) continue;

    Serial.print("GRBL: ");
    Serial.println(resp);

    if (resp == "ok") {
      // grbl-Mega can emit a duplicate `ok` after $H (one when alarm clears,
      // one when homing completes). Without this guard the spurious ack
      // dequeues a stale slot, desyncing bufferFill so waitForIdle hangs.
      if (qHead != qTail) bufferFill -= dequeue();
    } else if (resp.startsWith("error") || resp.startsWith("ALARM")) {
      // ALARM:N is async (not tied to a queued command, so it never produces
      // the `ok` waitForIdle is waiting on) — without halting on it, a failed
      // homing cycle silently spins waitForIdle forever.
      Serial.print("!!! GRBL halted: "); Serial.println(resp);
      while (true);
    }
    // Anything else — status reports `<...>` from `?`, settings lines `$N=...`
    // from `$$`, welcome banner `Grbl ...`, `[MSG:...]`, `ALARM:...` — is not
    // tied to a queued command, so don't dequeue and don't halt.
  }
}

void sendGcode(const char* cmd) {
  int cmdLen = strlen(cmd) + 1; // +1 for the newline GRBL counts

  // Reset the stall timer every time the buffer actually drains so a
  // long-but-progressing motion sequence doesn't trip the watchdog.
  unsigned long stallT0 = millis();
  int lastFill = bufferFill;
  while (bufferFill + cmdLen > RX_BUFFER_SAFE) {
    drainResponses();
    if (bufferFill != lastFill) { lastFill = bufferFill; stallT0 = millis(); }
    if (millis() - stallT0 > GRBL_STALL_TIMEOUT_MS) grblStallReset("sendGcode");
  }

  // Send `\n` only, not `\r\n` — grbl-Mega treats `\r` as a line end then
  // acks the trailing `\n` as an empty line, producing a duplicate ok per
  // command. The duplicate desyncs cmdLengths queue accounting.
  Serial1.print(cmd);
  Serial1.write('\n');
  bufferFill += cmdLen;
  enqueue(cmdLen);

  Serial.print("Sent [buf:");
  Serial.print(bufferFill);
  Serial.print("]: ");
  Serial.println(cmd);
}

void waitForIdle() {
  unsigned long stallT0 = millis();
  int lastFill = bufferFill;
  while (bufferFill > 0) {
    drainResponses();
    if (bufferFill != lastFill) { lastFill = bufferFill; stallT0 = millis(); }
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
    tcsSelect(TCS_RED);   delay(2); sr += tcsReadFrequencyHz();
    tcsSelect(TCS_GREEN); delay(2); sg += tcsReadFrequencyHz();
    tcsSelect(TCS_BLUE);  delay(2); sb += tcsReadFrequencyHz();
    tcsSelect(TCS_CLEAR); delay(2); sc += tcsReadFrequencyHz();
  }
  r = sr / 5; g = sg / 5; b = sb / 5; c = sc / 5;
}

// Sensor head sits offset from the flip actuator on the gantry.
const float SCAN_OFFSET_X = -23.0f;
const float SCAN_OFFSET_Y =   4.0f;

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
  Serial.println("Scanning grid...");
  rehome();
  // Serpentine top-to-bottom: alternating row direction so the only Y travel
  // between rows happens at an X soft-limit (handled by moveToYSafe).
  // Iterate x over [0, GRID_W-1) — the rightmost column is intentionally
  // skipped on the scan pass; the existing SCAN_OFFSET_X is set up so the
  // leftmost column likewise falls outside the sensor sweep.
  bool ltr = true;
  moveToYSafe(grid[0][0].x + SCAN_OFFSET_X,
              grid[0][0].y + SCAN_OFFSET_Y);
  for (int y = 0; y < GRID_H; y++) {
    int startCol = ltr ? 0 : GRID_W - 2;
    int endCol   = ltr ? GRID_W - 2 : 0;
    int step     = ltr ? +1 : -1;
    for (int x = startCol; x != endCol + step; x += step) {
      moveTo(grid[y][x].x + SCAN_OFFSET_X,
             grid[y][x].y + SCAN_OFFSET_Y);
      waitForMotion();

      unsigned long r, g, b, c;
      tcsReadRGBC(r, g, b, c);
      uint8_t color = classifyDisc(r, g, b, c);
      gridState[y][x] = color;

      Serial.print("("); Serial.print(x); Serial.print(",");
      Serial.print(y); Serial.print(") R="); Serial.print(r);
      Serial.print(" G="); Serial.print(g);
      Serial.print(" B="); Serial.print(b);
      Serial.print(" C="); Serial.print(c);
      Serial.print(" -> "); Serial.println(color);
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
  Serial.println("Scan done.");
}

// `G4 P0` is a dwell that GRBL syncs through the planner before acking, so
// once its `ok` lands every queued motion has actually finished — not just
// been planned. Pair with waitForIdle() before any non-GRBL action.
void waitForMotion() {
  sendGcode("G4 P0");
  waitForIdle();
}

// Two-stage 180° flip:
//   1) Servo to ROTATE (~80°) above the disc — this rotates the squisk 90°.
//      Servo back to REST. The squisk is now half-flipped and stays put.
//   2) Arc the gantry around the squisk (Y down, X over, Y back up) so the
//      arm clears the disc body, drop the arm to RELEASE (~30°), then slide
//      X back to the disc column. The 30° arm catches the half-rotated
//      squisk and pushes it through the final 90° as the gantry slides.
// The Y excursion is what keeps the arm from dragging across the disc face
// during the X repositioning move between stages.
void flipDisc(int gx, int gy) {
  moveTo(grid[gy][gx].x, grid[gy][gx].y);
  waitForMotion();

  writeServoUs(SERVO_US_ENGAGE, SERVO_80_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_REST, SERVO_80_DEG_SETTLE_MS);

  // Bottom bitmap row sits at the lowest physical Y (-Y_TRAVEL, the home
  // limit); invert the Y excursion direction there so the arc lifts toward 0
  // rather than past the negative soft limit.
  float dy = (gy == GRID_H - 1) ? -FLIP_OFFSET_Y : FLIP_OFFSET_Y;
  // Cap the X excursion so the arc and the matching finish slide never command
  // a position past X=0 (the work area is entirely negative).
  float dx = FLIP_OFFSET_X;
  if (grid[gy][gx].x + dx > 0.0f) dx = -grid[gy][gx].x;

  char cmd[32];
  sendGcode("G91");
  snprintf(cmd, sizeof(cmd), "G0 Y%.3f", -dy);
  sendGcode(cmd);
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", dx);
  sendGcode(cmd);
  snprintf(cmd, sizeof(cmd), "G0 Y%.3f", dy);
  sendGcode(cmd);
  sendGcode("G90");
  waitForMotion();

  writeServoUs(SERVO_US_RELEASE, SERVO_30_DEG_SETTLE_MS);

  sendGcode("G91");
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", -dx);
  sendGcode(cmd);
  sendGcode("G90");
  waitForMotion();

  writeServoUs(SERVO_US_REST, SERVO_30_DEG_SETTLE_MS);
}

// End-of-job cleanup pass with the servo parked at REST. Serpentine sweep
// top-to-bottom so the only Y travel between rows happens at an X soft-limit
// via moveToYSafe — never a pure-Y or diagonal move at non-limit X.
void releaseSweep() {
  Serial.println("Release sweep...");
  bool ltr = true;
  moveToYSafe(grid[0][0].x, grid[0][0].y);
  for (int y = 0; y < GRID_H; y++) {
    int endCol = ltr ? GRID_W - 1 : 0;
    moveTo(grid[y][endCol].x, grid[y][endCol].y);
    waitForMotion();
    if (y + 1 < GRID_H) {
      moveToYSafe(grid[y + 1][endCol].x, grid[y + 1][endCol].y);
      ltr = !ltr;
    }
  }
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
  int lastY  = -1;  // top-most changing row (smallest bitmap-y)
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
    int endCol   = ltr ? GRID_W - 1 : 0;
    int step     = ltr ? +1 : -1;
    for (int x = startCol; x != endCol + step; x += step) {
      uint8_t bit = bitmapBit(bitmap, x, y);
      if (bit != gridState[y][x]) {
        flipDisc(x, y);
        gridState[y][x] = bit;
      }
    }
    moveTo(grid[y][endCol].x, grid[y][endCol].y);
    waitForMotion();
    if (y > lastY) {
      moveToYSafe(grid[y - 1][endCol].x, grid[y - 1][endCol].y);
      ltr = !ltr;
    }
  }
}

// Scan every cell, comparing the sensor reading to the desired bitmap. Records
// every mismatch into a packed bitset, then re-flips each one. Returns the
// count of mismatches found at scan time *before* the fixes. Callers wanting
// "is the board clean?" should loop until this returns 0 — i.e. a pass that
// observed zero wrong cells. A nonzero return means we attempted fixes this
// pass; whether they succeeded is checked by the next pass's scan.
int verifyAndFix(uint8_t* bitmap) {
  const int WRONG_BYTES = (GRID_H * GRID_W + 7) / 8;
  uint8_t wrong[WRONG_BYTES];
  memset(wrong, 0, sizeof(wrong));
  int nWrong = 0;

  Serial.println("Verifying...");
  rehome();
  // Serpentine scan bottom-to-top: rows alternate L→R / R→L so the inter-row
  // Y travel happens at an X soft-limit via moveToYSafe. Rightmost column is
  // intentionally skipped on the scan pass — see scanGrid for rationale.
  bool scanLtr = true;
  moveToYSafe(grid[GRID_H - 1][0].x + SCAN_OFFSET_X,
              grid[GRID_H - 1][0].y + SCAN_OFFSET_Y);
  for (int y = GRID_H - 1; y >= 0; y--) {
    int startCol = scanLtr ? 0 : GRID_W - 2;
    int endCol   = scanLtr ? GRID_W - 2 : 0;
    int step     = scanLtr ? +1 : -1;
    for (int x = startCol; x != endCol + step; x += step) {
      moveTo(grid[y][x].x + SCAN_OFFSET_X,
             grid[y][x].y + SCAN_OFFSET_Y);
      waitForMotion();

      unsigned long r, g, b, c;
      tcsReadRGBC(r, g, b, c);
      uint8_t actual = classifyDisc(r, g, b, c);
      uint8_t want   = bitmapBit(bitmap, x, y);
      gridState[y][x] = actual;

      if (actual != want) {
        int idx = y * GRID_W + x;
        wrong[idx / 8] |= (1 << (idx % 8));
        nWrong++;
      }
    }
    if (y > 0) {
      // Re-home midway (after the 9th scanned row, y=9 of the 17→0 sweep) so
      // accumulated step drift can't skew the rest of the scan.
      if (y == 9) rehome();
      moveToYSafe(grid[y - 1][endCol].x + SCAN_OFFSET_X,
                  grid[y - 1][endCol].y + SCAN_OFFSET_Y);
      scanLtr = !scanLtr;
    }
  }

  Serial.print("Mismatches: "); Serial.println(nWrong);
  if (nWrong == 0) return 0;

  // Serpentine fix pass over the band of rows that need refixing.
  int firstY = -1, lastY = -1;
  for (int y = GRID_H - 1; y >= 0; y--) {
    for (int x = 0; x < GRID_W; x++) {
      int idx = y * GRID_W + x;
      if ((wrong[idx / 8] >> (idx % 8)) & 1) {
        if (firstY < 0) firstY = y;
        lastY = y;
        break;
      }
    }
  }

  bool fixLtr = true;
  moveToYSafe(grid[firstY][0].x, grid[firstY][0].y);
  for (int y = firstY; y >= lastY; y--) {
    int startCol = fixLtr ? 0 : GRID_W - 1;
    int endCol   = fixLtr ? GRID_W - 1 : 0;
    int step     = fixLtr ? +1 : -1;
    for (int x = startCol; x != endCol + step; x += step) {
      int idx = y * GRID_W + x;
      if ((wrong[idx / 8] >> (idx % 8)) & 1) {
        Serial.print("Refix ("); Serial.print(x);
        Serial.print(","); Serial.print(y); Serial.println(")");
        flipDisc(x, y);
        gridState[y][x] = bitmapBit(bitmap, x, y);
      }
    }
    moveTo(grid[y][endCol].x, grid[y][endCol].y);
    waitForMotion();
    if (y > lastY) {
      moveToYSafe(grid[y - 1][endCol].x, grid[y - 1][endCol].y);
      fixLtr = !fixLtr;
    }
  }
  return nWrong;
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);
  // Don't gate on USB CDC (`while (!Serial);`) — the rig runs headless. The
  // 2-second delay below covers the GRBL/Serial1 boot wait, which is what we
  // actually depend on.

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

  // Force the servo to REST before any gantry motion. If the rig powered down
  // mid-flip the arm could still be at ROTATE/RELEASE, and scanGrid() would
  // drag the gantry across every disc with the arm extended.
  flipServo.attach(SERVO_PIN);
  flipServo.writeMicroseconds(SERVO_US_REST);

  delay(2000); // GRBL boot wait (Serial1) + servo settle
  while (Serial1.available()) Serial1.read();

  Serial.println("Homing...");
  sendGcode("$H");
  waitForIdle();
  Serial.println("Homed.");

  // $1=255 keeps steppers energized while idle so the gantry holds position
  // between motions; we drop back to $1=0 + a tiny jog at job end to release.
  sendGcode("$1=255");
  sendGcode("G21");
  sendGcode("G90");
  waitForIdle();

  scanGrid();

  Serial.print("Connecting to WiFi");
  WiFi.begin(SSID, PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" connected.");
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
  Serial.print("Marking complete: "); Serial.println(pendingGalleryId);

  const unsigned long TOTAL_BUDGET_MS = 5UL * 60UL * 1000UL;
  unsigned long backoff = 1000;
  unsigned long start = millis();

  while (millis() - start < TOTAL_BUDGET_MS) {
    bool attemptOk = false;
    WiFiSSLClient ssl;
    if (ssl.connect(SERVER, PORT)) {
      ssl.print("GET /complete.php?id="); ssl.print(pendingGalleryId); ssl.print(" HTTP/1.1\r\n");
      ssl.print("Host: "); ssl.print(SERVER); ssl.print("\r\n");
      ssl.print("User-Agent: P.A.R./1.0\r\n");
      ssl.print("Accept: */*\r\n");
      ssl.print("X-Snapshot-Secret: "); ssl.print(SNAPSHOT_SECRET); ssl.print("\r\n");
      ssl.print("Connection: close\r\n");
      ssl.print("\r\n");

      String statusLine = ssl.readStringUntil('\n');
      ssl.stop();
      Serial.print("Complete: "); Serial.println(statusLine);
      // statusLine looks like "HTTP/1.1 200 OK". Treat any 2xx as success.
      int sp = statusLine.indexOf(' ');
      if (sp >= 0 && statusLine.charAt(sp + 1) == '2') attemptOk = true;
    } else {
      Serial.println("complete connect() failed");
    }

    if (attemptOk) { pendingGalleryId[0] = '\0'; return; }

    Serial.print("Retrying complete in "); Serial.print(backoff); Serial.println(" ms");
    delay(backoff);
    backoff *= 2;
    if (backoff > 60000UL) backoff = 60000UL;
  }

  Serial.println("!! complete.php abandoned after 5 min — entry stays pending");
  pendingGalleryId[0] = '\0';
}

void loop() {
  // Poll for next bitmap
  Serial.println("Polling for next frame...");
  int status = 0;
  String galleryId = "";
  String body = "";
  bool ok = fetchNext(status, galleryId, body);
  Serial.print("Status: "); Serial.println(status);
  Serial.print("Body: ");   Serial.println(body);
  if (!ok) {
    Serial.println("fetchNext failed, waiting 10s...");
    delay(10000);
    return;
  }

  if (status == 200 && body != "NONE" && body.length() > 0) {
    // 37 cols × 18 rows = 666 bits → 84 bytes (last byte has 6 padding bits).
    // 84 bytes encodes to exactly 112 base64 chars. decode_base64() does no
    // output-bounds-check, so reject anything longer before we hand it the
    // buffer, and decode into a scratch buffer that tolerates a small amount
    // of extra input as defense-in-depth.
    const size_t MAX_BODY_CHARS = 112;
    if (body.length() > MAX_BODY_CHARS) {
      Serial.print("Body too long: "); Serial.println(body.length());
      delay(10000);
      return;
    }
    uint8_t bitmap[128];
    int decoded = decode_base64((unsigned char*)body.c_str(), bitmap);

    if (decoded == 84) {
      Serial.println("Got valid bitmap:");
      printBitmap(bitmap);

      // Validate gallery id is digits-only and fits the buffer before storing
      // it — it gets interpolated straight into the /complete.php URL, so a
      // stray \r\n or other junk would let the server inject extra HTTP.
      if (!isDigitsOnlyId(galleryId) || galleryId.length() >= sizeof(pendingGalleryId)) {
        Serial.print("!! Bad gallery id, ignoring: "); Serial.println(galleryId);
        pendingGalleryId[0] = '\0';
      } else {
        strncpy(pendingGalleryId, galleryId.c_str(), sizeof(pendingGalleryId) - 1);
        pendingGalleryId[sizeof(pendingGalleryId) - 1] = '\0';
      }

      // Re-home before each job. The 10-min idle runs with motors disabled
      // ($1=0), so position can drift; $H reseeds machine zero before motion.
      Serial.println("Homing for new job...");
      sendGcode("$H");
      waitForIdle();
      sendGcode("$1=255");
      sendGcode("G21");
      sendGcode("G90");
      waitForIdle();

      displayBitmap(bitmap);
      waitForIdle();

      // One mechanical-cleanup pass before scanning, so any half-rotated discs
      // settle before the sensor reads them.
      releaseSweep();

      // Verify what's actually on the board, fixing any wrong discs and
      // rescanning until clean. Cap passes so a flaky sensor or stuck disc
      // can't trap us forever — in that case we skip onDisplayComplete so
      // the gallery entry stays pending.
      const int MAX_VERIFY_PASSES = 10;
      int pass = 0;
      bool clean = false;
      while (pass < MAX_VERIFY_PASSES) {
        if (verifyAndFix(bitmap) == 0) { clean = true; break; }
        pass++;
      }

      if (clean) {
        onDisplayComplete();
        Serial.println("Display complete, waiting 10 min before next poll.");
        // Release steppers for the long idle. $1=0 only takes effect on the
        // next idle transition, so kick a tiny jog (X-0.1, away from the X=0
        // soft limit) to trigger the disable.
        sendGcode("$1=0");
        sendGcode("G91");
        sendGcode("G0 X-0.1");
        sendGcode("G90");
        waitForIdle();
        delay(10UL * 60UL * 1000UL);
      } else {
        Serial.println("!! Verify-fix exhausted, leaving entry pending");
        pendingGalleryId[0] = '\0';
      }
    } else {
      Serial.print("Bad decode length: "); Serial.println(decoded);
    }
  } else {
    Serial.println("No new frame, waiting 10s...");
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
    Serial.println("connect() failed");
    return false;
  }

  ssl.print("GET /next.php HTTP/1.1\r\n");
  ssl.print("Host: "); ssl.print(SERVER); ssl.print("\r\n");
  ssl.print("User-Agent: P.A.R./1.0\r\n");
  ssl.print("Accept: */*\r\n");
  ssl.print("Connection: close\r\n");
  ssl.print("\r\n");

  // Wait for first byte (with timeout)
  unsigned long t0 = millis();
  while (ssl.connected() && !ssl.available()) {
    if (millis() - t0 > 15000) {
      Serial.println("response timeout");
      ssl.stop();
      return false;
    }
    delay(10);
  }

  // Status line: "HTTP/1.1 200 OK"
  String statusLine = ssl.readStringUntil('\n');
  int sp1 = statusLine.indexOf(' ');
  int sp2 = statusLine.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) { ssl.stop(); return false; }
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
      Serial.println("headers too large");
      ssl.stop();
      return false;
    }
    line.trim();
    if (line.length() == 0) break;
    int colon = line.indexOf(':');
    if (colon < 0) continue;
    String name = line.substring(0, colon);
    String value = line.substring(colon + 1);
    name.trim(); value.trim();
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
        Serial.println("chunked body timeout");
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
          Serial.println("chunk read timeout");
          ssl.stop();
          return false;
        }
      }
      // Trailing \r\n — read with a short timeout so a slow server can't
      // wedge us, and a missing CRLF doesn't desync the next chunk header.
      unsigned long tr = millis();
      int got = 0;
      while (got < 2 && millis() - tr < 1000) {
        if (ssl.available()) { ssl.read(); got++; }
      }
    }
  } else {
    unsigned long lastProgress = millis();
    while (ssl.connected() || ssl.available()) {
      if (ssl.available()) {
        outBody += (char)ssl.read();
        lastProgress = millis();
      } else if (millis() - lastProgress > BODY_TIMEOUT_MS) {
        Serial.println("body timeout");
        ssl.stop();
        return false;
      }
    }
  }

  outBody.trim();
  ssl.stop();
  return true;
}

void printBitmap(uint8_t* bitmap) {
  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      int idx = y * GRID_W + x;
      int bit = (bitmap[idx / 8] >> (7 - (idx % 8))) & 1;
      Serial.print(bit ? "#" : ".");
    }
    Serial.println();
  }
}