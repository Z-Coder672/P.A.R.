// TCS3200 sanity check: LED ambient-subtracted RGBC read + a simple
// clear-channel threshold (no ML model). Prints the ambient-subtracted RGBC and
// the resulting front-face guess (cyan/black) roughly twice a second.
//
// Read model (matches PARMain): a 5-frame averaged RGBC with the illumination
// LEDs OFF (ambient) is subtracted from a 5-frame average with the LEDs ON.
// Room light cancels, so the result depends only on the disc + our own LEDs.
//
// Polarity: the sensor views the BACK of each disc. An ON disc (cyan on the
// FRONT) shows its BLACK back to the sensor -> LOW clear channel; an OFF disc
// (black front) shows its cyan back -> HIGH clear. So front cyan/on = clear
// below the threshold. gridState/the "on" result is the FRONT/displayed color.

const int TCS_S0  = D4;
const int TCS_S1  = D5;
const int TCS_S2  = D6;
const int TCS_S3  = D7;
const int TCS_OUT = D8;
const int TCS_LED = D10;  // illumination bank (NPN base); HIGH = on

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
  unsigned long halfUs = pulseIn(TCS_OUT, HIGH, 100000UL);
  if (halfUs == 0) return 0;
  return 500000UL / halfUs;
}

// 5-frame averaged RGBC at whatever the current illumination is. Used twice per
// read by readAmbientSubtracted() — once LEDs-off, once LEDs-on.
void tcsReadRGBC(unsigned long& r, unsigned long& g,
                 unsigned long& b, unsigned long& c) {
  uint32_t sr = 0, sg = 0, sb = 0, sc = 0;
  for (int i = 0; i < 5; i++) {
    tcsSelect(TCS_RED);   delay(2); sr += tcsReadFrequencyHz();
    tcsSelect(TCS_GREEN); delay(2); sg += tcsReadFrequencyHz();
    tcsSelect(TCS_BLUE);  delay(2); sb += tcsReadFrequencyHz();
    tcsSelect(TCS_CLEAR); delay(2); sc += tcsReadFrequencyHz();
  }
  r = sr / 5; g = sg / 5; b = sb / 5; c = sc / 5;
}

// LED settle after toggling the illumination bank before reading.
const int LED_SETTLE_MS = 20;

// One ambient-subtracted RGBC read: LEDs-off average subtracted from LEDs-on
// average. Negatives clamped to 0. LEDs left off.
void readAmbientSubtracted(long& r, long& g, long& b, long& c) {
  unsigned long ar, ag, ab, ac, lr, lg, lb, lc;
  digitalWrite(TCS_LED, LOW);  delay(LED_SETTLE_MS); tcsReadRGBC(ar, ag, ab, ac);
  digitalWrite(TCS_LED, HIGH); delay(LED_SETTLE_MS); tcsReadRGBC(lr, lg, lb, lc);
  digitalWrite(TCS_LED, LOW);
  r = (long)lr - (long)ar; if (r < 0) r = 0;
  g = (long)lg - (long)ag; if (g < 0) g = 0;
  b = (long)lb - (long)ab; if (b < 0) b = 0;
  c = (long)lc - (long)ac; if (c < 0) c = 0;
}

// Simple threshold on the ambient-subtracted clear channel. 1 = cyan/on
// (front), 0 = black/off. See polarity note at the top.
// Classification threshold. Physically the BLUE channel, not clear — the S2/S3
// select lines are crossed on this rig, so tcsReadRGBC's `c` output holds blue.
// That is deliberate (blue separates the disc faces 10.21x vs clear's 2.22x).
// Full explanation and the measurements are in PARMain.ino at this constant.
// 3535 = geometric mean of the measured populations; was 6000, which was
// lopsided (3.69x / 1.28x) toward the failure side.
const long SCAN_ON_BLUE_MAX = 3535;
static inline uint8_t classifyDisc(long c) { return (c < SCAN_ON_BLUE_MAX) ? 1 : 0; }

void setup() {
  Serial.begin(115200);
  while (!Serial);

  pinMode(TCS_S0, OUTPUT);
  pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT);
  pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT, INPUT);
  pinMode(TCS_LED, OUTPUT);
  digitalWrite(TCS_LED, LOW);

  // 20% output frequency scaling.
  digitalWrite(TCS_S0, HIGH);
  digitalWrite(TCS_S1, LOW);
  tcsSelect(TCS_CLEAR);

  Serial.println("TCS3200 ambient-subtracted test ready.");
  Serial.println("R\tG\tB\tC\tguess");
}

void loop() {
  long r, g, b, c;
  readAmbientSubtracted(r, g, b, c);
  const char* guess = classifyDisc(c) ? "cyan" : "black";

  Serial.print(r); Serial.print('\t');
  Serial.print(g); Serial.print('\t');
  Serial.print(b); Serial.print('\t');
  Serial.print(c); Serial.print('\t');
  Serial.println(guess);

  delay(500);
}
