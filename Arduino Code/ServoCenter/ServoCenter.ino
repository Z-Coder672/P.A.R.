// ServoCenter — hold the servo at mechanical center (90 deg) and do NOTHING else.
//
// Drives the servo via the production hardware 9600-baud UART (Serial2) on D9 -> ServoNano
// (same path as PARMain). Deliberately does NOT touch Serial1 / GRBL, so the
// carriage CANNOT move — safe to run with the servo removed or mid-reassembly.
//
// Center = midpoint of the rig's configured pulse range (544us=0deg .. 2400us=180deg),
// so 90deg = 1472us. The value is re-sent every 500ms, so whenever the servo PSU is
// powered on the ServoNano picks it up and holds center.

const int SERVO_TX_PIN = D9;
const int SERVO_US_CENTER = 565;  // REST / 2 deg (parked)

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

void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);  // TX-only servo link on D9
  // NOTE: Serial1 is intentionally NOT started — GRBL gets no commands, carriage stays put.
}

void loop() {
  servoTxLine(SERVO_US_CENTER);
  Serial.print("hold -> ");
  Serial.print(SERVO_US_CENTER);
  Serial.println("us (REST / 0 deg)");
  delay(500);
}
