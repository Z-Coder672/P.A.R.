#include <Servo.h>

const int SERVO_PIN = 9;
const int SENSE_PIN = A0;

Servo servo;
String inputBuffer = "";

void setup() {
  Serial.begin(9600);
  servo.attach(SERVO_PIN);
  Serial.println("Ready. Send a degree value (0-180).");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      int deg = inputBuffer.toInt();
      if (deg >= 0 && deg <= 180) {
        servo.write(deg);
        Serial.print("Moving to: ");
        Serial.println(deg);
      } else {
        Serial.println("Out of range (0-180).");
      }
      inputBuffer = "";
    } else {
      inputBuffer += c;
    }
  }

  int raw = analogRead(SENSE_PIN);
  float sensed_voltage = raw * (3.3 / 1023.0);
  float actual_voltage = sensed_voltage * (10200.0 / 5100.0);
  float current = actual_voltage / 1.67;

  Serial.print("raw: ");
  Serial.print(raw);
  Serial.print("  actual_V: ");
  Serial.print(actual_voltage, 3);
  Serial.print("V  current: ");
  Serial.print(current, 3);
  Serial.println("A");

  delay(100);
}