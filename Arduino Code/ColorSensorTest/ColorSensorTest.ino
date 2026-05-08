// TCS3200 sanity check: sample R/G/B/C twice a second and dump to serial.
// Prints raw frequencies plus a blue-vs-black guess from the same tiny
// ternary transformer used by P.A.R.Main.

#include "classifier.h"

const int AVG_WINDOW = 5;  // must match train-time averaging window

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
  const int N = 5;
  unsigned long sum = 0;
  int valid = 0;
  for (int i = 0; i < N; i++) {
    unsigned long halfUs = pulseIn(TCS_OUT, HIGH, 100000UL);
    if (halfUs == 0) continue;
    sum += 500000UL / halfUs;
    valid++;
  }
  if (valid == 0) return 0;
  return sum / valid;
}

void setup() {
  Serial.begin(115200);
  while (!Serial);

  pinMode(TCS_S0, OUTPUT);
  pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT);
  pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT, INPUT);

  // 20% output frequency scaling.
  digitalWrite(TCS_S0, HIGH);
  digitalWrite(TCS_S1, LOW);
  tcsSelect(TCS_CLEAR);

  Serial.println("TCS3200 test ready.");
  Serial.println("R\tG\tB\tC\tlogit\tguess\tus");
}

// Ring buffer of the last AVG_WINDOW raw RGBC frames + running sum, so the
// windowed mean is a single divide per channel.
static uint32_t ring[AVG_WINDOW][4];
static uint32_t ringSum[4] = {0, 0, 0, 0};
static uint8_t  ringIdx = 0;
static uint8_t  ringFilled = 0;

void loop() {
  unsigned long r, g, b, c;
  tcsSelect(TCS_RED);   delay(2); r = tcsReadFrequencyHz();
  tcsSelect(TCS_GREEN); delay(2); g = tcsReadFrequencyHz();
  tcsSelect(TCS_BLUE);  delay(2); b = tcsReadFrequencyHz();
  tcsSelect(TCS_CLEAR); delay(2); c = tcsReadFrequencyHz();

  if (ringFilled == AVG_WINDOW) {
    ringSum[0] -= ring[ringIdx][0];
    ringSum[1] -= ring[ringIdx][1];
    ringSum[2] -= ring[ringIdx][2];
    ringSum[3] -= ring[ringIdx][3];
  } else {
    ringFilled++;
  }
  ring[ringIdx][0] = r;  ringSum[0] += r;
  ring[ringIdx][1] = g;  ringSum[1] += g;
  ring[ringIdx][2] = b;  ringSum[2] += b;
  ring[ringIdx][3] = c;  ringSum[3] += c;
  ringIdx = (ringIdx + 1) % AVG_WINDOW;

  if (ringFilled < AVG_WINDOW) {
    delay(1);
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
  unsigned long us = micros() - t0;
  const char* guess = (logit > 3.0f) ? "blue" : "black";

  Serial.print((unsigned long)rgbc[0]); Serial.print("\t");
  Serial.print((unsigned long)rgbc[1]); Serial.print("\t");
  Serial.print((unsigned long)rgbc[2]); Serial.print("\t");
  Serial.print((unsigned long)rgbc[3]); Serial.print("\t");
  Serial.print(logit, 3); Serial.print("\t");
  Serial.print(guess); Serial.print("\t");
  Serial.println(us);

  delay(1);
}
