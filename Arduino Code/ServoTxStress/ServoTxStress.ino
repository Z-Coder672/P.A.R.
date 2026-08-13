// ServoTxStress — Nano ESP32 side of the servo-link error-rate measurement.
//
// NO GRBL, NO MOTION. This sketch only drives the hardware UART (Serial2) on
// D9 (the ServoNano link). Serial1 is never opened, nothing is homed, nothing
// moves. Pair with ServoLinkProbe.ino on the Nano, which pins the servo at REST.
//
// servoTxLine is copied VERBATIM from PARMain.ino / FlipAllTest.ino.
//
// PORT NOTE: on the RP2040 this sketch existed to sweep SERVO_TX_BIT_US, because
// the frame was bit-banged and a mistuned bit period was a prime suspect for the
// dropped commands (bench answer: 102 measured 1061.94 us/byte, ~2 % SLOW, and
// 91 k bytes produced zero dropouts). Serial2 is a real UART peripheral clocked
// off APB, so there is no bit period left to tune and the +/- keys are gone.
// The 't' key remains as a sanity check that the UART really is at 9600 8N1 --
// it now times Serial2.write()+flush(), which is a wire-time measurement, not a
// software-loop measurement. What is still worth stressing here is everything
// downstream of the framing: the ServoNano's SoftwareSerial receiver, the cable,
// and the shared 5 V/ground.
//
// USB serial keys:  t      print measured per-byte wire time + UART config
//                   space  pause / resume

const int SERVO_TX_PIN = D9;

// Gap between lines. Production waits 100-300 ms (the servo settle), but the
// failure mechanism lives in the byte-to-byte timing INSIDE a line, not between
// lines. 25 ms is still long enough for the receiver's 9600-baud echo +
// "[ok NNNN]" to drain before the next line starts (~17 ms), so the receiver is
// in the same state at the start of each line as it is in production, while
// giving ~8x the sample rate.
const int LINE_GAP_MS = 25;

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

// The exact 4-command pattern one flipDisc() emits.
const int PATTERN[4] = {1471, 544, 1018, 544};
uint8_t idx = 0;
unsigned long lines = 0;
bool paused = false;

// Measure the real per-byte WIRE time. flush() returns only once the last stop
// bit has left the shift register, so 100 flushed bytes time the peripheral, not
// the CPU. Should land within a fraction of a percent of the nominal 1041.67 us.
void measure() {
  unsigned long t0 = micros();
  for (int i = 0; i < 100; i++) { Serial2.write('0'); Serial2.flush(); }
  unsigned long dt = micros() - t0;
  Serial2.write('\n'); Serial2.flush();   // flush the receiver's line state
  Serial.print("Serial2 9600 8N1 on D9 (TX only)");
  Serial.print("  measured frame = "); Serial.print(dt / 100.0, 2);
  Serial.print(" us/byte  (nominal 9600 = 1041.67)  -> effective baud ");
  Serial.println(10.0e6 / (dt / 100.0), 1);
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);  // TX-only servo link on D9
  delay(2000);
  Serial.println("ServoTxStress: servo UART link only, no GRBL, no motion.");
  measure();
}

void loop() {
  while (Serial.available()) {
    char k = Serial.read();
    if (k == 't') measure();
    if (k == ' ') { paused = !paused; Serial.println(paused ? "paused" : "running"); }
  }
  if (paused) { delay(10); return; }

  servoTxLine(PATTERN[idx]);
  idx = (idx + 1) & 3;
  if (++lines % 4000 == 0) { Serial.print("tx lines="); Serial.println(lines); }
  // Real flips wait 100-300 ms here; the failure mechanism lives in the
  // byte-to-byte timing inside a line, not the gap between lines, so shorten
  // the gap to get a usable sample count. Still long enough for the receiver
  // to finish its line handling.
  delay(LINE_GAP_MS);
}
