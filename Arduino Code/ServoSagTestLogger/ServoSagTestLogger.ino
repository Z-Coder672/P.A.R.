#define SENSE_PIN A0
#define TRIGGER_PIN 10
#define SAMPLES 600

int readings[SAMPLES];

void setup() {
  Serial.begin(115200);
  pinMode(TRIGGER_PIN, INPUT);

  Serial.println("waiting for trigger on D10...");
  while (digitalRead(TRIGGER_PIN) == LOW);

  for (int i = 0; i < SAMPLES; i++) {
    readings[i] = analogRead(SENSE_PIN);
    delayMicroseconds(500);
  }

  Serial.println("capture done — send 'p' to print results");
  { char c = 0; while (c != 'p') { if (Serial.available()) c = Serial.read(); } }

  int minVal = readings[0];
  int maxVal = readings[0];
  long sum = 0;
  int minIdx = 0;

  for (int i = 0; i < SAMPLES; i++) {
    if (readings[i] < minVal) { minVal = readings[i]; minIdx = i; }
    if (readings[i] > maxVal) maxVal = readings[i];
    sum += readings[i];
  }

  int avgVal = sum / SAMPLES;
  #define TVOLTS(x) ((x) * 9.8 / 1023.0)

  Serial.println("=== results ===");
  Serial.print("avg:  "); Serial.print(TVOLTS(avgVal), 3); Serial.println(" V");
  Serial.print("max:  "); Serial.print(TVOLTS(maxVal), 3); Serial.println(" V");
  Serial.print("min:  "); Serial.print(TVOLTS(minVal), 3); Serial.println(" V");
  Serial.print("drop: "); Serial.print(TVOLTS(maxVal - minVal), 3); Serial.println(" V");
  Serial.print("min at sample: "); Serial.print(minIdx);
  Serial.print("  ("); Serial.print(minIdx * 0.5); Serial.println(" ms)");

  int threshold = avgVal - 10;
  Serial.println("=== anomalous samples (>100mV below avg) ===");
  bool any = false;
  for (int i = 0; i < SAMPLES; i++) {
    if (readings[i] < threshold) {
      Serial.print("  t="); Serial.print(i * 0.5);
      Serial.print("ms  "); Serial.print(TVOLTS(readings[i]), 3); Serial.println("V");
      any = true;
    }
  }
  if (!any) Serial.println("  none — rail is stable");
}

void loop() {}