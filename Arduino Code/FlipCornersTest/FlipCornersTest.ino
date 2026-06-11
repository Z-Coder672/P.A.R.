// Flips the four corner squisks one at a time, then stops. Uses the same
// GRBL streaming + flipDisc motion as P.A.R.Main — no WiFi, no sensor,
// no classifier. Useful for mechanical/servo calibration on a fresh board.

#include <Servo.h>

const int GRID_W = 37;
const int GRID_H = 18;

const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 399.695f;

const int SERVO_PIN = 9;
// Pulse widths match the standard Servo lib mapping
// (MIN_PULSE_WIDTH=544, MAX_PULSE_WIDTH=2400 over 0–180°) so the angles the
// rig was tuned for stay the same: REST≈0°, RELEASE≈50°, ENGAGE≈90°.
const int SERVO_US_REST    = 544;
const int SERVO_US_RELEASE = 1060;
const int SERVO_US_ENGAGE  = 1471;
const int SERVO_90_DEG_SETTLE_MS  = 300;
const int SERVO_50_DEG_SETTLE_MS  = 100;
const float FLIP_OFFSET_X = 16.8f;

// Back to the standard Servo library — it's the same path Sweep uses, which
// the user has confirmed moves the servo on this rig. Bit-bang (digitalWrite)
// and mbed::PwmOut both produced no movement, so whatever the issue is, it's
// specific to those alternative drivers, not the Servo library or wiring.
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
      // 25 mm starting X offset (slight tweak vs. P.A.R.Main's 25.0f).
      grid[y][x].x = -X_TRAVEL + 25.f + 20.045f * x;
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

void waitForMotion() {
  sendGcode("G4 P0");
  waitForIdle();
}

void flipDisc(int gx, int gy) {
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

  writeServoUs(SERVO_US_REST, SERVO_50_DEG_SETTLE_MS);
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);
  while (!Serial)
    ;

  flipServo.attach(SERVO_PIN);

  initGrid();

  delay(2000);
  while (Serial1.available()) Serial1.read();

  Serial.println("Homing...");
  sendGcode("$H");
  waitForIdle();
  Serial.println("Homed.");
  unsigned long startTime = millis();

  sendGcode("G21");
  sendGcode("G90");
  waitForIdle();

  writeServoUs(SERVO_US_REST, 1000);

  const int corners[4][2] = {
    { GRID_W - 1, GRID_H - 1 },
    { 0, 0 },
    { GRID_W - 1, 0 },
    { 0, GRID_H - 1 },
  };

  for (int i = 0; i < 4; i++) {
    int x = corners[i][0];
    int y = corners[i][1];
    Serial.print("Flipping corner (");
    Serial.print(x);
    Serial.print(",");
    Serial.print(y);
    Serial.println(")");
    flipDisc(x, y);
    waitForIdle();
  }

  unsigned long elapsed = millis() - startTime;
  Serial.print("Total time: ");
  Serial.print(elapsed / 1000UL);
  Serial.print(".");
  Serial.print((elapsed % 1000) / 100);
  Serial.println("s");
  Serial.println("Done.");
}

void loop() {}
