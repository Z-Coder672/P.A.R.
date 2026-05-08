// USB Serial <-> Mega (Serial1) bridge for the Nano RP2040 Connect.
// Anything typed into the USB serial monitor is forwarded byte-for-byte
// to the Mega; anything the Mega sends back is printed raw.

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);
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
