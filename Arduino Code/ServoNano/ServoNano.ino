#include <Servo.h>
#include <SoftwareSerial.h>

#define SERVO_PIN 9
#define RX_PIN 2     // wire RP2040 D9 (bit-banged TX) here

// SoftwareSerial insists on owning a TX pin even though this link is one-way.
// Point it at an genuinely unused pin -- D3 is the ACK output now, and letting
// SoftwareSerial drive D3 as its idle-high TX would fight the ack pulses.
#define SS_DUMMY_TX_PIN 5

// ---------------------------------------------------------------- ack line
// D3 -> resistor divider -> RP2040 D2. The RP2040 is NOT 5V tolerant (abs max
// IOVDD+0.3 = 3.6 V), so this pin must never reach it directly:
//
//   ServoNano D3 ---[1.8k]---+---> RP2040 D2
//                            |
//                          [3.3k]
//                            |
//                           GND        -> 5.0 * 3.3/(1.8+3.3) = 3.24 V
//
// The 3.3k leg is now load-bearing in two ways: it sets the level AND it is
// what parks the line LOW (= no ack) if this board goes away. The main MCU
// adds INPUT_PULLDOWN, which drops the HIGH to ~3.15 V -- still well over the
// ESP32-S3's 2.475 V V_IH. A 2k bottom leg would give 2.45 V and NOT work.
//
// (The other direction needs nothing: the RP2040's 3.3 V clears this AVR's
// V_IH of 0.6*Vcc = 3.0 V, which is why the existing D9->D2 link works.)
//
// Protocol is a LEVEL, not a UART: idle LOW, driven HIGH for ACK_HOLD_MS on
// every accepted command. The main MCU already knows what value it sent, so it
// only needs "the command landed" -- no baud matching and no bit timing. The
// hold must outlast the sender's frame and stay shorter than the smallest
// settle (100 ms), so one command's ack can never be read as the next one's.
//
// !! POLARITY IS ACTIVE-HIGH (inverted 2026-08-17). It was idle-HIGH /
// !! pulse-LOW, with the main MCU using INPUT_PULLUP so a broken wire would
// !! read HIGH = "no ack". That reasoning only held for a break at the MCU pin.
// !! The divider's bottom-leg resistor sits between that node and GND, so a
// !! break anywhere upstream -- this wire, or an unplugged/unpowered ServoNano
// !! -- pinned the node LOW, which the old firmware read as a successful ack.
// !! Every command then "succeeded" and the enforced retry protected nothing.
// !!
// !! Idle LOW makes the bottom-leg resistor the fail-safe instead: lose this
// !! board and the line parks LOW = "no ack" = the main MCU retries forever
// !! instead of driving the carriage on an unconfirmed arm position.
// !!
// !! FLASH BOTH SIDES TOGETHER. A main MCU on the old build reads idle-LOW as a
// !! permanent ack; this build's idle HIGH would look the same to it. The main
// !! sketches probe the line at boot and log loudly on a mismatch.
#define ACK_PIN 3
const unsigned long ACK_HOLD_MS = 40;

// Discard a partially-received line if nothing new arrives for this long. The
// sender writes a whole line back-to-back in ~5 ms and repeats it every
// SERVO_TX_REPEAT_GAP_MS (6 ms), so a gap this large means the terminator was
// lost. Without this, a dropped byte leaves digits sitting in inputLine and they
// get glued onto the NEXT command.
const unsigned long LINE_STALE_MS = 50;

// LED acknowledge blink, non-blocking. This used to be `delay(50)` inline, which
// left the sketch deaf to the link for 50 ms after every accepted command --
// exactly when the sender's repeat copies are arriving.
const unsigned long LED_BLINK_MS = 30;

Servo s;
SoftwareSerial link(RX_PIN, SS_DUMMY_TX_PIN);

String inputLine;
unsigned long lastByteMs = 0;
unsigned long ledOffAtMs = 0;
unsigned long ackReleaseAtMs = 0;
int lastAppliedUs = -1;
unsigned long nRejected = 0;

// Parse one received line into a pulse width.
//
// Deliberately paranoid. The link is a bit-banged software UART with no
// handshake or checksum, so the only defence against a corrupted frame is
// refusing to act on anything that is not exactly what the sender is known to
// emit: 3 or 4 decimal digits, nothing else.
//
// The bug this replaces: `int us = inputLine.toInt()` truncated toInt()'s long
// to a 16-bit int BEFORE the 544..2400 range check, so two commands glued
// together by a dropped newline wrapped mod 65536 straight into the valid
// window -- "544"+"1018" -> 5441018 & 0xFFFF = 1530 us, "544"+"1471" ->
// 1983 us. Both sit at or past SERVO_US_ENGAGE, so a single lost byte threw the
// arm to engage mid-flip and squisked a disc 90 degrees. Keeping the value in a
// long and capping the digit count makes every such merge impossible.
//
// Note this can only reject a BAD command, never recover a LOST one -- if the
// digits are dropped there is nothing to act on and the arm holds its last
// position. That failure broke the flip arm (a lost REST left it at ENGAGE
// through both of flipDisc's X strokes), which is what the ack line above
// exists to catch. See "Servo link framing" in CLAUDE.md.
bool parsePulseUs(const String& line, int& out) {
  unsigned int n = line.length();
  if (n < 3 || n > 4) return false;              // 544..2400 is always 3-4 digits
  for (unsigned int i = 0; i < n; i++) {
    char c = line[i];
    if (c < '0' || c > '9') return false;        // no sign, space or stray byte
  }
  long v = line.toInt();                         // stays long -- no 16-bit wrap
  if (v < 544 || v > 2400) return false;
  out = (int)v;
  return true;
}

void setup() {
  Serial.begin(9600);          // USB-Serial, for debug echo
  link.begin(9600);            // matches the RP2040's bit-banged TX
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  pinMode(ACK_PIN, OUTPUT);
  digitalWrite(ACK_PIN, LOW);  // idle LOW (active-HIGH ack)
  // 544/2400 is the LIBRARY pulse range (0-180 deg); the boot park is REST,
  // which is 2 deg = 565us. Don't collapse the two -- the range must stay at
  // the library floor or every commanded angle shifts.
  s.attach(SERVO_PIN, 544, 2400);
  s.writeMicroseconds(565);
  lastAppliedUs = 565;
  Serial.println("ServoNano ready");
}

void loop() {
  unsigned long now = millis();

  if (ledOffAtMs && now >= ledOffAtMs) {
    digitalWrite(LED_BUILTIN, LOW);
    ledOffAtMs = 0;
  }
  if (ackReleaseAtMs && now >= ackReleaseAtMs) {
    digitalWrite(ACK_PIN, LOW);   // release: back to idle
    ackReleaseAtMs = 0;
  }

  // Drop a stalled partial line rather than letting it merge with the next one.
  if (inputLine.length() && (now - lastByteMs) > LINE_STALE_MS) {
    Serial.print("[stale ");
    Serial.print(inputLine);
    Serial.println("]");
    inputLine = "";
  }

  while (link.available()) {
    char c = (char)link.read();
    lastByteMs = millis();
    Serial.write(c);  // echo every byte that arrives so you can see corruption

    if (c == '\n' || c == '\r') {
      inputLine.trim();
      if (inputLine.length()) {
        int us;
        if (parsePulseUs(inputLine, us)) {
          s.writeMicroseconds(us);
          // Assert the ack for EVERY accepted line, including the sender's
          // repeat copies -- the RP2040 only needs to see it once, and
          // re-asserting simply extends the hold.
          digitalWrite(ACK_PIN, HIGH);  // assert ack
          ackReleaseAtMs = millis() + ACK_HOLD_MS;
          digitalWrite(LED_BUILTIN, HIGH);
          ledOffAtMs = millis() + LED_BLINK_MS;
          // Only ack a CHANGE over USB. The sender repeats each command 3x;
          // printing all three would triple the 9600-baud traffic and can back
          // up the 64-byte TX ring, which stalls this loop.
          if (us != lastAppliedUs) {
            Serial.print("[ok ");
            Serial.print(us);
            Serial.println("]");
            lastAppliedUs = us;
          }
        } else {
          nRejected++;
          Serial.print("[bad ");
          Serial.print(inputLine);
          Serial.print(" n=");
          Serial.print(nRejected);
          Serial.println("]");
        }
      }
      inputLine = "";
    } else {
      inputLine += c;
    }
  }
}
