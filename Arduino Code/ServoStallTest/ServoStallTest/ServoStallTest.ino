// ServoStallTest — send a degree value (0-180) over USB serial and continuously
// print the servo supply current sensed on A0. Stall diagnostic.
//
// HARDWARE NOTE (current rig): the main MCU does not drive the servo directly.
// The old D9-as-PWM path (Servo.h + attach(9)) is RETIRED. D9 is now a
// hardware 9600-baud UART TX (Serial2) feeding a dedicated 5V Arduino Nano
// ("ServoNano"), which parses one integer microsecond value per line and owns
// the actual SG90 PWM. So this sketch commands only a TARGET pulse width; the
// PWM waveform, and therefore the stall behaviour, lives on the ServoNano.
// The ServoNano echoes each received line plus `[ok <us>]` or `[bad ...]` over
// its own USB serial — that echo is the signal-integrity probe for this link,
// so watch the ServoNano's port alongside this one when interpreting a stall.

// D9 is a UART TX line to the ServoNano, NOT a PWM servo output. Driving
// Servo.h PWM here would be decoded as garbage UART frames.
const int SERVO_TX_PIN = D9;
const int SERVO_US_REST = 544;  // 0° park, matches PARMain

const int SENSE_PIN = A0;

String inputBuffer = "";

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
const int SERVO_TX_REPEATS = 3;
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

void writeServoUs(int us, int settle_ms) {
  servoTxLine(us);
  delay(settle_ms);
}

// Standard Servo-library mapping: 544us at 0° to 2400us at 180° (~10.31 us/deg).
// Replaces the retired servo.write(deg) call.
int angleToMicros(int deg) {
  long us = (long)SERVO_US_REST + (long)deg * (2400L - SERVO_US_REST) / 180L;
  return (int)us;
}

void setup() {
  Serial.begin(9600);
  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);  // TX-only servo link on D9
  // PORT: the ESP32 core's analogRead() defaults to 12-bit (0-4095); the RP2040
  // returned 10 bits. The divider math below is calibrated against a 0-1023
  // full scale, so pin the resolution rather than rescale the constants -- that
  // keeps the /1023.0 and the 10.2k/5.1k divider exactly as they were measured.
  analogReadResolution(10);
  delay(100);
  servoTxLine(SERVO_US_REST);
  Serial.println("Ready. Send a degree value (0-180).");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      int deg = inputBuffer.toInt();
      if (deg >= 0 && deg <= 180) {
        writeServoUs(angleToMicros(deg), 0);
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
