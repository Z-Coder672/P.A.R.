// Live blue-vs-black classifier for the TCS3200 using the tiny ternary
// transformer trained by ../train.py. Same pin assignments as the project's
// other TCS3200 sketches (P.A.R.Main, ColorSensorStream).
//
// Each loop captures one R,G,B,C reading (~8 ms), pushes it into a 5-frame
// running buffer, classifies the windowed mean, and prints timing +
// prediction. The model was trained on 5-step averaged inputs, so the same
// averaging must happen at inference (otherwise accuracy drops).
//
// Inference is float32 on the RP2040's Cortex-M0+ — no FPU, but the model
// is small enough that one forward pass takes well under a millisecond.

#include "classifier.h"

const unsigned long CYCLE_US = 30000UL;  // ~33 Hz reporting
const int AVG_WINDOW = 5;                // must match train-time averaging

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

static inline void tcsSelect(TcsFilter f) {
  digitalWrite(TCS_S2, (f & 0x02) ? HIGH : LOW);
  digitalWrite(TCS_S3, (f & 0x01) ? HIGH : LOW);
}

static inline unsigned long tcsReadFrequencyHz() {
  unsigned long halfUs = pulseIn(TCS_OUT, HIGH, 20000UL);
  if (halfUs == 0) return 0;
  return 500000UL / halfUs;
}

static void readRGBC(unsigned long &r, unsigned long &g, unsigned long &b, unsigned long &c) {
  tcsSelect(TCS_RED);   delay(2); r = tcsReadFrequencyHz();
  tcsSelect(TCS_GREEN); delay(2); g = tcsReadFrequencyHz();
  tcsSelect(TCS_BLUE);  delay(2); b = tcsReadFrequencyHz();
  tcsSelect(TCS_CLEAR); delay(2); c = tcsReadFrequencyHz();
}

void setup() {
  Serial.begin(115200);
  while (!Serial);

  pinMode(TCS_S0, OUTPUT);
  pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT);
  pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT, INPUT);

  // 20% output-frequency scaling, matching ColorSensorStream/P.A.R.Main.
  digitalWrite(TCS_S0, HIGH);
  digitalWrite(TCS_S1, LOW);
  tcsSelect(TCS_CLEAR);

  Serial.println(F("ColorClassifier ready (d_model=4, d_ff=8, ternary weights)"));
  Serial.println(F("r,g,b,c   logit   label   us_inference"));
}

// Ring buffer of the last AVG_WINDOW raw RGBC frames + the running sum,
// so the windowed mean is one int divide per channel (no per-frame loop).
static uint32_t ring[AVG_WINDOW][4];
static uint32_t ringSum[4] = {0, 0, 0, 0};
static uint8_t  ringIdx = 0;
static uint8_t  ringFilled = 0;

void loop() {
  unsigned long cycleStart = micros();

  unsigned long ru, gu, bu, cu;
  readRGBC(ru, gu, bu, cu);

  // Slide the ring forward: subtract the slot we're about to overwrite,
  // write the new sample, add it to the running sum.
  if (ringFilled == AVG_WINDOW) {
    ringSum[0] -= ring[ringIdx][0];
    ringSum[1] -= ring[ringIdx][1];
    ringSum[2] -= ring[ringIdx][2];
    ringSum[3] -= ring[ringIdx][3];
  } else {
    ringFilled++;
  }
  ring[ringIdx][0] = ru;  ringSum[0] += ru;
  ring[ringIdx][1] = gu;  ringSum[1] += gu;
  ring[ringIdx][2] = bu;  ringSum[2] += bu;
  ring[ringIdx][3] = cu;  ringSum[3] += cu;
  ringIdx = (ringIdx + 1) % AVG_WINDOW;

  // Skip classification until the buffer is primed.
  if (ringFilled < AVG_WINDOW) {
    unsigned long el = micros() - cycleStart;
    if (el < CYCLE_US) delayMicroseconds(CYCLE_US - el);
    return;
  }

  float rgbc[4] = {
    (float)ringSum[0] / (float)AVG_WINDOW,
    (float)ringSum[1] / (float)AVG_WINDOW,
    (float)ringSum[2] / (float)AVG_WINDOW,
    (float)ringSum[3] / (float)AVG_WINDOW,
  };

  unsigned long t0 = micros();
  float logit = classifier_logit(rgbc);
  unsigned long t1 = micros();

  bool blue = logit > 0.0f;

  Serial.print(rgbc[0], 0); Serial.print(',');
  Serial.print(rgbc[1], 0); Serial.print(',');
  Serial.print(rgbc[2], 0); Serial.print(',');
  Serial.print(rgbc[3], 0); Serial.print('\t');
  Serial.print(logit, 4);   Serial.print('\t');
  Serial.print(blue ? F("BLUE ") : F("BLACK")); Serial.print('\t');
  Serial.println(t1 - t0);

  unsigned long elapsed = micros() - cycleStart;
  if (elapsed < CYCLE_US) delayMicroseconds(CYCLE_US - elapsed);
}
