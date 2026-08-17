// ServoAngleTest — send an angle (0..90) over USB serial, terminated by
// newline, and the flip servo moves to that angle. Pulse-width mapping
// is linearly extrapolated from the two calibrated points in P.A.R.Main:
// 0° -> 544us, 30° -> 853us (so ~10.3 us/deg).
//
// Servo control is offloaded to a dedicated 5V Arduino Nano over a one-way TX
// line on Arduino D9 → 5V Nano D2 RX (SoftwareSerial; D0 is its USB debug echo), shared GND. On the Nano ESP32 the GPIO
// matrix routes hardware UART2 (Serial2) TX to D9, so no software UART is
// needed. The companion sketch (ServoNano.ino) listens on its hardware UART
// at 9600.

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
//
// Also at startup: unlock GRBL ($X), set $1=0 (release steppers when idle),
// and run a tiny jog so the steppers actually disengage and the gantry can
// be pushed by hand. Mirrors ServoToggleTest.

const int SERVO_US_0DEG = 544;
const int SERVO_US_30DEG = 853;
const int SETTLE_MS = 200;
const int ANGLE_MIN = 0;
const int ANGLE_MAX = 90;

String inputLine;

// Pulse width for a given angle. The MOSFET level-shifter inversion that
// PARMain.ino used to compensate for is gone now that the servo is driven by
// the dedicated 5V Nano, so the mapping is direct.
int angleToMicros(int deg) {
  long us = (long)SERVO_US_0DEG + (long)deg * (SERVO_US_30DEG - SERVO_US_0DEG) / 30L;
  return (int)us;
}

// Send a g-code line and wait for grbl-Mega's `ok`/`error`/`ALARM` ack.
void grblSend(const char* cmd) {
  Serial.print("> ");
  Serial.println(cmd);
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
        if (line.length()) {
          Serial.print("GRBL: ");
          Serial.println(line);
        }
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
  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);  // TX-only servo link on D9
  while (!Serial && millis() < 3000) {}

  delay(100);
  servoTxLine(angleToMicros(0));

  // GRBL boot wait + drain welcome banner
  delay(2000);
  while (Serial1.available()) Serial1.read();

  grblSend("$X");
  grblSend("$1=0");
  grblSend("G21");
  grblSend("G91");
  grblSend("G0 X-0.1 F500");
  grblSend("G90");

  Serial.println("ServoAngleTest ready. Send an angle 0..90 followed by newline.");
}

void handleAngle(int deg) {
  if (deg < ANGLE_MIN || deg > ANGLE_MAX) {
    Serial.print("Out of range (0..90): ");
    Serial.println(deg);
    return;
  }
  int us = angleToMicros(deg);
  servoTxLine(us);
  delay(SETTLE_MS);
  Serial.print("Servo -> ");
  Serial.print(deg);
  Serial.print("° (");
  Serial.print(us);
  Serial.println(" us)");
}

void loop() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      inputLine.trim();
      if (inputLine.length()) {
        handleAngle(inputLine.toInt());
      }
      inputLine = "";
    } else {
      inputLine += c;
    }
  }
}
