// ServoToggleTest — send 't' over USB serial to toggle the flip servo
// between 0° (REST) and 30° (RELEASE). Pulse widths match P.A.R.Main.
//
// Also at startup: unlock GRBL ($X), set $1=0 (release steppers when idle),
// and run a tiny jog so the steppers actually disengage and the gantry can
// be pushed by hand.

// D9 is NOT a PWM servo line. It is a hardware 9600-baud UART TX (Serial2)
// feeding the dedicated 5V ServoNano, which parses one integer microsecond
// value per line and drives the SG90 itself. Never attach Servo.h to this pin:
// 50Hz servo PWM decodes as garbage UART frames and throws the arm to random
// angles (this snapped the flip arm once).
const int SERVO_TX_PIN     = D9;
const int SERVO_US_REST    = 544;   // 0°
const int SERVO_US_RELEASE = 935;   // ~38° (raised 8° from the prior 853/~30°)
const int SETTLE_MS        = 200;

bool atRelease = false;

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

// Send a g-code line and wait for grbl-Mega's `ok`/`error`/`ALARM` ack.
// Mirrors the conventions in P.A.R.Main: `\n` only (no `\r`), and we
// stream all GRBL output back to USB serial so misbehavior is visible.
void grblSend(const char* cmd) {
  Serial.print("> "); Serial.println(cmd);
  Serial1.print(cmd);
  Serial1.write('\n');

  unsigned long t0 = millis();
  String line;
  while (millis() - t0 < 5000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      if (c == '\r') continue;
      if (c == '\n') {
        line.trim();
        if (line.length()) { Serial.print("GRBL: "); Serial.println(line); }
        if (line == "ok" || line.startsWith("error") || line.startsWith("ALARM")) return;
        line = "";
      } else {
        line += c;
      }
    }
  }
  Serial.println("(grblSend timeout)");
}

void setup() {
  Serial.begin(115200);
  // Serial0 owns D0/D1 by default on the Nano ESP32; hand them to Serial1 so
  // the GRBL link keeps its identifier and its physical wires.
  Serial0.end();
  Serial1.begin(115200, SERIAL_8N1, D0, D1);
  while (!Serial && millis() < 3000) {}

  // Park the arm at REST BEFORE any GRBL motion — homing/jogging with a
  // randomly-positioned arm drags it across the populated board.
  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);  // TX-only servo link on D9
  delay(100);
  servoTxLine(SERVO_US_REST);

  // GRBL boot wait + drain welcome banner
  delay(2000);
  while (Serial1.available()) Serial1.read();

  grblSend("$X");          // clear alarm without homing
  grblSend("$1=0");        // step idle delay 0 -> release steppers when idle
  grblSend("G21");         // mm
  grblSend("G91");         // relative
  grblSend("G0 X-0.1 F500"); // tiny jog so the next idle transition releases
  grblSend("G90");         // back to absolute

  Serial.println("ServoToggleTest ready. Send 't' to toggle 0° <-> 30°.");
}

void loop() {
  if (Serial.available() <= 0) return;

  int c = Serial.read();
  if (c != 't' && c != 'T') return;

  atRelease = !atRelease;
  int us = atRelease ? SERVO_US_RELEASE : SERVO_US_REST;
  writeServoUs(us, SETTLE_MS);

  Serial.print("Servo -> ");
  Serial.print(atRelease ? 30 : 0);
  Serial.print("° (");
  Serial.print(us);
  Serial.println(" us)");
}
