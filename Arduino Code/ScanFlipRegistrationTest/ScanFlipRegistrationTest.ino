// ScanFlipRegistrationTest — measure whether the scanner and the flip head
// address the SAME squisk, or are off by one (or more) cells.
//
// WHY: a uniform "flip everything blue" test cannot reveal a scan↔flip
// mis-registration — every cell reads black and every cell flips, so you get
// all-blue no matter which neighbor the sensor was actually over. PARMain runs
// against a MIXED board, where reading the wrong neighbor flips the wrong disc
// → random-looking garble. This test isolates the registration directly.
//
// METHOD (needs no known starting colors):
//   1. scan the bottom row            -> before[]
//   2. flip a few KNOWN columns with the flip head (a toggle always changes
//      the disc's color, whatever it was)
//   3. scan the bottom row again      -> after[]
//   4. the columns whose color CHANGED are where the discs physically flipped.
//      Compare to the columns we commanded. Same  -> registration is correct.
//      Shifted by k -> SCAN_OFFSET_X is off by k * X_PITCH (20.045 mm).
//
// Everything except the diff logic is copied verbatim from PARMain.ino.

#include "classifier.h"

const int GRID_W = 37;
const int GRID_H = 18;

const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 402.0f;
const float X_PITCH = 20.045f;

// Columns we will physically flip with the flip head (spread out, off-edge).
const int TEST_COLS[] = { 6, 18, 30 };
const int N_TEST_COLS = sizeof(TEST_COLS) / sizeof(TEST_COLS[0]);

// TCS3200.
const int TCS_S0 = 4, TCS_S1 = 5, TCS_S2 = 6, TCS_S3 = 7, TCS_OUT = 8;
enum TcsFilter { TCS_RED = 0, TCS_BLUE = 1, TCS_CLEAR = 2, TCS_GREEN = 3 };

// Servo (Servo-lib µs mapping).
const int SERVO_US_REST    = 544;
const int SERVO_US_RELEASE = 1018;
const int SERVO_US_ENGAGE  = 1471;
const int SERVO_90_DEG_SETTLE_MS = 300;
const int SERVO_50_DEG_SETTLE_MS = 100;
const float FLIP_OFFSET_X = 16.8f;

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

void initGrid() {
  for (int y = 0; y < GRID_H; y++)
    for (int x = 0; x < GRID_W; x++) {
      grid[y][x].x = -X_TRAVEL + 25.0f + X_PITCH * x;
      grid[y][x].y = -Y_TRAVEL + 0.0f + 23.40f * ((GRID_H - 1) - y);
    }
}

#define RX_BUFFER_SAFE 120
#define QUEUE_SIZE 32
int cmdLengths[QUEUE_SIZE];
int qHead = 0, qTail = 0, bufferFill = 0;
void enqueue(int l) { cmdLengths[qTail] = l; qTail = (qTail + 1) % QUEUE_SIZE; }
int dequeue() { int l = cmdLengths[qHead]; qHead = (qHead + 1) % QUEUE_SIZE; return l; }
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
    Serial.print("GRBL: "); Serial.println(resp);
    if (resp == "ok") { if (qHead != qTail) bufferFill -= dequeue(); }
    else if (resp.startsWith("ALARM")) haltSafe("GRBL ALARM");
    else if (resp.startsWith("error")) { if (qHead != qTail) bufferFill -= dequeue(); }
  }
}
void sendGcode(const char* cmd) {
  int cmdLen = strlen(cmd) + 1;
  while (bufferFill + cmdLen > RX_BUFFER_SAFE) drainResponses();
  Serial1.print(cmd); Serial1.write('\n');
  bufferFill += cmdLen; enqueue(cmdLen);
}
void waitForIdle() { while (bufferFill > 0) drainResponses(); }
void moveTo(float x, float y) {
  char cmd[40]; snprintf(cmd, sizeof(cmd), "G0 X%.3f Y%.3f", x, y); sendGcode(cmd);
}
void moveToYSafe(float targetX, float targetY) {
  char cmd[40];
  float xLimit = (targetX > -X_TRAVEL / 2.0f) ? 0.0f : -X_TRAVEL;
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", xLimit); sendGcode(cmd);
  snprintf(cmd, sizeof(cmd), "G0 Y%.3f", targetY); sendGcode(cmd);
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", targetX); sendGcode(cmd);
}
void waitForMotion() { sendGcode("G4 P0"); waitForIdle(); }

void tcsSelect(TcsFilter f) {
  digitalWrite(TCS_S2, (f & 0x02) ? HIGH : LOW);
  digitalWrite(TCS_S3, (f & 0x01) ? HIGH : LOW);
}
unsigned long tcsReadFrequencyHz() {
  unsigned long halfUs = pulseIn(TCS_OUT, HIGH, 100000UL);
  if (halfUs == 0) return 0;
  return 500000UL / halfUs;
}
void tcsReadRGBC(unsigned long& r, unsigned long& g, unsigned long& b, unsigned long& c) {
  uint32_t sr = 0, sg = 0, sb = 0, sc = 0;
  for (int i = 0; i < 5; i++) {
    tcsSelect(TCS_RED);   delay(2); sr += tcsReadFrequencyHz();
    tcsSelect(TCS_GREEN); delay(2); sg += tcsReadFrequencyHz();
    tcsSelect(TCS_BLUE);  delay(2); sb += tcsReadFrequencyHz();
    tcsSelect(TCS_CLEAR); delay(2); sc += tcsReadFrequencyHz();
  }
  r = sr / 5; g = sg / 5; b = sb / 5; c = sc / 5;
}
const float SCAN_OFFSET_X = -23.0f;   // <-- the constant under test
const float SCAN_OFFSET_Y = 4.0f;
const float SCAN_Y_MAX = -0.05f;
static inline float clampScanY(float y) { return y > SCAN_Y_MAX ? SCAN_Y_MAX : y; }
uint8_t classifyDisc(unsigned long r, unsigned long g, unsigned long b, unsigned long c) {
  float rgbc[4] = { (float)r, (float)g, (float)b, (float)c };
  return classifier_is_blue(rgbc) ? 1 : 0;
}

void rehome() {
  sendGcode("$H"); waitForIdle();
  sendGcode("$1=255"); sendGcode("G21"); sendGcode("G90"); waitForIdle();
}

// flipDisc — verbatim from PARMain (2-stage, no second catch).
void flipDisc(int gx, int gy) {
  moveTo(grid[gy][gx].x, grid[gy][gx].y);
  waitForMotion();
  writeServoUs(SERVO_US_ENGAGE, SERVO_90_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_REST, SERVO_90_DEG_SETTLE_MS);
  float dx = FLIP_OFFSET_X;
  if (grid[gy][gx].x + dx > 0.0f) dx = -grid[gy][gx].x;
  char cmd[32];
  sendGcode("G91"); snprintf(cmd, sizeof(cmd), "G0 X%.3f", dx); sendGcode(cmd);
  sendGcode("G90"); waitForMotion();
  writeServoUs(SERVO_US_RELEASE, SERVO_50_DEG_SETTLE_MS);
  sendGcode("G91"); snprintf(cmd, sizeof(cmd), "G0 X%.3f", -dx); sendGcode(cmd);
  sendGcode("G90"); waitForMotion();
  writeServoUs(SERVO_US_REST, SERVO_50_DEG_SETTLE_MS);
}

// Scan the bottom row left-to-right into out[GRID_W].
void scanBottomRow(uint8_t* out) {
  const int y = GRID_H - 1;
  moveToYSafe(grid[y][0].x + SCAN_OFFSET_X, clampScanY(grid[y][0].y + SCAN_OFFSET_Y));
  for (int x = 0; x < GRID_W; x++) {
    moveTo(grid[y][x].x + SCAN_OFFSET_X, clampScanY(grid[y][x].y + SCAN_OFFSET_Y));
    waitForMotion();
    unsigned long r, g, b, c;
    tcsReadRGBC(r, g, b, c);
    out[x] = classifyDisc(r, g, b, c);
  }
}

void printRow(const char* label, const uint8_t* row) {
  Serial.print(label);
  for (int x = 0; x < GRID_W; x++) Serial.print(row[x] ? '#' : '.');
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);

  pinMode(SERVO_TX_PIN, OUTPUT);
  digitalWrite(SERVO_TX_PIN, HIGH);
  delay(100);
  servoTxLine(SERVO_US_REST);

  pinMode(TCS_S0, OUTPUT); pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT); pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT, INPUT);
  digitalWrite(TCS_S0, HIGH); digitalWrite(TCS_S1, LOW);
  tcsSelect(TCS_CLEAR);

  initGrid();
  delay(2000);
  while (Serial1.available()) Serial1.read();

  Serial.println("Homing...");
  rehome();
  Serial.println("Homed.");

  const int y = GRID_H - 1;
  uint8_t before[GRID_W], after[GRID_W];

  Serial.println("\n--- Scan BEFORE ---");
  scanBottomRow(before);
  printRow("before: ", before);

  Serial.print("\n--- Flipping (toggling) logical columns:");
  for (int i = 0; i < N_TEST_COLS; i++) { Serial.print(' '); Serial.print(TEST_COLS[i]); }
  Serial.println(" ---");
  for (int i = 0; i < N_TEST_COLS; i++) {
    flipDisc(TEST_COLS[i], y);
    waitForIdle();
  }

  Serial.println("\n--- Scan AFTER ---");
  scanBottomRow(after);
  printRow("after:  ", after);

  // Mark which columns we commanded vs which the scanner saw change.
  char cmd[GRID_W + 1], chg[GRID_W + 1];
  for (int x = 0; x < GRID_W; x++) { cmd[x] = '.'; chg[x] = (before[x] != after[x]) ? '#' : '.'; }
  for (int i = 0; i < N_TEST_COLS; i++) cmd[TEST_COLS[i]] = '#';
  cmd[GRID_W] = chg[GRID_W] = '\0';
  Serial.print("\ncommanded flips: "); Serial.println(cmd);
  Serial.print("scanner saw at:  "); Serial.println(chg);

  // Estimate the offset: for each commanded column, find the nearest changed
  // column and report the signed delta. A consistent non-zero delta IS the
  // registration error (in cells); * 20.045 mm = the SCAN_OFFSET_X correction.
  Serial.println("\nper-column offset (scanner_col - commanded_col):");
  for (int i = 0; i < N_TEST_COLS; i++) {
    int cc = TEST_COLS[i], best = 999;
    for (int x = 0; x < GRID_W; x++) {
      if (before[x] != after[x]) {
        int d = x - cc;
        if (abs(d) < abs(best)) best = d;
      }
    }
    Serial.print("  commanded "); Serial.print(cc);
    Serial.print(" -> nearest change delta = ");
    if (best == 999) Serial.println("(none seen!)");
    else { Serial.print(best); Serial.print("  ("); Serial.print(best * X_PITCH, 3); Serial.println(" mm)"); }
  }
  Serial.println("\nIf every delta is 0  -> scan/flip registration is correct (look elsewhere).");
  Serial.println("If every delta is the SAME non-zero k -> SCAN_OFFSET_X is off by k cells;");
  Serial.println("  add (k * 20.045) mm to SCAN_OFFSET_X to align scan to flip.");

  // Park / release steppers.
  moveToYSafe(grid[y][0].x, grid[y][0].y);
  waitForIdle();
  sendGcode("$1=0"); sendGcode("G91"); sendGcode("G0 X-0.1"); sendGcode("G90");
  waitForIdle();
  Serial.println("\nDone.");
}

void loop() {}
