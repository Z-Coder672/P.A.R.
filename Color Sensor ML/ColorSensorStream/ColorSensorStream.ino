// Streams LED ambient-subtracted TCS3200 R/G/B/C readings:
//   Each cycle reads all 4 channels twice — once with the illumination
//   LEDs OFF (ambient / room light) and once with them ON (lit) — and
//   streams (lit - ambient) per channel, clamped to >=0. This cancels
//   room light so the numbers reflect only the LED-lit disc color.
// Output: one CSV line per loop -> "r,g,b,c\n" (ambient-subtracted).
//
// On RP2040 the USB CDC Serial is essentially non-blocking, so a fixed
// micros() pacer is used to keep the cycle near its target cadence
// regardless of channel frequency (which controls how fast pulseIn
// returns). Note the two LED settle windows dominate the cycle now, so
// each cycle runs longer than the old 15ms single-read cadence; the
// pacer simply no-ops when the read already overshoots CYCLE_US.

const unsigned long CYCLE_US = 15000UL;
const int LED_SETTLE_MS = 20;  // settle after toggling the LEDs before reading

const int TCS_S0  = 4;
const int TCS_S1  = 5;
const int TCS_S2  = 6;
const int TCS_S3  = 7;
const int TCS_OUT = 8;
const int TCS_LED = 10;  // illumination bank via NPN: HIGH = LEDs ON

// S2/S3 select the photodiode filter bank.
//
// These labels name the filter that is PHYSICALLY selected. The S2/S3 lines are
// crossed on this rig, so the datasheet's (S2,S3) -> filter table does not hold
// for values 1 and 2 -- the value assignments below already account for that.
// Everything downstream then reads straight: `b` holds blue, `c` holds clear,
// and classifyDisc thresholds `b`.
//
// !! classifyDisc MUST THRESHOLD BLUE. Blue separates the cyan disc face from
// !! the black one by 10.22x; CLEAR manages only 2.22x, so reading CLEAR would
// !! cut usable margin from 2.17x each way to 1.24x. These two values encode the
// !! wiring, so if S2/S3 are ever rewired straight they must move with it.
enum TcsFilter {
  TCS_RED = 0,    // commanded S2=L,S3=L -> RED
  TCS_CLEAR = 1,  // commanded S2=L,S3=H -> CLEAR (datasheet says BLUE)
  TCS_BLUE = 2,   // commanded S2=H,S3=L -> BLUE  (datasheet says CLEAR)
  TCS_GREEN = 3   // commanded S2=H,S3=H -> GREEN
};

void tcsSelect(TcsFilter f) {
  digitalWrite(TCS_S2, (f & 0x02) ? HIGH : LOW);
  digitalWrite(TCS_S3, (f & 0x01) ? HIGH : LOW);
}

unsigned long tcsReadFrequencyHz() {
  // 20ms timeout caps the worst-case dark/no-signal read.
  unsigned long halfUs = pulseIn(TCS_OUT, HIGH, 20000UL);
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

void setup() {
  Serial.begin(115200);
  while (!Serial);

  pinMode(TCS_S0, OUTPUT);
  pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT);
  pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT, INPUT);

  // LED driver (illumination bank via NPN).
  pinMode(TCS_LED, OUTPUT);
  digitalWrite(TCS_LED, LOW);  // LEDs off at boot

  // 20% output frequency scaling to match P.A.R.Main.
  digitalWrite(TCS_S0, HIGH);
  digitalWrite(TCS_S1, LOW);
  tcsSelect(TCS_BLUE);  // idle on the channel classifyDisc reads
}

void loop() {
  unsigned long cycleStart = micros();

  // Ambient-subtracted read: LEDs OFF -> ambient, LEDs ON -> lit,
  // output = (lit - ambient) per channel, clamped to >=0. LEDs left off.
  unsigned long ar, ag, ab, ac;  // ambient (LEDs off)
  unsigned long lr, lg, lb, lc;  // lit     (LEDs on)

  digitalWrite(TCS_LED, LOW);
  delay(LED_SETTLE_MS);
  tcsReadRGBC(ar, ag, ab, ac);

  digitalWrite(TCS_LED, HIGH);
  delay(LED_SETTLE_MS);
  tcsReadRGBC(lr, lg, lb, lc);

  digitalWrite(TCS_LED, LOW);  // leave LEDs off between cycles

  long r = (long)lr - (long)ar; if (r < 0) r = 0;
  long g = (long)lg - (long)ag; if (g < 0) g = 0;
  long b = (long)lb - (long)ab; if (b < 0) b = 0;
  long c = (long)lc - (long)ac; if (c < 0) c = 0;

  Serial.print(r); Serial.print(',');
  Serial.print(g); Serial.print(',');
  Serial.print(b); Serial.print(',');
  Serial.println(c);

  // Hold the loop to a steady CYCLE_US cadence. Skip pacing if we
  // already overshot (e.g. a slow pulseIn timeout).
  unsigned long elapsed = micros() - cycleStart;
  if (elapsed < CYCLE_US) {
    delayMicroseconds(CYCLE_US - elapsed);
  }
}
