// VL53L4CDTest — running-averaged distance readout for the ST VL53L4CD ToF sensor.
//
// Logs the mean of the last 5 range samples, in millimeters, over Serial.
// Sensor talks I2C (SDA/SCL are fixed to the Nano's A4/A5 — not configurable here).
// Only XSHUT and the GPIO1 interrupt line are optional wired pins; left as placeholders.
//
// Library: Pololu VL53L4CD  (Arduino Library Manager -> "VL53L4CD")

#include <Wire.h>
#include <VL53L4CD.h>

// ---- Pin placeholders (fill in your wiring) --------------------------------
// XSHUT: pull LOW to hold the sensor in reset; drive HIGH (or leave unwired /
//        tied to VDD) to enable. Set to -1 if you didn't wire it.
#define XSHUT_PIN   -1   // TODO: set to the Nano pin wired to XSHUT (or -1)
// GPIO1 interrupt: not used in this polling-based test. Set to -1 if unused.
#define GPIO1_PIN   -1   // TODO: set to the Nano pin wired to GPIO1 (or -1)
// ----------------------------------------------------------------------------

// Running-average window.
const uint8_t WINDOW = 80;

// Tunable calibration offset (mm), added to every raw reading before averaging.
const int16_t DISTANCE_OFFSET_MM = -13;

VL53L4CD sensor;

uint16_t samples[WINDOW];
uint8_t  sampleCount = 0;   // number of valid samples collected (caps at WINDOW)
uint8_t  sampleIndex = 0;   // next slot to overwrite (ring buffer)

void setup()
{
  Serial.begin(115200);
  while (!Serial) { /* wait for USB serial on the Nano */ }

  // If XSHUT is wired, bring the sensor out of reset before I2C init.
  if (XSHUT_PIN >= 0) {
    pinMode(XSHUT_PIN, OUTPUT);
    digitalWrite(XSHUT_PIN, LOW);
    delay(10);
    digitalWrite(XSHUT_PIN, HIGH);
    delay(10);
  }

  Wire.begin();
  Wire.setClock(400000);  // 400 kHz fast mode

  sensor.setTimeout(500);
  if (!sensor.init()) {
    Serial.println("VL53L4CD init failed — check wiring / I2C address.");
    while (1) { delay(1000); }
  }

  // Timing budget in milliseconds; longer = lower per-sample noise (max ~200).
  sensor.setRangeTiming(50, 0);   // 50 ms measurement, back-to-back
  sensor.startContinuous();

  Serial.println("VL53L4CD ready. Logging 5-sample running average (mm).");
}

void loop()
{
  uint16_t raw = sensor.read();   // blocking read, distance in mm

  if (sensor.timeoutOccurred()) {
    Serial.println("timeout");
    return;
  }

  // Apply calibration offset, clamping to 0 so it can't go negative.
  int32_t corrected = (int32_t)raw + DISTANCE_OFFSET_MM;
  if (corrected < 0) corrected = 0;
  uint16_t d = (uint16_t)corrected;

  // Push into the ring buffer.
  samples[sampleIndex] = d;
  sampleIndex = (sampleIndex + 1) % WINDOW;
  if (sampleCount < WINDOW) sampleCount++;

  // Trimmed mean: sort the collected samples, drop the top and bottom 20%,
  // and average what's left. Rejects the occasional outlier (like a median)
  // while still using most of the data (like a mean) — steadier than either.
  uint16_t sorted[WINDOW];
  for (uint8_t i = 0; i < sampleCount; i++) sorted[i] = samples[i];
  for (uint8_t i = 1; i < sampleCount; i++) {   // insertion sort
    uint16_t v = sorted[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
    sorted[j + 1] = v;
  }
  uint8_t trim = sampleCount / 5;               // 20% off each end
  uint8_t lo = trim;
  uint8_t hi = sampleCount - trim;              // exclusive
  if (hi <= lo) { lo = 0; hi = sampleCount; }   // too few samples to trim yet
  uint8_t n = hi - lo;
  uint32_t sum = 0;
  for (uint8_t i = lo; i < hi; i++) sum += sorted[i];
  uint16_t avg = (sum + n / 2) / n;

  Serial.print("raw=");
  Serial.print(raw);
  Serial.print(" mm   corr=");
  Serial.print(d);
  Serial.print(" mm   trimmed-avg(");
  Serial.print(n);
  Serial.print("/");
  Serial.print(sampleCount);
  Serial.print(")=");
  Serial.print(avg);
  Serial.print(" mm   window[min=");
  Serial.print(sorted[0]);
  Serial.print(" max=");
  Serial.print(sorted[sampleCount - 1]);
  Serial.print(" spread=");
  Serial.print(sorted[sampleCount - 1] - sorted[0]);
  Serial.println("]");
}
