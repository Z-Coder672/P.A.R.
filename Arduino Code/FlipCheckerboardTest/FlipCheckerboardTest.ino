// Flips a checkerboard pattern on the 37x18 grid (every other squisk), then
// stops. Uses the same GRBL streaming + flipDisc motion as P.A.R.Main —
// no WiFi, no sensor, no classifier. Useful for mechanical exercise without
// flipping every cell in the work area.

const int GRID_W = 37;
const int GRID_H = 18;

const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 412.0f;  // MUST equal GRBL $131 — homing pins the -Y switch at -$131, so this anchors the grid

// D9 is NOT a PWM servo line. The SG90 is driven by a dedicated 5V Arduino Nano
// (ServoNano.ino); D9 carries a hardware 9600-baud UART TX (Serial2) of the µs value
// into that Nano's RX, which parses an integer per line and drives the servo.
// Driving D9 with Servo.h blasts 50Hz PWM into that RX and throws the arm to
// random angles. Serial2's TX is matrix-routed to D9, so this is a real UART.
const int SERVO_TX_PIN = D9;

// Pulse widths match the standard Servo lib mapping
// (MIN_PULSE_WIDTH=544, MAX_PULSE_WIDTH=2400 over 0–180°) so the angles the
// rig was tuned for stay the same: REST≈2°, RELEASE≈46°, ENGAGE≈90°.
const int SERVO_US_REST    = 565;
const int SERVO_US_RELEASE = 1018;  // ≈46° (raised 8° from the prior 936/≈38°)
const int SERVO_US_ENGAGE  = 1471;
const int SERVO_90_DEG_SETTLE_MS  = 300;
const int SERVO_50_DEG_SETTLE_MS  = 100;
// Lowered arm angle ~10° below RELEASE (scan sweep + second-catch pass in
// PARMain; second-catch only here). 544–2400µs over 0–180° (~10.3µs/°), so
// 10° ≈ 103µs. Mirrors PARMain.ino.
const int SERVO_US_10_DEG = 103;
const int SERVO_US_RELEASE2 = SERVO_US_RELEASE - SERVO_US_10_DEG;
const int SERVO_10_DEG_SETTLE_MS = 100;
const float FLIP_OFFSET_X = 16.8f;
// Inverted flip, applied on LEFT-TO-RIGHT sweep rows: the clearing slide runs
// -X and the catch/return slide runs +X, the opposite of the original flip.
// Two effects: the catch drives the squisk through its final 90 deg in the
// opposite rotational sense, so LTR rows unwind the column-rod twist the RTL
// rows wind in; and the return stroke now ends in the direction the sweep is
// already heading, so an LTR row stops backtracking FLIP_OFFSET_X before every
// next cell. (RTL rows keep the original flip, whose -X return already points
// along their sweep.) Approaching from the other side puts the arm on the
// opposite face of the squisk, so the flip X target shifts right by this much
// on inverted rows to keep the contact geometry identical. Mirrors PARMain.ino.
const float FLIP_INVERT_OFFSET_X = 11.0f;

// Step-3 second-catch pass: after the main flip+catch, drop the arm a further
// ~10° (to RELEASE2, ~36°) and sweep +X once more to push back any disc the
// first catch left over/under-rotated. Comment this out to remove the
// second-catch back-move. Mirrors PARMain.ino — keep both in sync.
//#define FLIP_SECOND_CATCH

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

void writeServoUs(int us, int servo_settle_ms) {
  Serial.print("writeServoUs("); Serial.print(us); Serial.println(")");
  servoTxLine(us);
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
// `inverted`: mirror the whole X excursion (dx = -FLIP_OFFSET_X, flip target
// shifted right by FLIP_INVERT_OFFSET_X) — pass it on LEFT-TO-RIGHT rows, so
// the return stroke ends the way the sweep is already heading.
// `catchByNextMove`: the second catch always runs OPPOSITE the return stroke,
// which under the mirror is against the sweep on both row directions — so it
// can never be folded into the caller's next move any more. Callers pass false;
// flipDisc emits its own +dx stroke and re-parks at REST.
void flipDisc(int gx, int gy, bool catchByNextMove, bool inverted) {
  // Mirrored-flip shift on inverted rows; the excursions below are relative
  // (G91), so only this absolute X target moves.
  float fx = grid[gy][gx].x + (inverted ? FLIP_INVERT_OFFSET_X : 0.0f);
  moveTo(fx, grid[gy][gx].y);
  waitForMotion();

  writeServoUs(SERVO_US_ENGAGE, SERVO_90_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_REST, SERVO_90_DEG_SETTLE_MS);

  // Capped against BOTH soft limits — inverted (LTR) rows slide toward
  // -X_TRAVEL, plain (RTL) rows toward 0.
  float dx = inverted ? -FLIP_OFFSET_X : FLIP_OFFSET_X;
  if (fx + dx > 0.0f) dx = -fx;
  if (fx + dx < -X_TRAVEL) dx = -X_TRAVEL - fx;

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
    float dx2 = inverted ? -FLIP_OFFSET_X : FLIP_OFFSET_X;
    if (fx + dx2 > 0.0f) dx2 = -fx;
    if (fx + dx2 < -X_TRAVEL) dx2 = -X_TRAVEL - fx;
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
  // Serial0 owns D0/D1 by default on the Nano ESP32; hand them to Serial1 so
  // the GRBL link keeps its identifier and its physical wires.
  Serial0.end();
  Serial1.begin(115200, SERIAL_8N1, D0, D1);

  // Park the arm at REST before any gantry motion — homing with a randomly
  // positioned arm drags it across the populated board.
  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);  // TX-only servo link on D9
  delay(100);
  servoTxLine(SERVO_US_REST);

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
  waitForIdle();  // sync past the $1 EEPROM write before pipelining more — grbl
                  // disables interrupts during the commit and drops Serial1 RX
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
      // The mirrored (LTR) flip ends its return stroke in the sweep direction,
      // so the second catch — which always runs OPPOSITE that return — now runs
      // AGAINST the sweep on both row directions. It can never be folded into
      // the caller's next move any more, so this is always false. That's the
      // trade the mirror buys: the fold-in saved a stroke only on the rows that
      // were paying a backtrack stroke to begin with.
      const bool catchByNextMove = false;
      flipDisc(x, y, catchByNextMove, ltr);  // mirrored flip on LTR rows
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
