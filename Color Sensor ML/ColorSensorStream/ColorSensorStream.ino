// Streams raw TCS3200 R/G/B/C frequency readings every ~15ms:
//   ~8ms capture (4 channels x 2ms settle + ~1 pulse read)
//   ~5ms serial send budget at 115200
//   2ms loop tail delay
// Output: one CSV line per loop -> "r,g,b,c\n"
//
// On RP2040 the USB CDC Serial is essentially non-blocking, so a fixed
// micros() pacer is used to keep the cycle near 15ms regardless of
// channel frequency (which controls how fast pulseIn returns).

const unsigned long CYCLE_US = 15000UL;

const int TCS_S0  = 4;
const int TCS_S1  = 5;
const int TCS_S2  = 6;
const int TCS_S3  = 7;
const int TCS_OUT = 8;

enum TcsFilter {
  TCS_RED   = 0,
  TCS_BLUE  = 1,
  TCS_CLEAR = 2,
  TCS_GREEN = 3
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

void setup() {
  Serial.begin(115200);
  while (!Serial);

  pinMode(TCS_S0, OUTPUT);
  pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT);
  pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT, INPUT);

  // 20% output frequency scaling to match P.A.R.Main.
  digitalWrite(TCS_S0, HIGH);
  digitalWrite(TCS_S1, LOW);
  tcsSelect(TCS_CLEAR);
}

void loop() {
  unsigned long cycleStart = micros();

  unsigned long r, g, b, c;
  tcsSelect(TCS_RED);   delay(2); r = tcsReadFrequencyHz();
  tcsSelect(TCS_GREEN); delay(2); g = tcsReadFrequencyHz();
  tcsSelect(TCS_BLUE);  delay(2); b = tcsReadFrequencyHz();
  tcsSelect(TCS_CLEAR); delay(2); c = tcsReadFrequencyHz();

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
