// Flips a checkerboard pattern on the 37x18 grid (every other squisk), then
// stops. Uses the same GRBL streaming + flipDisc motion as P.A.R.Main —
// no WiFi, no sensor, no classifier. Useful for mechanical exercise without
// flipping every cell in the work area.

#include <Servo.h>

const int GRID_W = 37;
const int GRID_H = 18;

const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 399.695f;

const int SERVO_PIN = 9;
// Pulse widths match the standard Servo lib mapping
// (MIN_PULSE_WIDTH=544, MAX_PULSE_WIDTH=2400 over 0–180°) so the angles the
// rig was tuned for stay the same: REST≈0°, RELEASE≈38°, ENGAGE≈90°.
const int SERVO_US_REST    = 544;
const int SERVO_US_RELEASE = 936;
const int SERVO_US_ENGAGE  = 1471;
const int SERVO_90_DEG_SETTLE_MS  = 300;
const int SERVO_50_DEG_SETTLE_MS  = 100;
// Second-catch arm angle for the error-reduction pass: ~10° below RELEASE.
// 544–2400µs over 0–180° (~10.3µs/°), so 10° ≈ 103µs. Mirrors PARMain.ino.
const int SERVO_US_10_DEG = 103;
const int SERVO_US_RELEASE2 = SERVO_US_RELEASE - SERVO_US_10_DEG;
const int SERVO_10_DEG_SETTLE_MS = 100;
const float FLIP_OFFSET_X = 16.8f;

// Step-3 second-catch pass: after the main flip+catch, drop the arm a further
// ~10° (to RELEASE2, ~28°) and sweep +X once more to push back any disc the
// first catch left over/under-rotated. Comment this out to remove the
// second-catch back-move. Mirrors PARMain.ino — keep both in sync.
//#define FLIP_SECOND_CATCH

Servo flipServo;

void writeServoUs(int us, int servo_settle_ms) {
  Serial.print("writeServoUs("); Serial.print(us); Serial.println(")");
  flipServo.writeMicroseconds(us);
  delay(servo_settle_ms);
}

struct Coord {
  float x;
  float y;
};
Coord grid[GRID_H][GRID_W];

void initGrid() {
  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      // 25 mm starting X offset — matches P.A.R.Main.
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

  while (bufferFill + cmdLen > RX_BUFFER_SAFE) {
    drainResponses();
  }

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
  while (bufferFill > 0) drainResponses();
}

void moveTo(float x, float y) {
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "G0 X%.3f Y%.3f", x, y);
  sendGcode(cmd);
}

// Travel to (targetX, targetY) such that any vertical (Y) component happens
// with X pinned to the nearest absolute machine limit (X = 0 or X = -X_TRAVEL).
// Emits pure-X → pure-Y → pure-X. Use for entry into a phase or any cross-row
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

void waitForMotion() {
  sendGcode("G4 P0");
  waitForIdle();
}

// Mirrors PARMain.ino flipDisc, including the second error-reduction catch.
// `catchByNextMove`: when the caller's next motion already travels +X by
// ≥FLIP_OFFSET_X (an LTR row with another flip ahead), pass true to leave the
// arm down at RELEASE2 and let that move perform the catch; otherwise pass
// false for an explicit +X stroke + re-park at REST.
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
  // Second catch pass — drop the arm ~10° below RELEASE; emit our own +X
  // stroke only when the next move won't already provide it.
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
  // Second catch disabled — there's no extra pass to leave the arm down for, so
  // park it at REST regardless of catchByNextMove.
  (void)catchByNextMove;
  writeServoUs(SERVO_US_REST, SERVO_50_DEG_SETTLE_MS);
#endif
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

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);

  flipServo.attach(SERVO_PIN);

  initGrid();

  delay(2000);
  while (Serial1.available()) Serial1.read();

  Serial.println("Homing...");
  sendGcode("$H");
  waitForIdle();
  Serial.println("Homed.");
  unsigned long startTime = millis();

  // $1=255 keeps steppers energized between motions; released at end via $1=0
  // + a tiny jog so the disable actually takes effect on the next idle.
  sendGcode("$1=255");
  sendGcode("G21");
  sendGcode("G90");
  waitForIdle();

  writeServoUs(SERVO_US_REST, 1000);

  // Bottom-to-top (bitmap y = GRID_H-1 → 0). Within each row, flip every
  // other cell so the result is a checkerboard. The starting column parity
  // flips per row so adjacent rows interleave. Rows alternate sweep direction
  // (serpentine) so the only Y travel between rows happens at an X soft-limit
  // via moveToYSafe.
  bool ltr = true;
  for (int y = GRID_H - 1; y >= 0; y--) {
    int xFirst = (y + 1) % 2;
    int xLast = xFirst;
    while (xLast + 2 < GRID_W) xLast += 2;

    int xStart = ltr ? xFirst : xLast;
    int xEnd   = ltr ? xLast  : xFirst;
    int xStep  = ltr ? +2     : -2;

    for (int x = xStart; (xStep > 0) ? (x <= xEnd) : (x >= xEnd); x += xStep) {
      Serial.print("Flipping (");
      Serial.print(x);
      Serial.print(",");
      Serial.print(y);
      Serial.println(")");
      // On an LTR row, if another flipped cell follows (x + xStep still in
      // range), its flipDisc opening move travels +X by 2 cell pitches
      // (40.08 mm > 16.8 mm) — fold the second catch into it.
      bool catchByNextMove = ltr && (x + xStep <= xEnd);
      flipDisc(x, y, catchByNextMove);
      waitForIdle();
    }

    if (y > 0) {
      moveToYSafe(grid[y - 1][xEnd].x, grid[y - 1][xEnd].y);
      ltr = !ltr;
    }
  }

  unsigned long elapsed = millis() - startTime;
  Serial.print("Total time: ");
  Serial.print(elapsed / 1000UL);
  Serial.print(".");
  Serial.print((elapsed % 1000) / 100);
  Serial.println("s");

  releaseSweep();

  // Release steppers. $1=0 only takes effect on the next idle transition, so
  // the tiny X-0.1 jog (safe — work area is entirely negative X) gives GRBL
  // the motion→idle edge it needs to disable the drivers.
  sendGcode("$1=0");
  sendGcode("G91");
  sendGcode("G0 X-0.1");
  sendGcode("G90");
  waitForIdle();

  Serial.println("Done.");
}

void loop() {}
