// Live cyan-vs-black disc classifier for the TCS3200 using LED ambient
// subtraction + a simple BLUE-channel threshold — the same read the P.A.R.
// scan now uses (see ../../Arduino Code/PARMain/PARMain.ino). The old tiny
// ternary transformer model is gone: with the LED illumination bank the two
// faces separate by ~10x on the BLUE channel, so a fixed cut is all that's
// needed. classifier.h / model_weights.h remain on disk but are unreferenced.
//
// Each loop does one ambient-subtracted read: a 5-frame RGBC average with the
// LEDs OFF (ambient) subtracted from a 5-frame average with the LEDs ON (lit),
// so room light cancels and the reading depends only on the disc + our LEDs.
// It thresholds the BLUE channel and prints timing + the FRONT/displayed color.

const unsigned long CYCLE_US = 30000UL;  // ~33 Hz reporting

const int TCS_S0  = 4;
const int TCS_S1  = 5;
const int TCS_S2  = 6;
const int TCS_S3  = 7;
const int TCS_OUT = 8;
const int TCS_LED = 10;  // illumination bank (NPN base); HIGH = on

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

static inline void tcsSelect(TcsFilter f) {
  digitalWrite(TCS_S2, (f & 0x02) ? HIGH : LOW);
  digitalWrite(TCS_S3, (f & 0x01) ? HIGH : LOW);
}

static inline unsigned long tcsReadFrequencyHz() {
  unsigned long halfUs = pulseIn(TCS_OUT, HIGH, 100000UL);
  if (halfUs == 0) return 0;
  return 500000UL / halfUs;
}

// Average 5 consecutive RGBC frames (2ms-paced) at the current head position and
// current illumination. Called twice per read by readAmbientSubtracted() — once
// LEDs-off, once LEDs-on.
void tcsReadRGBC(unsigned long& r, unsigned long& g,
                 unsigned long& b, unsigned long& c) {
  uint32_t sr = 0, sg = 0, sb = 0, sc = 0;
  for (int i = 0; i < 5; i++) {
    tcsSelect(TCS_RED);   delay(2); sr += tcsReadFrequencyHz();
    tcsSelect(TCS_GREEN); delay(2); sg += tcsReadFrequencyHz();
    tcsSelect(TCS_CLEAR); delay(2); sc += tcsReadFrequencyHz();
    tcsSelect(TCS_BLUE);  delay(2); sb += tcsReadFrequencyHz();
  }
  r = sr / 5;
  g = sg / 5;
  b = sb / 5;
  c = sc / 5;
}

// LED settle after toggling the illumination bank before reading.
const int LED_SETTLE_MS = 20;

// One ambient-subtracted RGBC read: LEDs-off average subtracted from LEDs-on
// average. Room light shows up in both and cancels. Negatives clamped to 0.
// Verbatim from PARMain.ino. LEDs left off after.
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

// Disc classification by a SIMPLE THRESHOLD on the ambient-subtracted BLUE
// channel. The result is the FRONT/displayed color (1 = cyan/on, 0 = black/off).
// The sensor views the BACK of each disc: an ON disc (cyan front) shows its BLACK
// back and reads LOW blue; an OFF disc (black front) shows its cyan back and
// reads HIGH blue. So on = BLUE below the threshold. Value sits in the wide gap
// between the on-cluster (~1.8k) and off-cluster (~16k).
// Thresholds the BLUE channel (S2/S3 are crossed -- see the enum). RETIRED
// sketch: the live rig uses SCAN_ON_BLUE_MAX = 3535, not 6000.
const long SCAN_ON_BLUE_MAX = 6000;   // ambient-sub BLUE < this => cyan/on (front)
static inline uint8_t classifyDisc(long b) { return (b < SCAN_ON_BLUE_MAX) ? 1 : 0; }

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

  // 20% output-frequency scaling, matching ColorSensorStream/PARMain.
  digitalWrite(TCS_S0, HIGH);
  digitalWrite(TCS_S1, LOW);
  tcsSelect(TCS_BLUE);  // idle on the channel classifyDisc reads

  Serial.println(F("ColorClassifier ready (LED ambient-subtract + BLUE-channel threshold)"));
  Serial.println(F("r,g,b,c   label   us_read"));
}

void loop() {
  unsigned long cycleStart = micros();

  unsigned long t0 = micros();
  long r, g, b, c;
  readAmbientSubtracted(r, g, b, c);
  unsigned long t1 = micros();

  bool cyan = classifyDisc(b);  // FRONT/displayed color

  Serial.print(r); Serial.print(',');
  Serial.print(g); Serial.print(',');
  Serial.print(b); Serial.print(',');
  Serial.print(c); Serial.print('\t');
  Serial.print(cyan ? F("CYAN ") : F("BLACK")); Serial.print('\t');
  Serial.println(t1 - t0);

  unsigned long elapsed = micros() - cycleStart;
  if (elapsed < CYCLE_US) delayMicroseconds(CYCLE_US - elapsed);
}
