// ServoCycle — cycle the servo through the flip angles forever, with ZERO GRBL
// motion (Serial1 never started), so the carriage cannot move. Maximally safe:
// only the servo actuates, wherever the carriage currently sits.
//
// Drives the production hardware-UART -> ServoNano path. Purpose: reproduce
// the symptom "main servo stops while the witness keeps moving" under sustained
// max-duty actuation (thermal/wire onset), with both servos in camera view.
//
// Per cycle (mimics flipDisc's servo moves): ENGAGE 90 -> REST 0 -> RELEASE ~38
// -> REST 0. ~0.8s/cycle. Prints cycle count + uptime so a failure can be tied
// to time (thermal) and cycle number.

const int SERVO_TX_PIN = D9;
const int SERVO_US_REST    = 544;
const int SERVO_US_RELEASE = 1018;  // ≈46° (raised 8° from the prior 936/≈38°)
const int SERVO_US_ENGAGE  = 1471;
const int SERVO_90_DEG_SETTLE_MS = 300;
const int SERVO_50_DEG_SETTLE_MS = 100;

// PORT (Arduino Nano ESP32): the RP2040 bit-banged this 9600-baud frame on D9
// with interrupts disabled (servoTxByte + SERVO_TX_BIT_US, both deleted). The
// ESP32-S3 GPIO matrix routes a real UART to any pin, so the link is now
// hardware Serial2 TX on the SAME physical D9 wire -- same 9600 8N1 framing, no
// ISR blackout, no bit-period tuning. RX is unused (-1): the link is still
// one-way; the ack is the separate D2 level line.
// The link is ONE-WAY with no ack, so a dropped byte silently LOSES a command
// and the arm simply stays where it was. That broke the flip arm once: a lost
// REST left it at ENGAGE, and flipDisc then ran both X strokes with the arm
// buried in the board. A receiver-side check cannot help -- a command that never
// arrives cannot be rejected -- so every command is sent SERVO_TX_REPEATS times.
// writeMicroseconds() is idempotent, so the repeats are free: re-commanding the
// position the servo already holds does nothing. Losing a command now takes
// SERVO_TX_REPEATS independent dropouts instead of one.
//
// The repeats also fix LATE application: if only the trailing newline is lost,
// the stranded digits sit in the ServoNano's buffer until the NEXT command's
// leading newline flushes them -- which without repeats is up to a full settle
// period later, i.e. after the stroke has already started. The next repeat
// flushes them SERVO_TX_REPEAT_GAP_MS later instead.
// With the ack line fitted (SERVO_ACK_MODE 2) blind repeats are OBSOLETE and
// actively harmful: writeServoUs() now detects a lost command and retries, which
// is strictly better than sending 3 copies and hoping. Each extra copy costs the
// ServoNano ~6 ms with INTERRUPTS DISABLED (SoftwareSerial::recv holds them off
// for 9.75 bit times = 1.02 ms per byte), and the Servo library needs its Timer1
// ISR on time. At 3 copies every command spanned 1.56 servo frames at 59 %
// blocked duty, so a 544 us REST pulse was routinely stretched by up to 1016 us
// -> ~1560 us, which IS the ENGAGE command. The servo twitched toward engage on
// nearly every command (audible buzzing, no stall), and when enough consecutive
// pulses were stretched the arm never left ENGAGE inside the 300 ms settle --
// the arm-breaking failure, made ~40x more common by the repeats meant to fix it.
// Keep this at 1 whenever SERVO_ACK_MODE is 2.
const int SERVO_TX_REPEATS = 1;
const int SERVO_TX_REPEAT_GAP_MS = 6;

void servoTxLine(int us) {
  char buf[12];
  snprintf(buf, sizeof(buf), "\n%d\n", us);
  for (int r = 0; r < SERVO_TX_REPEATS; r++) {
    Serial2.print(buf);
    // Block until the last stop bit is actually on the wire, so this call
    // stays synchronous like the old bit-bang did -- callers time their
    // settle delay from here.
    Serial2.flush();
    if (r + 1 < SERVO_TX_REPEATS) delay(SERVO_TX_REPEAT_GAP_MS);
  }
}

// ---------------------------------------------------------------- servo ack
// ServoNano D3 --[1.8k]--+--> this pin (D2);  3.3k from that junction to GND.
// The divider is MANDATORY: the ESP32-S3 is not 5V tolerant (abs max VDD+0.3 =
// 3.6 V) and the ServoNano drives 5 V. 5.0*3.3/(1.8+3.3) = 3.24 V. See
// ServoNano.ino. The other direction (D9 -> ServoNano) needs nothing, since
// 3.3 V clears the AVR's V_IH of 0.6*Vcc = 3.0 V.
//
// The line is a LEVEL, not a UART: idle HIGH, held LOW for ~40 ms on every
// command the ServoNano accepts. We already know what we sent, so all we need
// is "it landed". INPUT_PULLUP so a broken or unfitted wire reads HIGH, i.e.
// "no ack" -- it fails loud rather than silently reporting success.
//
// SERVO_ACK_MODE  0 = off      (no wire fitted; original open-loop behaviour)
//                 1 = observe  (log every missing ack, keep running)
//                 2 = enforce  (retry FOREVER; never move without the ack)
// Mode 2 is live: the divider + ack wire are fitted.
#define SERVO_ACK_MODE 2
const int SERVO_ACK_PIN = D2;
const unsigned long SERVO_ACK_TIMEOUT_MS = 80;   // must exceed the ~6 ms frame
unsigned long servoAckMisses = 0;

#if SERVO_ACK_MODE > 0
// Let the previous command's 40 ms hold expire so it cannot be mistaken for ours.
static void servoAckWaitIdle() {
  unsigned long t0 = millis();
  while (digitalRead(SERVO_ACK_PIN) == LOW && millis() - t0 < 100) {}
}
static bool servoAckSeen() {
  unsigned long t0 = millis();
  while (millis() - t0 < SERVO_ACK_TIMEOUT_MS)
    if (digitalRead(SERVO_ACK_PIN) == LOW) return true;
  return false;
}
// Mode 2 RETRIES FOREVER rather than giving up. Blocking here is the safe
// failure: every writeServoUs() call site is reached with GRBL already idle
// (flipDisc waits for motion before each servo move, and scanGrid's mid-scan
// servo changes happen before the next move is queued), so a stall leaves the
// carriage stationary with no G-code in flight. It deliberately does NOT reset
// the MCU on failure -- a reset re-homes, and homing would drag the carriage
// with the arm possibly still at ENGAGE, which is exactly how the arm broke.
#endif

void writeServoUs(int us, int settle_ms) {
#if SERVO_ACK_MODE == 0
  servoTxLine(us);
  delay(settle_ms);
#else
  servoAckWaitIdle();
  unsigned long t0 = millis();
  unsigned long attempt = 0;
  bool acked = false;
  do {
    attempt++;
    servoTxLine(us);
    acked = servoAckSeen();
    if (!acked) {
      servoAckMisses++;
      // Throttle: a disconnected ack wire would otherwise flood the log.
      if (attempt <= 5 || (attempt % 100) == 0) {
        Serial.print("!! servo ack MISSING us="); Serial.print(us);
        Serial.print(" attempt="); Serial.print(attempt);
        Serial.print(" totalMisses="); Serial.println(servoAckMisses);
      }
    }
#if SERVO_ACK_MODE == 1
    break;                      // observe-only: record the miss and carry on
#endif
  } while (!acked);             // mode 2: retry FOREVER, never move unconfirmed
  unsigned long spent = millis() - t0;
  if ((unsigned long)settle_ms > spent) delay(settle_ms - spent);
#endif
}

unsigned long cycle = 0, startMs = 0;

void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);  // TX-only servo link on D9
  pinMode(SERVO_ACK_PIN, INPUT_PULLUP);
  // Serial1 / GRBL intentionally untouched — carriage stays put.
  servoTxLine(SERVO_US_REST);
  delay(1000);
  startMs = millis();
}

// Log every send with the cycle number and the move's position in the sequence,
// so a failure called out at the bench can be pinned to an exact command and
// cross-referenced against the ServoNano's own log for the same instant. Step
// names match flipDisc: ENGAGE, REST1 (the one whose loss leaves the arm at
// engage through both X strokes), RELEASE, REST2.
void move(const char* step, int us, int settle_ms) {
  Serial.print("c"); Serial.print(cycle);
  Serial.print(" "); Serial.print(step);
  Serial.print(" us="); Serial.print(us);
  Serial.print(" t="); Serial.println(millis() - startMs);
  writeServoUs(us, settle_ms);
}

void loop() {
  cycle++;
  move("ENGAGE",  SERVO_US_ENGAGE,  SERVO_90_DEG_SETTLE_MS);
  move("REST1",   SERVO_US_REST,    SERVO_90_DEG_SETTLE_MS);
  move("RELEASE", SERVO_US_RELEASE, SERVO_50_DEG_SETTLE_MS);
  move("REST2",   SERVO_US_REST,    SERVO_50_DEG_SETTLE_MS);
}
