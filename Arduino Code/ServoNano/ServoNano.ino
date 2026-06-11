#include <Servo.h>
#include <SoftwareSerial.h>

#define SERVO_PIN 9
#define RX_PIN 2     // wire RP2040 D9 (bit-banged TX) here
#define TX_PIN 3     // unused, SoftwareSerial requires a pin

Servo s;
SoftwareSerial link(RX_PIN, TX_PIN);

String inputLine;

void setup() {
  Serial.begin(9600);          // USB-Serial, for debug echo
  link.begin(9600);            // matches the RP2040's bit-banged TX
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  s.attach(SERVO_PIN, 544, 2400);
  s.writeMicroseconds(544);
  Serial.println("ServoNano ready");
}

void loop() {
  while (link.available()) {
    char c = (char)link.read();
    Serial.write(c);  // echo every byte that arrives so you can see corruption

    if (c == '\n' || c == '\r') {
      inputLine.trim();
      if (inputLine.length()) {
        int us = inputLine.toInt();
        if (us >= 544 && us <= 2400) {
          s.writeMicroseconds(us);
          digitalWrite(LED_BUILTIN, HIGH);
          delay(50);
          digitalWrite(LED_BUILTIN, LOW);
          Serial.print("[ok ");
          Serial.print(us);
          Serial.println("]");
        } else {
          Serial.print("[bad ");
          Serial.print(inputLine);
          Serial.println("]");
        }
      }
      inputLine = "";
    } else {
      inputLine += c;
    }
  }
}
