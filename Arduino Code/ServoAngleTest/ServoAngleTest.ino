// ServoAngleTest — send an angle (0..90) over USB serial, terminated by
// newline, and the flip servo moves to that angle. Pulse-width mapping
// is linearly extrapolated from the two calibrated points in P.A.R.Main:
// 0° -> 544us, 30° -> 853us (so ~10.3 us/deg).
//
// Servo control is offloaded to a dedicated 5V Arduino Nano over a bit-banged
// TX line on Arduino D9 → 5V Nano D0 RX, shared GND, one-way. Going through
// mbed's UART class on an arbitrary PinName crashed the chip; software UART at
// 9600 baud sidesteps that and tolerates ISR jitter from GRBL Serial1 RX. The
// companion sketch (ServoNano.ino) listens on its hardware UART at 9600.

const int SERVO_TX_PIN = 9;
// 1/9600 ≈ 104.17 µs. mbed digitalWrite costs ~2 µs, so trim the delay to keep
// total bit width close to 104 µs and avoid cumulative drift across the frame.
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
  Serial1.begin(115200);
  pinMode(SERVO_TX_PIN, OUTPUT);
  digitalWrite(SERVO_TX_PIN, HIGH);  // UART idle = high
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
