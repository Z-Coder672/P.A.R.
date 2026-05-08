// ServoToggleTest — send 't' over USB serial to toggle the flip servo
// between 0° (REST) and 30° (RELEASE). Pulse widths match P.A.R.Main.
//
// Also at startup: unlock GRBL ($X), set $1=0 (release steppers when idle),
// and run a tiny jog so the steppers actually disengage and the gantry can
// be pushed by hand.

#include <Servo.h>

const int SERVO_PIN        = 9;
const int SERVO_US_REST    = 544;   // 0°
const int SERVO_US_RELEASE = 853;   // ~30°
const int SETTLE_MS        = 200;

Servo flipServo;
bool atRelease = false;

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
  Serial1.begin(115200);
  while (!Serial && millis() < 3000) {}

  flipServo.attach(SERVO_PIN);
  flipServo.writeMicroseconds(SERVO_US_REST);

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
  flipServo.writeMicroseconds(us);
  delay(SETTLE_MS);

  Serial.print("Servo -> ");
  Serial.print(atRelease ? 30 : 0);
  Serial.print("° (");
  Serial.print(us);
  Serial.println(" us)");
}
