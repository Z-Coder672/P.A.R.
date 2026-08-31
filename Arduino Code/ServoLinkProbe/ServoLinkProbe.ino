// ServoLinkProbe — instrumented stand-in for ServoNano.ino, used to measure the
// error rate of the bit-banged RP2040 D9 -> Nano D2 software UART.
//
// MOTION SAFETY: the servo is attached and pinned at REST for the whole run and
// is NEVER commanded anywhere else, whatever arrives on the link. Nothing moves.
//
// It parses lines with the EXACT logic of ServoNano.ino (String accumulate,
// trim(), toInt() truncated to int, 544..2400 range check) so any value this
// sketch reports as "accepted" is a value the real sketch would have written to
// the servo. It then compares against the expected cyclic pattern that
// ServoTxStress sends, and reports every deviation with the raw bytes.
//
// The byte echo (Serial.write(c) per received byte, as ServoNano.ino does) is a
// suspect in its own right: it makes the hardware UART's UDRE interrupt fire in
// phase with the incoming byte stream, right where SoftwareSerial re-arms its
// pin-change mask. Send 'e' / 'q' over USB serial to enable/disable the echo
// mid-run and compare error rates.

#include <Servo.h>
#include <SoftwareSerial.h>

#define SERVO_PIN 9
#define RX_PIN 2
#define TX_PIN 3

const int SERVO_US_PARK = 544;   // the only value ever written. Never changes.

Servo s;
SoftwareSerial link(RX_PIN, TX_PIN);

String inputLine;

// Expected pattern sent by ServoTxStress (one "flip" = 4 lines).
const int EXPECTED[4] = {1471, 544, 1018, 544};
uint8_t expIdx = 0;
bool synced = false;

unsigned long nBytes = 0, nLines = 0, nAccepted = 0, nRejected = 0;
unsigned long nMismatch = 0, nDropSuspect = 0;
bool echoBytes = true;

// Raw bytes of the current line, for anomaly reporting.
uint8_t raw[40];
uint8_t rawN = 0;

void printRaw() {
  Serial.print(" raw=[");
  for (uint8_t i = 0; i < rawN; i++) {
    if (raw[i] >= 32 && raw[i] < 127) Serial.write((char)raw[i]);
    else { Serial.print("<"); Serial.print(raw[i], HEX); Serial.print(">"); }
  }
  Serial.print("]");
}

void tally(const char* why) {
  Serial.print(" | bytes="); Serial.print(nBytes);
  Serial.print(" lines="); Serial.print(nLines);
  Serial.print(" acc="); Serial.print(nAccepted);
  Serial.print(" rej="); Serial.print(nRejected);
  Serial.print(" mismatch="); Serial.print(nMismatch);
  Serial.print(" merges="); Serial.print(nDropSuspect);
  Serial.print(" echo="); Serial.print(echoBytes ? 1 : 0);
  Serial.print(" ("); Serial.print(why); Serial.println(")");
}

// PRODUCTION_ECHO reproduces ServoNano.ino exactly: USB serial at 9600 with the
// per-byte echo on. That matters — at 9600 the echo of byte N is still being
// shifted out when byte N+1 arrives, so the UART's UDRE interrupt fires in phase
// with the incoming byte stream, right where SoftwareSerial re-arms its
// pin-change mask. At 115200 the echo finishes long before the next byte and
// that collision disappears. Build both ways and compare the error rates.
#define PRODUCTION_ECHO

#ifdef PRODUCTION_ECHO
const long REPORT_BAUD = 9600;
#else
const long REPORT_BAUD = 115200;
#endif

void setup() {
  Serial.begin(REPORT_BAUD);
  link.begin(9600);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  s.attach(SERVO_PIN, 544, 2400);
  s.writeMicroseconds(SERVO_US_PARK);   // park, and never touch it again
  Serial.println("ServoLinkProbe ready (servo pinned at REST, will not move)");
}

void loop() {
  while (Serial.available()) {
    char k = Serial.read();
    if (k == 'e') { echoBytes = true;  tally("echo on"); }
    if (k == 'q') { echoBytes = false; tally("echo off"); }
    if (k == 'r') { nBytes = nLines = nAccepted = nRejected = 0;
                    nMismatch = nDropSuspect = 0; synced = false; tally("reset"); }
  }

  while (link.available()) {
    char c = (char)link.read();
    nBytes++;
    if (echoBytes) Serial.write(c);      // same load ServoNano.ino puts on the UART
    if (rawN < sizeof(raw)) raw[rawN++] = (uint8_t)c;

    if (c == '\n' || c == '\r') {
      nLines++;
      inputLine.trim();
      if (inputLine.length()) {
        int us = inputLine.toInt();      // long -> int truncation, exactly as ServoNano.ino
        bool accepted = (us >= 544 && us <= 2400);
        if (accepted) nAccepted++; else nRejected++;
#ifdef PRODUCTION_ECHO
        // ServoNano.ino's per-line acknowledgement, reproduced so the UART TX
        // load matches production byte for byte.
        if (accepted) { Serial.print("[ok "); Serial.print(us); Serial.println("]"); }
        else          { Serial.print("[bad "); Serial.print(inputLine); Serial.println("]"); }
#endif

        if (!synced) {
          // sync to the pattern on the first clean 1471
          if (accepted && us == EXPECTED[0]) { synced = true; expIdx = 1; }
        } else {
          int want = EXPECTED[expIdx];
          if (!(accepted && us == want)) {
            nMismatch++;
            // a "merge" is the dangerous case: a line long enough to be two
            // commands run together, that STILL passed the range check
            bool merged = (inputLine.length() >= 7);
            if (accepted && merged) nDropSuspect++;
            Serial.println();
            Serial.print("*** ANOMALY want="); Serial.print(want);
            Serial.print(" got=");
            if (accepted) Serial.print(us); else Serial.print("REJECTED");
            Serial.print(" len="); Serial.print(inputLine.length());
            if (accepted && merged) Serial.print("  <<< MERGE: SERVO WOULD MOVE HERE");
            printRaw();
            tally("anomaly");
            synced = false;               // re-sync on the next clean 1471
          }
          expIdx = (expIdx + 1) & 3;
        }
      }
      inputLine = "";
      rawN = 0;
      if ((nLines % 4000) == 0) tally("heartbeat");
    } else {
      inputLine += c;
    }
  }
}
