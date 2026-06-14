// ServoCycle — cycle the servo through the flip angles forever, with ZERO GRBL
// motion (Serial1 never started), so the carriage cannot move. Maximally safe:
// only the servo actuates, wherever the carriage currently sits.
//
// Drives the production bit-banged-UART -> ServoNano path. Purpose: reproduce
// the symptom "main servo stops while the witness keeps moving" under sustained
// max-duty actuation (thermal/wire onset), with both servos in camera view.
//
// Per cycle (mimics flipDisc's servo moves): ENGAGE 90 -> REST 0 -> RELEASE ~38
// -> REST 0. ~0.8s/cycle. Prints cycle count + uptime so a failure can be tied
// to time (thermal) and cycle number.

const int SERVO_TX_PIN = 9;
const int SERVO_TX_BIT_US = 102;
const int SERVO_US_REST    = 544;
const int SERVO_US_RELEASE = 1018;  // ≈46° (raised 8° from the prior 936/≈38°)
const int SERVO_US_ENGAGE  = 1471;
const int SERVO_90_DEG_SETTLE_MS = 300;
const int SERVO_50_DEG_SETTLE_MS = 100;

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
void writeServoUs(int us, int settle_ms) { servoTxLine(us); delay(settle_ms); }

unsigned long cycle = 0, startMs = 0;

void setup() {
  Serial.begin(115200);
  pinMode(SERVO_TX_PIN, OUTPUT);
  digitalWrite(SERVO_TX_PIN, HIGH);  // UART idle high
  // Serial1 / GRBL intentionally untouched — carriage stays put.
  servoTxLine(SERVO_US_REST);
  delay(1000);
  startMs = millis();
}

void loop() {
  writeServoUs(SERVO_US_ENGAGE,  SERVO_90_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_REST,    SERVO_90_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_RELEASE, SERVO_50_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_REST,    SERVO_50_DEG_SETTLE_MS);
  cycle++;
  unsigned long el = millis() - startMs;
  Serial.print("CYCLE "); Serial.print(cycle);
  Serial.print("  t="); Serial.print(el / 1000UL); Serial.println("s");
}
