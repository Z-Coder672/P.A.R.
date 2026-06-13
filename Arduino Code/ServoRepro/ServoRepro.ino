// ServoRepro — reproduce the servo failure that shows up during a normal job.
//
// Uses the PRODUCTION servo path: RP2040 D9 bit-banged 9600 UART -> ServoNano
// (which drives the actual SG90 + witness servo on the shared signal line),
// exactly as PARMain.ino. Reuses FlipCornersTest's GRBL streaming. No WiFi, no
// sensor, no scan — homes once, then flips the TOP-LEFT corner (grid 0,0) over
// and over, printing a cycle counter + elapsed time so we can correlate a
// failure (seen on camera / in the ServoNano echo) with cycle # and uptime
// (thermal). Strip-down knobs are grouped at the top.

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

// ---- Strip-down knobs (change one at a time once it reproduces) ----
#define DO_X_MOVES   0   // 1 = real flip with X slide+catch; 0 = servo-only (no GRBL motion between servo writes)
#define DO_HOMING    0   // 1 = $H at boot (needed for DO_X_MOVES); 0 = $X unlock only

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

void writeServoUs(int us, int settle_ms) {
  servoTxLine(us);
  delay(settle_ms);
}

struct Coord { float x; float y; };
Coord grid[GRID_H][GRID_W];

void initGrid() {
  for (int y = 0; y < GRID_H; y++)
    for (int x = 0; x < GRID_W; x++) {
      grid[y][x].x = -X_TRAVEL + 25.0f + 20.045f * x;
      grid[y][x].y = -Y_TRAVEL + 0.0f + 23.40f * ((GRID_H - 1) - y);
    }
}

// ---- GRBL streaming (from FlipCornersTest) ----
#define RX_BUFFER_SAFE 120
#define QUEUE_SIZE 32
int cmdLengths[QUEUE_SIZE];
int qHead = 0, qTail = 0, bufferFill = 0;
void enqueue(int len) { cmdLengths[qTail] = len; qTail = (qTail + 1) % QUEUE_SIZE; }
int dequeue() { int len = cmdLengths[qHead]; qHead = (qHead + 1) % QUEUE_SIZE; return len; }

void drainResponses() {
  while (Serial1.available()) {
    String resp = Serial1.readStringUntil('\n');
    resp.trim();
    if (resp.length() == 0) continue;
    if (resp == "ok") {
      if (qHead != qTail) bufferFill -= dequeue();
    } else if (resp.startsWith("error") || resp.startsWith("ALARM")) {
      Serial.print("!!! GRBL halted: "); Serial.println(resp);
      while (true) ;
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

void waitForIdle() { while (bufferFill > 0) drainResponses(); }
void moveTo(float x, float y) { char cmd[40]; snprintf(cmd, sizeof(cmd), "G0 X%.3f Y%.3f", x, y); sendGcode(cmd); }
void waitForMotion() { sendGcode("G4 P0"); waitForIdle(); }

void flipCorner(int gx, int gy) {
#if DO_X_MOVES
  moveTo(grid[gy][gx].x, grid[gy][gx].y);
  waitForMotion();
#endif

  writeServoUs(SERVO_US_ENGAGE, SERVO_90_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_REST,   SERVO_90_DEG_SETTLE_MS);

#if DO_X_MOVES
  float dx = FLIP_OFFSET_X;
  if (grid[gy][gx].x + dx > 0.0f) dx = -grid[gy][gx].x;
  char cmd[32];
  sendGcode("G91"); snprintf(cmd, sizeof(cmd), "G0 X%.3f", dx);  sendGcode(cmd); sendGcode("G90"); waitForMotion();
#endif

  writeServoUs(SERVO_US_RELEASE, SERVO_50_DEG_SETTLE_MS);

#if DO_X_MOVES
  sendGcode("G91"); snprintf(cmd, sizeof(cmd), "G0 X%.3f", -dx); sendGcode(cmd); sendGcode("G90"); waitForMotion();
#endif

  writeServoUs(SERVO_US_REST, SERVO_50_DEG_SETTLE_MS);
}

unsigned long cycle = 0;
unsigned long startMs = 0;

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);
  pinMode(SERVO_TX_PIN, OUTPUT);
  digitalWrite(SERVO_TX_PIN, HIGH);  // UART idle high
  while (!Serial && millis() < 3000) ;

  servoTxLine(SERVO_US_REST);
  initGrid();

  delay(2000);  // GRBL boot
  while (Serial1.available()) Serial1.read();

#if DO_HOMING
  Serial.println("Homing...");
  sendGcode("$H"); waitForIdle();
#else
  sendGcode("$X"); waitForIdle();
#endif
  sendGcode("G21"); sendGcode("G90"); waitForIdle();
  writeServoUs(SERVO_US_REST, 1000);

  Serial.println("ServoRepro start: flipping top-left corner (0,0) forever");
  startMs = millis();
}

void loop() {
  flipCorner(0, 0);
  cycle++;
  unsigned long el = millis() - startMs;
  Serial.print("CYCLE "); Serial.print(cycle);
  Serial.print("  t="); Serial.print(el / 1000UL); Serial.print("s");
  Serial.println();
}
