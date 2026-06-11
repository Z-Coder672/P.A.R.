#include <Servo.h>

const int SERVO_PIN = 9;
const int TRIGGER_PIN = 10;  // wire this to a digital pin on the logger Uno

const int SERVO_US_REST    = 544;
const int SERVO_US_RELEASE = 1060;
const int SERVO_90_DEG_SETTLE_MS = 300;
const int SERVO_50_DEG_SETTLE_MS = 100;

Servo flipServo;

void writeServoUs(int us, int settle_ms) {
  flipServo.writeMicroseconds(us);
  delay(settle_ms);
}

void flipServoAttach() {
  flipServo.attach(SERVO_PIN, SERVO_US_REST, 2400);
  flipServo.writeMicroseconds(SERVO_US_REST);
  delay(SERVO_90_DEG_SETTLE_MS);
}

void flipServoDetach() {
  flipServo.detach();
  pinMode(SERVO_PIN, OUTPUT);
  digitalWrite(SERVO_PIN, LOW);
}

void setup() {
  Serial.begin(115200);

  pinMode(TRIGGER_PIN, OUTPUT);
  digitalWrite(TRIGGER_PIN, LOW);

  // park and detach like main code does in setup()
  flipServo.attach(SERVO_PIN, SERVO_US_REST, 2400);
  flipServo.writeMicroseconds(SERVO_US_REST);
  delay(500);
  flipServo.detach();
  pinMode(SERVO_PIN, OUTPUT);
  digitalWrite(SERVO_PIN, LOW);

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