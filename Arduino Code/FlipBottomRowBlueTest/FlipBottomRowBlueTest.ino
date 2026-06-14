// FlipBottomRowBlueTest — scan only the bottom row (bitmap y = GRID_H-1) and
// flip every black squisk in it to blue, then stop. No WiFi, no queue.
//
// "Flip everything to blue" can't be a blind flip of every cell: flipping an
// already-blue disc would turn it black. So we scan each bottom-row cell with
// the TCS3200, classify blue/black with the production ternary model, and call
// flipDisc only on the cells currently reading black. Already-blue cells are
// left alone — after the pass the whole bottom row is blue.
//
// Drives the same GRBL streaming + D9→ServoNano bit-banged path + classifier as
// PARMain.ino. Motion constants, flipDisc, moveToYSafe, the scan offsets, and
// the TCS read/classify chain are copied verbatim from PARMain — keep in sync.

#include "classifier.h"  // tiny ternary transformer for blue/black RGBC

const int GRID_W = 37;
const int GRID_H = 18;

const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 402.0f;  // MUST equal GRBL $131

// TCS3200 color sensor: S0-S3 + OUT on D4..D8.
const int TCS_S0 = 4;
const int TCS_S1 = 5;
const int TCS_S2 = 6;
const int TCS_S3 = 7;
const int TCS_OUT = 8;

enum TcsFilter {
  TCS_RED = 0,    // S2=L, S3=L
  TCS_BLUE = 1,   // S2=L, S3=H
  TCS_CLEAR = 2,  // S2=H, S3=L
  TCS_GREEN = 3   // S2=H, S3=H
};

// Servo pulse widths (Servo-lib mapping: 544–2400µs over 0–180°).
const int SERVO_US_REST    = 544;
const int SERVO_US_RELEASE = 1018;  // ≈46°
const int SERVO_US_ENGAGE  = 1471;  // ≈90°
const int SERVO_90_DEG_SETTLE_MS = 300;
const int SERVO_50_DEG_SETTLE_MS = 100;
const int SERVO_US_10_DEG = 103;
const int SERVO_US_RELEASE2 = SERVO_US_RELEASE - SERVO_US_10_DEG;
const int SERVO_10_DEG_SETTLE_MS = 100;
const float FLIP_OFFSET_X = 16.8f;

// Optional second-catch pass — mirror PARMain.ino; off by default.
//#define FLIP_SECOND_CATCH

// Servo offloaded to a 5V Nano over a bit-banged 9600-baud software UART on D9.
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
  delayMicroseconds(SERVO_TX_BIT_US);  // stop bit
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
Coord grid[GRID_H][GRID_W];

void initGrid() {
  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      grid[y][x].x = -X_TRAVEL + 25.0f + 20.045f * x;
      grid[y][x].y = -Y_TRAVEL + 0.0f + 23.40f * ((GRID_H - 1) - y);
    }
  }
}

// ---- GRBL character-counting streaming ----
#define RX_BUFFER_SAFE 120
#define QUEUE_SIZE 32

int cmdLengths[QUEUE_SIZE];
int qHead = 0, qTail = 0, bufferFill = 0;

void enqueue(int len) { cmdLengths[qTail] = len; qTail = (qTail + 1) % QUEUE_SIZE; }
int dequeue() { int len = cmdLengths[qHead]; qHead = (qHead + 1) % QUEUE_SIZE; return len; }

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
    if (resp == "ok") {
      if (qHead != qTail) bufferFill -= dequeue();
    } else if (resp.startsWith("ALARM")) {
      haltSafe("GRBL ALARM");
    } else if (resp.startsWith("error")) {
      if (qHead != qTail) bufferFill -= dequeue();
    }
  }
}

void sendGcode(const char* cmd) {
  int cmdLen = strlen(cmd) + 1;
  while (bufferFill + cmdLen > RX_BUFFER_SAFE) drainResponses();
  Serial1.print(cmd);
  Serial1.write('\n');
  bufferFill += cmdLen;
  enqueue(cmdLen);
  Serial.print("Sent [buf:"); Serial.print(bufferFill); Serial.print("]: ");
  Serial.println(cmd);
}

void waitForIdle() { while (bufferFill > 0) drainResponses(); }

void moveTo(float x, float y) {
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "G0 X%.3f Y%.3f", x, y);
  sendGcode(cmd);
}

// Pure-X → pure-Y → pure-X so any vertical travel happens at an X soft-limit.
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

void waitForMotion() {
  sendGcode("G4 P0");
  waitForIdle();
}

// ---- TCS3200 read + classify (verbatim from PARMain) ----
void tcsSelect(TcsFilter f) {
  digitalWrite(TCS_S2, (f & 0x02) ? HIGH : LOW);
  digitalWrite(TCS_S3, (f & 0x01) ? HIGH : LOW);
}

unsigned long tcsReadFrequencyHz() {
  unsigned long halfUs = pulseIn(TCS_OUT, HIGH, 100000UL);
  if (halfUs == 0) return 0;
  return 500000UL / halfUs;
}

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

const float SCAN_OFFSET_X = -23.0f;
const float SCAN_OFFSET_Y = 4.0f;
const float SCAN_Y_MAX = -0.05f;
static inline float clampScanY(float y) { return y > SCAN_Y_MAX ? SCAN_Y_MAX : y; }

uint8_t classifyDisc(unsigned long r, unsigned long g,
                     unsigned long b, unsigned long c) {
  float rgbc[4] = { (float)r, (float)g, (float)b, (float)c };
  return classifier_is_blue(rgbc) ? 1 : 0;
}

void rehome() {
  sendGcode("$H");
  waitForIdle();
  sendGcode("$1=255");
  sendGcode("G21");
  sendGcode("G90");
  waitForIdle();
}

// ---- flipDisc (verbatim from PARMain) ----
void flipDisc(int gx, int gy, bool catchByNextMove) {
  moveTo(grid[gy][gx].x, grid[gy][gx].y);
  waitForMotion();

  writeServoUs(SERVO_US_ENGAGE, SERVO_90_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_REST, SERVO_90_DEG_SETTLE_MS);

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
  writeServoUs(SERVO_US_RELEASE2, SERVO_10_DEG_SETTLE_MS);
  if (!catchByNextMove) {
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
  (void)catchByNextMove;
  writeServoUs(SERVO_US_REST, SERVO_50_DEG_SETTLE_MS);
#endif
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);

  pinMode(SERVO_TX_PIN, OUTPUT);
  digitalWrite(SERVO_TX_PIN, HIGH);  // UART idle = high
  delay(100);
  servoTxLine(SERVO_US_REST);

  pinMode(TCS_S0, OUTPUT);
  pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT);
  pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT, INPUT);
  digitalWrite(TCS_S0, HIGH);   // 20% output scaling
  digitalWrite(TCS_S1, LOW);
  tcsSelect(TCS_CLEAR);

  initGrid();

  delay(2000);  // GRBL boot wait
  while (Serial1.available()) Serial1.read();

  Serial.println("Homing...");
  rehome();
  Serial.println("Homed.");

  const int yRow = GRID_H - 1;  // bottom row

  // --- Scan the bottom row left-to-right ---
  uint8_t rowState[GRID_W];
  Serial.println("Scanning bottom row...");
  moveToYSafe(grid[yRow][0].x + SCAN_OFFSET_X,
              clampScanY(grid[yRow][0].y + SCAN_OFFSET_Y));
  for (int x = 0; x < GRID_W; x++) {
    moveTo(grid[yRow][x].x + SCAN_OFFSET_X,
           clampScanY(grid[yRow][x].y + SCAN_OFFSET_Y));
    waitForMotion();

    unsigned long r, g, b, c;
    tcsReadRGBC(r, g, b, c);
    uint8_t color = classifyDisc(r, g, b, c);
    rowState[x] = color;

    Serial.print("scan x="); Serial.print(x);
    Serial.print(" RGBC="); Serial.print(r); Serial.print(',');
    Serial.print(g); Serial.print(','); Serial.print(b); Serial.print(',');
    Serial.print(c);
    Serial.print(" -> "); Serial.println(color ? "BLUE" : "BLACK");
  }

  // --- Flip every black cell to blue, left-to-right ---
  // LTR sweep: when another black cell follows, its flipDisc opening move
  // already travels +X by ≥ one cell pitch (20.045 > 16.8), so fold any
  // second catch into it (matches PARMain's displayBitmap).
  int flipCols[GRID_W];
  int nFlips = 0;
  for (int x = 0; x < GRID_W; x++) {
    if (rowState[x] == 0) flipCols[nFlips++] = x;
  }
  Serial.print("Black cells to flip: "); Serial.println(nFlips);

  for (int i = 0; i < nFlips; i++) {
    int x = flipCols[i];
    Serial.print("Flipping ("); Serial.print(x); Serial.print(',');
    Serial.print(yRow); Serial.println(") -> blue");
    bool catchByNextMove = (i + 1 < nFlips);
    flipDisc(x, yRow, catchByNextMove);
    waitForIdle();
  }

  // Park: drop X to the limit, release steppers.
  moveToYSafe(grid[yRow][0].x, grid[yRow][0].y);
  waitForIdle();
  sendGcode("$1=0");
  sendGcode("G91");
  sendGcode("G0 X-0.1");
  sendGcode("G90");
  waitForIdle();

  Serial.println("Done — bottom row is all blue.");
}

void loop() {}
