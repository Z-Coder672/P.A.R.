// USB Serial <-> Mega (Serial1) bridge for the Arduino Nano ESP32.
// Anything typed into the USB serial monitor is forwarded byte-for-byte
// to the Mega; anything the Mega sends back is printed raw.

void setup() {
  Serial.begin(115200);
  // Serial0 owns D0/D1 by default on the Nano ESP32; hand them to Serial1 so
  // the GRBL link keeps its identifier and its physical wires.
  Serial0.end();
  Serial1.begin(115200, SERIAL_8N1, D0, D1);
  while (!Serial) {
    ;
  }
}

void loop() {
  while (Serial.available()) {
    Serial1.write(Serial.read());
  }
  while (Serial1.available()) {
    Serial.write(Serial1.read());
  }
}
