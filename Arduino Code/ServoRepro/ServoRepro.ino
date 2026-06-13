// ServoRepro — faithful "normal job" servo+motion load to reproduce the MAIN
// servo failure, with the WITNESS servo (same signal line, off the ServoNano
// output) as the diagnostic reference.
//
// Uses PARMain's EXACT motion choreography: serpentine displayBitmap + moveToYSafe
// (Y travel only at X soft-limits, never a diagonal sweep across the board) +
// flipDisc with second-catch OFF (so the servo ALWAYS returns to REST between
// flips). The carriage therefore only ever moves with the servo at REST, except
// inside a flip — satisfying the hardware-safety rule. Servo is driven via the
// production bit-banged-UART -> ServoNano path. No WiFi, no sensor, no scan.
//
// Loop alternates a top-row+bottom-row pattern on/off, forcing 74 flips per pass
// with full-width X sweeps at top and bottom and a full-height Y transition
// between them. Run it and watch: does the main servo fail while the witness
// keeps moving (-> main's wire/power/sag), or do both fail (-> shared signal)?

const int GRID_W = 37;
const int GRID_H = 18;
const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 402.0f;

// ---- Production servo pulse widths / settles (match PARMain.ino) ----
const int SERVO_US_REST    = 544;
const int SERVO_US_RELEASE = 936;
const int SERVO_US_ENGAGE  = 1471;
const int SERVO_90_DEG_SETTLE_MS = 300;
const int SERVO_50_DEG_SETTLE_MS = 100;
const float FLIP_OFFSET_X = 16.8f;

// ---- Production bit-banged servo TX (match PARMain.ino exactly) ----
const int SERVO_TX_PIN = 9;
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
  delayMicroseconds(SERVO_TX_BIT_US);
}
void servoTxLine(int us) {
  char buf[12];
  int n = snprintf(buf, sizeof(buf), "%d\n", us);
  for (int i = 0; i < n; i++) servoTxByte((uint8_t)buf[i]);
}
void writeServoUs(int us, int settle_ms) { servoTxLine(us); delay(settle_ms); }

struct Coord { float x; float y; };
Coord grid[GRID_H][GRID_W];
uint8_t gridState[GRID_H][GRID_W];

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
void flipDisc(int gx, int gy) {
  moveTo(grid[gy][gx].x, grid[gy][gx].y);
  waitForMotion();

  writeServoUs(SERVO_US_ENGAGE, SERVO_90_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_REST,   SERVO_90_DEG_SETTLE_MS);

  float dx = FLIP_OFFSET_X;
  if (grid[gy][gx].x + dx > 0.0f) dx = -grid[gy][gx].x;
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
        flipDisc(x, y);
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

unsigned long pass = 0, startMs = 0;
bool patternOn = false;

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);
  pinMode(SERVO_TX_PIN, OUTPUT);
  digitalWrite(SERVO_TX_PIN, HIGH);
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
