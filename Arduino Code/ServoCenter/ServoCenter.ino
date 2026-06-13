// ServoCenter — hold the servo at mechanical center (90 deg) and do NOTHING else.
//
// Drives the servo via the production bit-banged 9600-baud UART on D9 -> ServoNano
// (same path as PARMain). Deliberately does NOT touch Serial1 / GRBL, so the
// carriage CANNOT move — safe to run with the servo removed or mid-reassembly.
//
// Center = midpoint of the rig's configured pulse range (544us=0deg .. 2400us=180deg),
// so 90deg = 1472us. The value is re-sent every 500ms, so whenever the servo PSU is
// powered on the ServoNano picks it up and holds center.

const int SERVO_TX_PIN = 9;
const int SERVO_TX_BIT_US = 102;
const int SERVO_US_CENTER = 1472;  // 90 deg

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

void setup() {
  Serial.begin(115200);
  pinMode(SERVO_TX_PIN, OUTPUT);
  digitalWrite(SERVO_TX_PIN, HIGH);  // UART idle high
  // NOTE: Serial1 is intentionally NOT started — GRBL gets no commands, carriage stays put.
}

void loop() {
  servoTxLine(SERVO_US_CENTER);
  Serial.print("center -> ");
  Serial.print(SERVO_US_CENTER);
  Serial.println("us (90 deg)");
  delay(500);
}
