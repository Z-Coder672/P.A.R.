// ServoSagTest — measure servo "sag"/droop across a park -> RELEASE -> REST
// sequence, firing TRIGGER_PIN so an external logger Uno starts capturing at a
// known instant. No GRBL / Serial1, so the carriage never moves.
//
// HARDWARE NOTE (current rig): the main MCU does not hold the servo directly.
// D9 is a hardware 9600-baud UART TX (Serial2) feeding the dedicated 5V
// ServoNano, which parses one integer µs value per line and owns the actual
// PWM hold. So this sketch only *commands* a target µs — the hold (and hence
// any sag behavior of the PWM source itself) belongs to the ServoNano. There is
// also no attach/detach here: the ServoNano keeps driving the last commanded
// position, so the "detach" step is a logical park, not a signal teardown.
// Do NOT drive D9 LOW to "release" — that is a UART break to the ServoNano.

// D9 is a UART TX line to the ServoNano, NOT a PWM servo output. The companion
// sketch (ServoNano.ino) listens on its hardware UART at 9600 baud.
const int SERVO_TX_PIN = D9;

const int TRIGGER_PIN = D10;  // wire this to a digital pin on the logger Uno

const int SERVO_US_REST    = 565;
const int SERVO_US_RELEASE = 1142;  // ≈58° (raised 8° from the prior 1060/≈50°)
const int SERVO_90_DEG_SETTLE_MS = 300;
const int SERVO_50_DEG_SETTLE_MS = 100;

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

void writeServoUs(int us, int settle_ms) {
  servoTxLine(us);
  delay(settle_ms);
}

// Formerly flipServo.attach(...) + writeMicroseconds(REST). With the ServoNano
// path there is nothing to attach; just command REST and let it settle.
void flipServoAttach() {
  writeServoUs(SERVO_US_REST, SERVO_90_DEG_SETTLE_MS);
}

// Formerly flipServo.detach() + drive the pin LOW. The ServoNano owns the PWM
// and keeps holding, so this is only a logical park at REST; the TX line must
// stay idle HIGH (LOW would be a UART break).
void flipServoDetach() {
  servoTxLine(SERVO_US_REST);
}

void setup() {
  Serial.begin(115200);

  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);  // TX-only servo link on D9
  delay(100);
  servoTxLine(SERVO_US_REST);

  pinMode(TRIGGER_PIN, OUTPUT);
  digitalWrite(TRIGGER_PIN, LOW);

  // park like main code does in setup()
  servoTxLine(SERVO_US_REST);
  delay(500);
  flipServoDetach();

  Serial.println("parked and detached — send 's' to run test");
  { char c = 0; while (c != 's') { if (Serial.available()) c = Serial.read(); } }

  // fire trigger so logger starts capturing exactly here
  digitalWrite(TRIGGER_PIN, HIGH);
  delayMicroseconds(100);
  digitalWrite(TRIGGER_PIN, LOW);

  Serial.println("attaching...");
  flipServoAttach();

  Serial.println("moving to RELEASE (50 deg)...");
  writeServoUs(SERVO_US_RELEASE, SERVO_50_DEG_SETTLE_MS);

  Serial.println("returning to REST (0 deg)...");
  writeServoUs(SERVO_US_REST, SERVO_90_DEG_SETTLE_MS);

  Serial.println("detaching...");
  flipServoDetach();

  Serial.println("done");
}

void loop() {}
