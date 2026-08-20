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
const int SERVO_US_REST    = 565;
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
// The line is a LEVEL, not a UART: idle LOW, driven HIGH for ~40 ms on every
// command the ServoNano accepts. We already know what we sent, so all we need
// is "it landed".
//
// !! POLARITY IS ACTIVE-HIGH (inverted 2026-08-17). It used to be idle-HIGH /
// !! pulse-LOW with INPUT_PULLUP, on the theory that a broken wire would read
// !! HIGH = "no ack" = fail loud. That is only true if the break is at THIS
// !! pin. The divider's bottom-leg resistor sits between this node and GND, so
// !! a break ANYWHERE UPSTREAM -- the ack wire, an unplugged or unpowered
// !! ServoNano -- pinned the node LOW through it, which the old code read as
// !! "acked". Every command then reported success and SERVO_ACK_MODE 2's
// !! retry-until-confirmed guarantee became vacuous. No divider ratio fixes
// !! that; the polarity has to be the other way round.
// !!
// !! Now the bottom-leg resistor IS the fail-safe: any upstream break parks the
// !! node LOW = "no ack" = the enforced retry actually fires. INPUT_PULLDOWN
// !! covers the remaining case where the divider itself is absent.
// !!
// !! REQUIRES A 3.3k BOTTOM LEG. INPUT_PULLDOWN (~45k) sits in parallel with
// !! it, so with 1.8k/3.3k the HIGH level is 5*(3.3||45)/(1.8+(3.3||45)) =
// !! 3.15 V, comfortably over the ESP32-S3's V_IH of 0.75*VDD = 2.475 V. With a
// !! 2k bottom leg it collapses to 2.45 V and the ack stops working. Do NOT
// !! flash this onto a rig whose divider is 2k/2k.
// !!
// !! BOTH SIDES MUST BE FLASHED TOGETHER. A ServoNano running the old
// !! active-LOW build idles HIGH, which this code would read as a permanent
// !! ack -- the exact failure the change removes. servoAckProbeIdle() below
// !! detects that at boot and says so loudly.
//
// SERVO_ACK_MODE  0 = off      (no wire fitted; original open-loop behaviour)
//                 1 = observe  (log every missing ack, keep running)
//                 2 = enforce  (retry FOREVER; never move without the ack)
// Mode 2 is live: the divider + ack wire are fitted.
#define SERVO_ACK_MODE 2
const int SERVO_ACK_PIN = D2;
const unsigned long SERVO_ACK_TIMEOUT_MS = 80;   // must exceed the ~6 ms frame
unsigned long servoAckMisses = 0;
unsigned long servoAckStuck  = 0;   // line stuck asserted -> unverifiable
// Idle-wait budget. Must exceed ACK_HOLD_MS (40) so a legitimate hold from the
// PREVIOUS command is never mistaken for a stuck line.
const unsigned long SERVO_ACK_IDLE_TIMEOUT_MS = 100;

#if SERVO_ACK_MODE > 0
// Read the ack as an ANALOG level, not a digital one.
//
// !! WHY: the divider feeds a 5 V swing into a 3.3 V pin, so the asserted level
// !! depends entirely on the divider ratio, and digitalRead compares it against
// !! the ESP32-S3's V_IH of 0.75*VDD = 2.475 V. The rig is physically wired
// !! 2k/2k, which puts the asserted level at 2.45-2.50 V -- straddling V_IH, so
// !! digitalRead is a coin flip (and with INPUT_PULLDOWN it reads LOW outright,
// !! which would hang SERVO_ACK_MODE 2 forever on the first command). The shield
// !! PCB uses 2k/3.3k and would be fine, but the two must run one firmware.
// !!
// !! Comparing against a threshold far below BOTH divider ratios' asserted level
// !! removes the dependency on V_IH entirely: idle is 0 V (the divider's bottom
// !! leg IS the pulldown), asserted is 2.45 V at worst. 1.20 V sits >1.2 V from
// !! either state. This is strictly more robust than digitalRead ever was here,
// !! and it works unchanged on 2k/2k, 2k/3.3k and 1.8k/3.3k.
// !!
// !! SERVO_ACK_PIN = D2 = GPIO5 = ADC1_CH4. ADC1 is mandatory: ADC2 is unusable
// !! while WiFi is running, and PARMain always has WiFi up.
const int SERVO_ACK_THRESHOLD_MV = 1200;
static inline bool servoAckHigh() {
  return analogReadMilliVolts(SERVO_ACK_PIN) > SERVO_ACK_THRESHOLD_MV;
}

// Let the previous command's 40 ms hold expire so it cannot be mistaken for ours.
// Returns FALSE if the line never returned to idle -- i.e. it is stuck asserted.
//
// !! THIS RETURN VALUE IS SAFETY-CRITICAL, DO NOT IGNORE IT. Under the
// !! active-HIGH protocol a line stuck HIGH (ServoNano wedged mid-ack, a short
// !! to the divider's top leg, or a ServoNano still running the old active-LOW
// !! build) makes servoAckSeen() return true instantly and unconditionally.
// !! The ack would then confirm every command without any command having
// !! landed, and SERVO_ACK_MODE 2's retry-until-confirmed guarantee -- the one
// !! thing stopping the carriage from moving on an unconfirmed arm position --
// !! becomes vacuous. A stuck line must be treated as a FAULT, never as an ack.
static bool servoAckWaitIdle() {
  unsigned long t0 = millis();
  while (servoAckHigh()) {
    if (millis() - t0 >= SERVO_ACK_IDLE_TIMEOUT_MS) return false;
  }
  return true;
}
static bool servoAckSeen() {
  unsigned long t0 = millis();
  while (millis() - t0 < SERVO_ACK_TIMEOUT_MS)
    if (servoAckHigh()) return true;
  return false;
}

// Boot-time firmware-match check. Under the active-HIGH protocol the line must
// IDLE LOW; a ServoNano still running the old active-LOW build idles HIGH, and
// this code would then read every command as instantly acked. Returns false if
// the line never goes LOW -- stale ServoNano firmware, or a short to 5 V.
static bool servoAckProbeIdle() {
  unsigned long t0 = millis();
  while (millis() - t0 < 250)
    if (!servoAckHigh()) return true;
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
  unsigned long t0 = millis();
  unsigned long attempt = 0;
  bool acked = false;
  do {
    attempt++;
    // Stuck-asserted line: an ack read now would be meaningless. Treat exactly
    // like a missing ack -- block and retry -- rather than believing it.
    if (!servoAckWaitIdle()) {
      servoAckStuck++;
      if (attempt <= 5 || (attempt % 100) == 0) {
        Serial.print("!! servo ack STUCK ASSERTED - cannot verify, blocking. us=");
        Serial.print(us); Serial.print(" attempt="); Serial.println(attempt);
      }
#if SERVO_ACK_MODE == 1
      break;                    // observe-only: record it and carry on
#else
      delay(50);
      continue;                 // mode 2: never accept an unverifiable ack
#endif
    }
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
  pinMode(SERVO_ACK_PIN, INPUT);  // divider's bottom leg is the pulldown
#if SERVO_ACK_MODE > 0
  // Active-HIGH ack: the line must idle LOW. Idling HIGH means the
  // ServoNano still has the old active-LOW firmware.
  if (!servoAckProbeIdle())
    Serial.println(F("ACK LINE IDLES HIGH - ServoNano is probably still running the OLD active-LOW build. The ack is NOT protecting you: flash ServoNano.ino before running any job."));
#endif
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
