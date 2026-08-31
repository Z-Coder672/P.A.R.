// ServoLoadTest — 5 s of servo motion, 5 s idle, repeat. NOTHING ELSE.
//
// Purpose: a steady, predictable duty cycle you can hold a multimeter against
// while measuring the servo feed. The 5 s / 5 s square wave is the point — it
// gives you time to read V+ to GND at the SERVO CONNECTOR during motion, then
// see the idle baseline, without chasing a needle that only dips for 100 ms.
//
// NO GRBL. Serial1 is never opened, no G-code is sent, the carriage never
// moves. Only the servo link (Serial2 TX on D9) and the ack input (D2) are
// touched, so this is safe to run with the gantry powered or unpowered.
//
// WHAT TO LOOK FOR (feed measured at ~0.7 ohm loop, 5.08 V source):
//     holds near 5.0 V while moving   -> feed is fine, look elsewhere
//     sags below 4.8 V while moving   -> browning out; MG90S minimum is 4.8 V
//     sags to ~4.4 V at peak          -> matches the 0.7 ohm prediction
//     idle phase should sit at ~5.08 V either way
//
// LOADED vs UNLOADED: with no GRBL the head stays wherever it was left. If it
// is parked over a squisk the arm presses and you get the real (worst-case)
// current; if it is clear of the board you get the unloaded case. Both are
// worth measuring — unloaded first, then loaded, and compare the sag.
//
// The ack line is in OBSERVE mode deliberately: it reports misses but never
// blocks. Enforce mode retries forever, which would hang this sketch precisely
// when the supply collapses — the condition being measured.

const int SERVO_TX_PIN  = D9;   // Serial2 TX -> ServoNano RX (one-way, 9600 8N1)
const int SERVO_ACK_PIN = D2;   // ServoNano D3 via 1.8k/3.3k divider, idle HIGH

// Production pulse widths, so the current draw matches a real flip.
const int SERVO_US_A = 544;    // REST, arm parked
const int SERVO_US_B = 1317;   // ENGAGE, 75 deg — the largest excursion the rig uses

const int DWELL_MS   = 250;    // per leg; ~10 reversals per 5 s burst
const int MOVE_MS    = 5000;   // motion phase
const int IDLE_MS    = 5000;   // rest phase

// Same framing as the flip sketches: leading AND trailing newline so a stranded
// partial line can never merge with the next command.
const int SERVO_TX_REPEATS       = 1;
const int SERVO_TX_REPEAT_GAP_MS = 6;
const unsigned long ACK_WAIT_MS  = 60;   // must outlast ServoNano's ACK_HOLD_MS (40)

unsigned long ackMisses = 0;
unsigned long cycles    = 0;

void servoTxLine(int us) {
  char buf[12];
  snprintf(buf, sizeof(buf), "\n%d\n", us);
  for (int r = 0; r < SERVO_TX_REPEATS; r++) {
    Serial2.print(buf);
    Serial2.flush();            // block until the last stop bit is on the wire
    if (r + 1 < SERVO_TX_REPEATS) delay(SERVO_TX_REPEAT_GAP_MS);
  }
}

// ServoNano pulls the ack LOW for ACK_HOLD_MS on every accepted line.
bool servoAckSeen() {
  unsigned long t0 = millis();
  while (millis() - t0 < ACK_WAIT_MS) {
    if (digitalRead(SERVO_ACK_PIN) == LOW) return true;
  }
  return false;
}

void servoWrite(int us) {
  servoTxLine(us);
  if (!servoAckSeen()) {
    ackMisses++;
    Serial.print("  !! ack MISSING us="); Serial.print(us);
    Serial.print(" total="); Serial.println(ackMisses);
  }
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);   // TX only
  pinMode(SERVO_ACK_PIN, INPUT_PULLUP);                // unfitted wire reads HIGH = no ack

  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 8000) { delay(10); }
  delay(300);

  Serial.println();
  Serial.println("=== ServoLoadTest ===");
  Serial.println("No GRBL. Serial1 never opened. Carriage will not move.");
  Serial.print("cycling "); Serial.print(SERVO_US_A);
  Serial.print(" <-> "); Serial.print(SERVO_US_B);
  Serial.print(" us every "); Serial.print(DWELL_MS); Serial.println(" ms");
  Serial.print(MOVE_MS / 1000); Serial.print(" s moving / ");
  Serial.print(IDLE_MS / 1000); Serial.println(" s idle, repeating");
  Serial.println("Measure V+ to GND AT THE SERVO CONNECTOR. MG90S minimum is 4.8 V.");
  Serial.println();

  servoWrite(SERVO_US_A);   // known start position
  delay(500);
}

void loop() {
  cycles++;

  Serial.print(millis()); Serial.print("  MOVING  (cycle ");
  Serial.print(cycles); Serial.println(") — read the meter NOW");
  unsigned long t0 = millis();
  bool atB = false;
  while (millis() - t0 < (unsigned long)MOVE_MS) {
    servoWrite(atB ? SERVO_US_A : SERVO_US_B);
    atB = !atB;
    delay(DWELL_MS);
  }

  // Park before idling so the idle baseline is measured with the arm at REST,
  // not holding an engaged position against the board.
  servoWrite(SERVO_US_A);
  Serial.print(millis()); Serial.print("  IDLE    (arm parked at ");
  Serial.print(SERVO_US_A); Serial.println(" us) — this is your baseline");
  delay(IDLE_MS);
}
