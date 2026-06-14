// Flips every squisk in the LEFTMOST column (grid x=0), top row to bottom,
// then stops. Purpose: calibrate FLIP_SKEW_X_TOP — the linear left-lean applied
// to the flip head as a function of board height (see PARMain.ino flipSkewX).
//
// The leftmost column spans the full board height with X fixed, so it isolates
// the height-dependent shift: watch each row's flip land on-center and tweak
// FLIP_SKEW_X_TOP below until the top rows catch as reliably as the bottom.
// 0 disables the skew (square-board baseline); negative leans the top left.
//
// Uses the production servo path: RP2040 D9 bit-bangs a 9600-baud software UART
// of the µs value → ServoNano → SG90 (same as PARMain.ino). GRBL motion over
// Serial1. USB Serial is used for bench logging only.

const int GRID_W = 37;
const int GRID_H = 18;

// CNC homes to full negatives, so the work area lives in negative coordinates.
const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 402.0f;  // MUST equal GRBL $131

// --- Skew under test --------------------------------------------------------
// X shift (mm) applied to the flip head at the TOP row; 0 at the bottom row,
// linearly interpolated by physical height. Negative = toward homing (−X).
// Keep in sync with PARMain.ino's FLIP_SKEW_X_TOP once dialed in.
const float FLIP_SKEW_X_TOP = -2.0f;

static inline float flipSkewX(int gy) {
  float heightFrac = (float)((GRID_H - 1) - gy) / (float)(GRID_H - 1);
  return FLIP_SKEW_X_TOP * heightFrac;
}
// ---------------------------------------------------------------------------

// Pulse widths match the standard Servo lib mapping (544–2400µs over 0–180°).
const int SERVO_US_REST = 544;
const int SERVO_US_RELEASE = 1018;  // ≈46°
const int SERVO_US_ENGAGE = 1471;   // ≈90°
const int SERVO_90_DEG_SETTLE_MS = 300;
const int SERVO_50_DEG_SETTLE_MS = 100;
const float FLIP_OFFSET_X = 16.8f;

// Bit-banged software UART to the ServoNano (matches PARMain.ino).
const int SERVO_TX_PIN = 9;
const int SERVO_TX_BIT_US = 102;  // ~104µs/bit at 9600 baud, trimmed for write cost

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
Coord grid[GRID_H][GRID_W];

void initGrid() {
  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      grid[y][x].x = -X_TRAVEL + 25.0f + 20.045f * x;
      grid[y][x].y = -Y_TRAVEL + 0.0f + 23.40f * ((GRID_H - 1) - y);
    }
  }
}

#define RX_BUFFER_SAFE 120
#define QUEUE_SIZE 32

int cmdLengths[QUEUE_SIZE];
int qHead = 0;
int qTail = 0;
int bufferFill = 0;

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
      if (qHead != qTail) bufferFill -= dequeue();
    } else if (resp.startsWith("error") || resp.startsWith("ALARM")) {
      Serial.print("!!! GRBL halted: ");
      Serial.println(resp);
      while (true)
        ;
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
}

void waitForIdle() {
  while (bufferFill > 0) drainResponses();
}

void moveTo(float x, float y) {
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "G0 X%.3f Y%.3f", x, y);
  sendGcode(cmd);
}

// Pure-Y travel at an X soft-limit so the arm never drags across discs.
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

void flipDisc(int gx, int gy) {
  float fx = grid[gy][gx].x + flipSkewX(gy);
  moveTo(fx, grid[gy][gx].y);
  waitForMotion();

  writeServoUs(SERVO_US_ENGAGE, SERVO_90_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_REST, SERVO_90_DEG_SETTLE_MS);

  float dx = FLIP_OFFSET_X;
  if (fx + dx > 0.0f) dx = -fx;

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

  writeServoUs(SERVO_US_REST, SERVO_50_DEG_SETTLE_MS);
}

void setup() {
  Serial.begin(115200);

  // Bring the servo UART up first so the initial park is received.
  pinMode(SERVO_TX_PIN, OUTPUT);
  digitalWrite(SERVO_TX_PIN, HIGH);  // UART idle = high
  delay(100);
  servoTxLine(SERVO_US_REST);

  Serial1.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 5000)
    ;  // wait briefly for USB monitor, but don't hang headless

  initGrid();

  delay(2000);  // GRBL boot
  while (Serial1.available()) Serial1.read();

  Serial.println("Homing...");
  sendGcode("$H");
  waitForIdle();
  Serial.println("Homed.");

  sendGcode("G21");
  sendGcode("G90");
  waitForIdle();

  writeServoUs(SERVO_US_REST, 1000);

  Serial.print("FLIP_SKEW_X_TOP = ");
  Serial.print(FLIP_SKEW_X_TOP, 3);
  Serial.println(" mm");

  // Flip the leftmost column, top (y=0) to bottom. Pure-Y transitions between
  // rows go through the X soft-limit so the arm doesn't graze neighbors.
  const int gx = 0;
  moveToYSafe(grid[0][gx].x + flipSkewX(0), grid[0][gx].y);
  for (int y = 0; y < GRID_H; y++) {
    Serial.print("Flip (");
    Serial.print(gx);
    Serial.print(",");
    Serial.print(y);
    Serial.print(")  skewX=");
    Serial.print(flipSkewX(y), 3);
    Serial.println(" mm");
    flipDisc(gx, y);
    waitForIdle();
    if (y + 1 < GRID_H) {
      moveToYSafe(grid[y + 1][gx].x + flipSkewX(y + 1), grid[y + 1][gx].y);
    }
  }

  Serial.println("Done.");
}

void loop() {}
