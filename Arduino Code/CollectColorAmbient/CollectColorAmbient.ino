// CollectColorAmbient — TCS3200 color-sensor logger with LED ambient-subtraction
// ----------------------------------------------------------------------------
// Target: vanilla Arduino Nano (ATmega328), FQBN arduino:avr:nano.
// No GRBL, no servo, no WiFi, no motion — just the color sensor, a bank of
// controllable LEDs, and USB serial. Used to collect color training data on a
// standalone test board.
//
// PIN MAP (exact)
//   TCS3200 S0 = D5, S1 = D6   frequency-scaling pins
//   TCS3200 S2 = D7, S3 = D8   photodiode filter-select pins (MSB=S2, LSB=S3)
//   TCS3200 OUT = D10          digital input; 50%-duty square wave whose
//                              frequency is proportional to light intensity
//                              for the selected filter
//   LEDs      = D9             OUTPUT; drives an NPN transistor.
//                              D9 HIGH = LEDs ON, D9 LOW = LEDs OFF.
//
// SENSOR CONFIG (matches the PARMain project conventions)
//   S0=HIGH, S1=LOW selects 20% output frequency scaling. Full-speed (HIGH/HIGH)
//   tops out near 600 kHz, which is past what pulseIn can resolve cleanly.
//   S2/S3 select the filter bank:
//     RED:   S2=LOW,  S3=LOW
//     BLUE:  S2=LOW,  S3=HIGH
//     CLEAR: S2=HIGH, S3=LOW
//     GREEN: S2=HIGH, S3=HIGH
//   Each channel is read as a frequency in Hz: pulseIn() times one HIGH
//   half-period (µs), then frequency = 500000 / halfUs (guard halfUs==0 → 0).
//   A delay(2) after selecting a filter paces reads at 2 ms (train-time cadence).
//
// AMBIENT-SUBTRACTION READING (the key feature)
//   One "sample" = 5 cycles averaged. Each cycle:
//     1. LEDs OFF (D9 LOW), settle ~20 ms, read R,G,B,C  → ambient readings.
//     2. LEDs ON  (D9 HIGH), settle ~20 ms, read R,G,B,C → lit readings.
//     3. Accumulate (lit - ambient) per channel (signed math).
//   After 5 cycles, divide each accumulator by 5. Negative results (noise) are
//   clamped to 0. LEDs are left OFF between samples.
//
// SERIAL PROTOCOL (115200 baud)
//   On boot, prints a "# ... ready" banner on its own line.
//   Idle until a command char arrives:
//     'b' → begin BLUE:  prints <blue_data>  on its own line, then streams samples.
//     'k' → begin BLACK: prints <black_data> on its own line, then streams samples.
//     's' → stop:        prints the matching </blue_data> or </black_data> and
//                        returns to idle.
//   While recording, each sample is printed as one CSV line "r,g,b,c" (integers,
//   ambient-subtracted, no label prefix — the surrounding tag identifies class).
//   Any number of tagged blocks may follow, one after another. 'b'/'k' are
//   ignored while already recording; 's' is ignored while idle; whitespace and
//   any other characters are ignored. Serial is checked at least once per sample
//   so 's' stays responsive.
// ----------------------------------------------------------------------------

// ---- Pin assignments --------------------------------------------------------
const int TCS_S0  = D5;   // frequency-scaling select (with S1)
const int TCS_S1  = D6;
const int TCS_S2  = D7;   // filter select MSB
const int TCS_S3  = D8;   // filter select LSB
const int TCS_OUT = D10;  // sensor output (square wave, digital input)
const int LED_PIN = D9;   // NPN base: HIGH = LEDs ON

// ---- Sampling constants -----------------------------------------------------
const int  AVG_CYCLES  = 5;    // cycles averaged per reported sample
const int  LED_SETTLE_MS = 20; // settle after toggling the LEDs before reading

// S2/S3 select the photodiode filter bank. Datasheet names; whether they
// are physically right on this rig is unresolved and does not matter.
//
// !! CHANNEL: classifyDisc thresholds the `c` slot -- TcsFilter value 2,
// !! commanded (S2=H, S3=L). Reversed 2026-08-22 from the 2026-08-20 decision
// !! to threshold `b`; slot `b`'s two populations OVERLAP and cannot be
// !! separated by any cut. Measured on 666 cells of full-board ground truth:
// !! best achievable errors r=7 g=6 b=14 c=6, and 40 earlier jobs (26,640
// !! cells) thresholding `c` scored 0.33%. Threshold and rationale live in
// !! PARMain.ino -- keep this file in sync with it, do not re-derive here.
enum TcsFilter {
  TCS_RED = 0,    // S2=L,S3=L
  TCS_BLUE = 1,   // S2=L,S3=H -- populations OVERLAP, unusable
  TCS_CLEAR = 2,  // S2=H,S3=L <- the channel classifyDisc reads (slot `c`)
  TCS_GREEN = 3   // S2=H,S3=H -- separates ~as well as value 2, unused
};

// ---- Recording state --------------------------------------------------------
enum RecState { IDLE, REC_BLUE, REC_BLACK };
RecState recState = IDLE;

// Select the active photodiode filter via S2/S3.
void tcsSelect(TcsFilter f) {
  digitalWrite(TCS_S2, (f & 0x02) ? HIGH : LOW);
  digitalWrite(TCS_S3, (f & 0x01) ? HIGH : LOW);
}

// OUT is a 50%-duty square wave whose frequency tracks light intensity for the
// active filter. pulseIn times one HIGH half-period; 500000/halfUs converts that
// half-period (µs) to a full-wave frequency in Hz. 100 ms timeout keeps a dark /
// disconnected sensor from hanging.
unsigned long tcsReadFrequencyHz() {
  unsigned long halfUs = pulseIn(TCS_OUT, HIGH, 100000UL);
  if (halfUs == 0) return 0;
  return 500000UL / halfUs;
}

// Read all four channels once at the current LED/light state, 2 ms-paced.
void tcsReadRGBC(unsigned long& r, unsigned long& g,
                 unsigned long& b, unsigned long& c) {
  tcsSelect(TCS_RED);   delay(2); r = tcsReadFrequencyHz();
  tcsSelect(TCS_GREEN); delay(2); g = tcsReadFrequencyHz();
  tcsSelect(TCS_CLEAR); delay(2); c = tcsReadFrequencyHz();
  tcsSelect(TCS_BLUE);  delay(2); b = tcsReadFrequencyHz();
}

// Take one ambient-subtracted sample: AVG_CYCLES cycles of (lit - ambient),
// averaged, clamped at 0. Results returned as long (already non-negative).
void readAmbientSubtracted(long& outR, long& outG, long& outB, long& outC) {
  long accR = 0, accG = 0, accB = 0, accC = 0;

  for (int i = 0; i < AVG_CYCLES; i++) {
    unsigned long ar, ag, ab, ac;  // ambient (LEDs off)
    unsigned long lr, lg, lb, lc;  // lit     (LEDs on)

    // 1. LEDs OFF → ambient.
    digitalWrite(LED_PIN, LOW);
    delay(LED_SETTLE_MS);
    tcsReadRGBC(ar, ag, ab, ac);

    // 2. LEDs ON → lit.
    digitalWrite(LED_PIN, HIGH);
    delay(LED_SETTLE_MS);
    tcsReadRGBC(lr, lg, lb, lc);

    // 3. Accumulate (lit - ambient), signed.
    accR += (long)lr - (long)ar;
    accG += (long)lg - (long)ag;
    accB += (long)lb - (long)ab;
    accC += (long)lc - (long)ac;
  }

  // Leave LEDs OFF between samples.
  digitalWrite(LED_PIN, LOW);

  // Average and clamp negatives (noise) to 0.
  outR = accR / AVG_CYCLES; if (outR < 0) outR = 0;
  outG = accG / AVG_CYCLES; if (outG < 0) outG = 0;
  outB = accB / AVG_CYCLES; if (outB < 0) outB = 0;
  outC = accC / AVG_CYCLES; if (outC < 0) outC = 0;
}

// Print the closing tag for whatever block is open. Caller ensures a block is open.
void printCloseTag() {
  if (recState == REC_BLUE)  Serial.println(F("</blue_data>"));
  else if (recState == REC_BLACK) Serial.println(F("</black_data>"));
}

// Handle a single incoming command char. Ignores irrelevant chars.
void handleCommand(char c) {
  switch (c) {
    case 'b':
      if (recState == IDLE) {
        recState = REC_BLUE;
        Serial.println(F("<blue_data>"));
      } else {
        Serial.println(F("# already recording"));
      }
      break;
    case 'k':
      if (recState == IDLE) {
        recState = REC_BLACK;
        Serial.println(F("<black_data>"));
      } else {
        Serial.println(F("# already recording"));
      }
      break;
    case 's':
      if (recState != IDLE) {
        printCloseTag();
        recState = IDLE;
      }
      break;
    default:
      // Whitespace, newlines, and any other char are ignored.
      break;
  }
}

void setup() {
  Serial.begin(115200);

  // Sensor pins.
  pinMode(TCS_S0, OUTPUT);
  pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT);
  pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT, INPUT);

  // LED driver.
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);  // LEDs off at boot

  // S0=HIGH, S1=LOW → 20% output frequency scaling.
  digitalWrite(TCS_S0, HIGH);
  digitalWrite(TCS_S1, LOW);
  tcsSelect(TCS_CLEAR);  // idle on the channel classifyDisc reads

  Serial.println(F("# CollectColorAmbient ready — send 'b' (blue) or 'k' (black) to start, 's' to stop."));
}

void loop() {
  // Handle any pending serial commands first so start/stop is responsive.
  while (Serial.available() > 0) {
    handleCommand((char)Serial.read());
  }

  if (recState == IDLE) {
    return;  // wait for a 'b'/'k' command
  }

  // Recording: take one ambient-subtracted sample and print it as CSV.
  long r, g, b, c;
  readAmbientSubtracted(r, g, b, c);

  // Re-check for a stop char that may have arrived during the (~200 ms) sample,
  // so 's' is honored between samples without printing an extra line.
  bool stopRequested = false;
  while (Serial.available() > 0) {
    char cmd = (char)Serial.read();
    if (cmd == 's') stopRequested = true;
    else handleCommand(cmd);  // 'b'/'k' ignored while recording, others no-op
  }
  if (stopRequested) {
    printCloseTag();
    recState = IDLE;
    return;
  }

  Serial.print(r); Serial.print(',');
  Serial.print(g); Serial.print(',');
  Serial.print(b); Serial.print(',');
  Serial.println(c);
}
