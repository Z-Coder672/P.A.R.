// ServoRepro — faithful "normal job" servo+motion load to reproduce the MAIN
// servo failure, with the WITNESS servo (same signal line, off the ServoNano
// output) as the diagnostic reference.
//
// Uses PARMain's EXACT motion choreography: serpentine displayBitmap + moveToYSafe
// (Y travel only at X soft-limits, never a diagonal sweep across the board) +
// flipDisc with second-catch OFF (so the servo ALWAYS returns to REST between
// flips). The carriage therefore only ever moves with the servo at REST, except
// inside a flip — satisfying the hardware-safety rule. Servo is driven via the
// production hardware-UART -> ServoNano path. No WiFi, no sensor, no scan.
//
// Loop alternates a top-row+bottom-row pattern on/off, forcing 74 flips per pass
// with full-width X sweeps at top and bottom and a full-height Y transition
// between them. Run it and watch: does the main servo fail while the witness
// keeps moving (-> main's wire/power/sag), or do both fail (-> shared signal)?

const int GRID_W = 37;
const int GRID_H = 18;
const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 412.0f;

// ---- Production servo pulse widths / settles (match PARMain.ino) ----
const int SERVO_US_REST    = 544;
const int SERVO_US_RELEASE = 1018;  // ≈46° (raised 8° from the prior 936/≈38°)
const int SERVO_US_ENGAGE  = 1471;
const int SERVO_90_DEG_SETTLE_MS = 300;
const int SERVO_50_DEG_SETTLE_MS = 100;
const float FLIP_OFFSET_X = 16.8f;
// Inverted flip, applied on LEFT-TO-RIGHT rows: clearing slide -X, catch slide
// +X. Unwinds the column-rod twist the RTL rows wind in, and its return stroke
// ends the way the sweep is already heading so the head stops backtracking
// before the next cell. The flip target shifts right by this much because the
// arm meets the opposite face of the squisk. Mirrors PARMain.ino.
const float FLIP_INVERT_OFFSET_X = 11.0f;

// ---- Production servo TX over Serial2 (match PARMain.ino exactly) ----
const int SERVO_TX_PIN = D9;

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
void writeServoUs(int us, int settle_ms) { servoTxLine(us); delay(settle_ms); }

struct Coord { float x; float y; };
Coord grid[GRID_H][GRID_W];
uint8_t gridState[GRID_H][GRID_W];
unsigned long startMs = 0;

void initGrid() {
  for (int y = 0; y < GRID_H; y++)
    for (int x = 0; x < GRID_W; x++) {
      grid[y][x].x = -X_TRAVEL + 25.0f + 20.045f * x;
      grid[y][x].y = -Y_TRAVEL + 0.0f + 23.40f * ((GRID_H - 1) - y);
      gridState[y][x] = 0;
    }
}

// ---- GRBL streaming (robust + LOGGED, streamlined from PARMain) ----
#define RX_BUFFER_SAFE 120
#define QUEUE_SIZE 32
#define MAX_CMD_LEN 40
#define MAX_ERROR_RETRIES 5
#define ERROR_RETRY_DELAY_MS 500
#define GRBL_STALL_TIMEOUT_MS 30000UL
char cmdTexts[QUEUE_SIZE][MAX_CMD_LEN];
int cmdLengths[QUEUE_SIZE];
int qHead = 0, qTail = 0, bufferFill = 0;
int errorRetryCount = 0;
char lastErrorCmd[MAX_CMD_LEN] = "";

void enqueue(const char* cmd, int len) {
  strncpy(cmdTexts[qTail], cmd, MAX_CMD_LEN - 1); cmdTexts[qTail][MAX_CMD_LEN-1] = '\0';
  cmdLengths[qTail] = len; qTail = (qTail + 1) % QUEUE_SIZE;
}
int dequeue() { int len = cmdLengths[qHead]; qHead = (qHead + 1) % QUEUE_SIZE; return len; }

// Park servo at REST and freeze — used on an unrecoverable fault so the rig is
// left safe and the printed reason can be read from the USB log.
void haltSafe(const char* why) {
  servoTxLine(SERVO_US_REST);
  Serial.print("!!! HALT: "); Serial.println(why);
  while (true) { delay(1000); servoTxLine(SERVO_US_REST); }
}

void drainResponses() {
  while (Serial1.available()) {
    String resp = Serial1.readStringUntil('\n');
    resp.trim();
    if (resp.length() == 0) continue;
    Serial.print("GRBL: "); Serial.println(resp);   // log EVERY response to USB

    if (resp == "ok") {
      if (qHead != qTail) { bufferFill -= dequeue(); errorRetryCount = 0; lastErrorCmd[0] = '\0'; }
    } else if (resp.startsWith("ALARM")) {
      if (qHead != qTail) { Serial.print("!!! ALARM on cmd: '"); Serial.print(cmdTexts[qHead]); Serial.println("'"); }
      haltSafe("GRBL ALARM");
    } else if (resp.startsWith("error")) {
      if (qHead == qTail) { Serial.println("(error with empty queue, ignored)"); continue; }
      char failed[MAX_CMD_LEN]; strncpy(failed, cmdTexts[qHead], MAX_CMD_LEN); failed[MAX_CMD_LEN-1]='\0';
      bufferFill -= dequeue();
      if (strcmp(failed, lastErrorCmd) == 0) errorRetryCount++;
      else { strncpy(lastErrorCmd, failed, MAX_CMD_LEN); lastErrorCmd[MAX_CMD_LEN-1]='\0'; errorRetryCount = 1; }
      Serial.print("  -> "); Serial.print(resp); Serial.print(" on '"); Serial.print(failed);
      Serial.print("' retry "); Serial.print(errorRetryCount); Serial.print("/"); Serial.println(MAX_ERROR_RETRIES);
      if (errorRetryCount > MAX_ERROR_RETRIES) haltSafe("error retries exhausted");
      delay(ERROR_RETRY_DELAY_MS);
      int rlen = strlen(failed) + 1;
      Serial1.print(failed); Serial1.write('\n');
      bufferFill += rlen; enqueue(failed, rlen);
    }
    // else: status/banner/[MSG] — not queue-tied, ignore.
  }
}

void sendGcode(const char* cmd) {
  int cmdLen = strlen(cmd) + 1;
  unsigned long t0 = millis(); int lastFill = bufferFill;
  while (bufferFill + cmdLen > RX_BUFFER_SAFE) {
    drainResponses();
    if (bufferFill != lastFill) { lastFill = bufferFill; t0 = millis(); }
    if (millis() - t0 > GRBL_STALL_TIMEOUT_MS) haltSafe("stall in sendGcode (no ok 30s)");
  }
  Serial1.print(cmd); Serial1.write('\n');
  bufferFill += cmdLen; enqueue(cmd, cmdLen);
}
void waitForIdle() {
  unsigned long t0 = millis(); int lastFill = bufferFill;
  while (bufferFill > 0) {
    drainResponses();
    if (bufferFill != lastFill) { lastFill = bufferFill; t0 = millis(); }
    if (millis() - t0 > GRBL_STALL_TIMEOUT_MS) haltSafe("stall in waitForIdle (no ok 30s)");
  }
}
void moveTo(float x, float y) { char cmd[40]; snprintf(cmd, sizeof(cmd), "G0 X%.3f Y%.3f", x, y); sendGcode(cmd); }
void waitForMotion() { sendGcode("G4 P0"); waitForIdle(); }

// Pure-X -> pure-Y -> pure-X so the Y leg never drags the head across the disc
// area at a non-limit X (copied from PARMain).
void moveToYSafe(float targetX, float targetY) {
  char cmd[40];
  float xLimit = (targetX > -X_TRAVEL / 2.0f) ? 0.0f : -X_TRAVEL;
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", xLimit); sendGcode(cmd);
  snprintf(cmd, sizeof(cmd), "G0 Y%.3f", targetY); sendGcode(cmd);
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", targetX); sendGcode(cmd);
}

// flipDisc with second-catch OFF: arm always parks at REST at the end, so every
// inter-cell / inter-row move runs with the servo at REST (hardware-safe rule).
// `inverted` mirrors the X excursion (see FLIP_INVERT_OFFSET_X) — pass it on
// left-to-right sweep rows.
void flipDisc(int gx, int gy, bool inverted) {
  float fx = grid[gy][gx].x + (inverted ? FLIP_INVERT_OFFSET_X : 0.0f);
  moveTo(fx, grid[gy][gx].y);
  waitForMotion();

  writeServoUs(SERVO_US_ENGAGE, SERVO_90_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_REST,   SERVO_90_DEG_SETTLE_MS);

  // Capped against both soft limits — inverted (LTR) rows slide toward -X_TRAVEL.
  float dx = inverted ? -FLIP_OFFSET_X : FLIP_OFFSET_X;
  if (fx + dx > 0.0f) dx = -fx;
  if (fx + dx < -X_TRAVEL) dx = -X_TRAVEL - fx;
  char cmd[32];
  sendGcode("G91"); snprintf(cmd, sizeof(cmd), "G0 X%.3f", dx);  sendGcode(cmd); sendGcode("G90"); waitForMotion();

  writeServoUs(SERVO_US_RELEASE, SERVO_50_DEG_SETTLE_MS);

  sendGcode("G91"); snprintf(cmd, sizeof(cmd), "G0 X%.3f", -dx); sendGcode(cmd); sendGcode("G90"); waitForMotion();

  writeServoUs(SERVO_US_REST, SERVO_50_DEG_SETTLE_MS);
}

uint8_t bitmapBit(const uint8_t* bitmap, int x, int y) {
  int idx = y * GRID_W + x;
  return (bitmap[idx / 8] >> (7 - (idx % 8))) & 1;
}

// Serpentine display over the band of changing rows (copied from PARMain,
// second-catch path removed). Flips only cells whose bit differs from gridState.
void displayBitmap(uint8_t* bitmap) {
  int firstY = -1, lastY = -1;
  for (int y = GRID_H - 1; y >= 0; y--)
    for (int x = 0; x < GRID_W; x++)
      if (bitmapBit(bitmap, x, y) != gridState[y][x]) { if (firstY < 0) firstY = y; lastY = y; break; }
  if (firstY < 0) return;

  bool ltr = true;
  moveToYSafe(grid[firstY][0].x, grid[firstY][0].y);
  for (int y = firstY; y >= lastY; y--) {
    int startCol = ltr ? 0 : GRID_W - 1;
    int endCol   = ltr ? GRID_W - 1 : 0;
    int step     = ltr ? +1 : -1;
    for (int x = startCol; x != endCol + step; x += step) {
      if (bitmapBit(bitmap, x, y) != gridState[y][x]) {
        unsigned long el = (millis() - startMs) / 1000UL;
        Serial.print("FLIP x="); Serial.print(x); Serial.print(" y="); Serial.print(y);
        Serial.print(" t="); Serial.print(el); Serial.println("s");
        flipDisc(x, y, ltr);  // mirrored flip on LTR rows
        gridState[y][x] = bitmapBit(bitmap, x, y);
      }
    }
    moveTo(grid[y][endCol].x, grid[y][endCol].y);
    waitForMotion();
    if (y > lastY) { moveToYSafe(grid[y - 1][endCol].x, grid[y - 1][endCol].y); ltr = !ltr; }
  }
}

// Full-board checkerboard, inverted each pass -> every cell flips every pass
// (~333 flips/pass), traversing the entire board incl. the bottom-right corner.
// Closest stand-in for a real job's servo+motion volume.
uint8_t testBmp[84];
void buildPattern(bool phase) {
  for (int i = 0; i < 84; i++) testBmp[i] = 0;
  for (int y = 0; y < GRID_H; y++)
    for (int x = 0; x < GRID_W; x++) {
      if (((x + y) & 1) == (phase ? 1 : 0)) {
        int idx = y * GRID_W + x;
        testBmp[idx / 8] |= (uint8_t)(1 << (7 - (idx % 8)));
      }
    }
}

unsigned long pass = 0;
bool patternOn = false;

void setup() {
  Serial.begin(115200);
  // Serial0 owns D0/D1 by default on the Nano ESP32; hand them to Serial1 so
  // the GRBL link keeps its identifier and its physical wires.
  Serial0.end();
  Serial1.begin(115200, SERIAL_8N1, D0, D1);
  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);  // TX-only servo link on D9
  while (!Serial && millis() < 3000) ;

  servoTxLine(SERVO_US_REST);   // park servo before any motion
  initGrid();

  delay(2000);
  while (Serial1.available()) Serial1.read();

  Serial.println("Homing...");
  sendGcode("$H"); waitForIdle();
  sendGcode("G21"); sendGcode("G90"); waitForIdle();
  writeServoUs(SERVO_US_REST, 1000);

  Serial.println("ServoRepro: faithful displayBitmap loop (top+bottom rows toggle)");
  startMs = millis();
}

void loop() {
  patternOn = !patternOn;
  buildPattern(patternOn);
  displayBitmap(testBmp);
  pass++;
  unsigned long el = millis() - startMs;
  Serial.print("=== PASS "); Serial.print(pass);
  Serial.print(" (pattern "); Serial.print(patternOn ? "ON" : "OFF");
  Serial.print(")  t="); Serial.print(el / 1000UL); Serial.println("s ===");
}
