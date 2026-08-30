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

const int GRID_W = 37;
const int GRID_H = 18;

const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 412.0f;
const float X_PITCH = 20.045f;

// Columns we will physically flip with the flip head (spread out, off-edge).
const int TEST_COLS[] = { 6, 18, 30 };
const int N_TEST_COLS = sizeof(TEST_COLS) / sizeof(TEST_COLS[0]);

// TCS3200 + LED illumination bank on D10 (via NPN, HIGH = on).
const int TCS_S0 = D4, TCS_S1 = D5, TCS_S2 = D6, TCS_S3 = D7, TCS_OUT = D8;
const int TCS_LED = D10;
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

// Servo (Servo-lib µs mapping).
const int SERVO_US_REST    = 565;
const int SERVO_US_RELEASE = 1018;
const int SERVO_US_ENGAGE  = 1471;
const int SERVO_90_DEG_SETTLE_MS = 300;
const int SERVO_50_DEG_SETTLE_MS = 100;
const float FLIP_OFFSET_X = 16.8f;

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
    tcsSelect(TCS_CLEAR); delay(2); sc += tcsReadFrequencyHz();
    tcsSelect(TCS_BLUE);  delay(2); sb += tcsReadFrequencyHz();
  }
  r = sr / 5; g = sg / 5; b = sb / 5; c = sc / 5;
}
const float SCAN_OFFSET_X = -23.0f;   // <-- the constant under test
const float SCAN_OFFSET_Y = 4.0f;
const float SCAN_Y_MAX = -0.05f;
static inline float clampScanY(float y) { return y > SCAN_Y_MAX ? SCAN_Y_MAX : y; }

// LED settle after toggling the illumination bank before reading.
const int LED_SETTLE_MS = 20;

// One ambient-subtracted RGBC read: LEDs-off average subtracted from LEDs-on
// average. Room light cancels. Negatives clamped to 0. LEDs left off.
void readAmbientSubtracted(long& r, long& g, long& b, long& c) {
  unsigned long ar, ag, ab, ac, lr, lg, lb, lc;
  digitalWrite(TCS_LED, LOW);  delay(LED_SETTLE_MS); tcsReadRGBC(ar, ag, ab, ac);
  digitalWrite(TCS_LED, HIGH); delay(LED_SETTLE_MS); tcsReadRGBC(lr, lg, lb, lc);
  digitalWrite(TCS_LED, LOW);
  r = (long)lr - (long)ar; if (r < 0) r = 0;
  g = (long)lg - (long)ag; if (g < 0) g = 0;
  b = (long)lb - (long)ab; if (b < 0) b = 0;
  c = (long)lc - (long)ac; if (c < 0) c = 0;
}

// Simple threshold on the ambient-subtracted BLUE channel. 1 = cyan/on
// (front), 0 = black/off. Sensor views the disc BACK, so front cyan = clear
// below the threshold.
// Classification threshold. Physically the BLUE channel, not clear — the S2/S3
// select lines are crossed on this rig; the enum labels name the PHYSICAL
// filter, so tcsReadRGBC's `b` output holds true BLUE and `c` holds true CLEAR.
// That is deliberate (blue separates the disc faces 10.21x vs clear's 2.22x).
// Full explanation and the measurements are in PARMain.ino at this constant.
// 3535 = geometric mean of the measured populations; was 6000, which was
// lopsided (3.69x / 1.28x) toward the failure side.
const long SCAN_ON_CLEAR_MAX = 900;
static inline uint8_t classifyDisc(long c) { return (c < SCAN_ON_CLEAR_MAX) ? 1 : 0; }

void rehome() {
  sendGcode("$H"); waitForIdle();
  sendGcode("$1=255"); waitForIdle();  // sync past the $1 EEPROM write (grbl drops RX during the commit)
  sendGcode("G21"); sendGcode("G90"); waitForIdle();
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
    long r, g, b, c;
    readAmbientSubtracted(r, g, b, c);
    out[x] = classifyDisc(c);
  }
}

void printRow(const char* label, const uint8_t* row) {
  Serial.print(label);
  for (int x = 0; x < GRID_W; x++) Serial.print(row[x] ? '#' : '.');
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  // Serial0 owns D0/D1 by default on the Nano ESP32; hand them to Serial1 so
  // the GRBL link keeps its identifier and its physical wires.
  Serial0.end();
  Serial1.begin(115200, SERIAL_8N1, D0, D1);

  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);  // TX-only servo link on D9
  delay(100);
  servoTxLine(SERVO_US_REST);

  pinMode(TCS_S0, OUTPUT); pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT); pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT, INPUT);
  pinMode(TCS_LED, OUTPUT); digitalWrite(TCS_LED, LOW);
  digitalWrite(TCS_S0, HIGH); digitalWrite(TCS_S1, LOW);
  tcsSelect(TCS_CLEAR);  // idle on the channel classifyDisc reads

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
