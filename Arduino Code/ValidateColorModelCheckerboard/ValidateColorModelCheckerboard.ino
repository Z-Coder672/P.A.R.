// Walks 6 rows of the known-pattern board (top 2, middle 2, bottom 2),
// samples the center of each cell with the TCS3200, runs the trained
// classifier, and prints the resulting blue/black accuracy.
//
// Uses the same homing/grid/streaming setup as CollectColorTrainingData,
// but only the row centers and only one sample per cell — this is a quick
// model sanity check, not a training pass.
//
// Assumed board pattern: full checkerboard — black where (x+y) is even,
// blue where (x+y) is odd. Matches CollectColorTrainingData.
//
// Drop a trained model_weights.h into this sketch folder before flashing.

#include "classifier.h"

const int   GRID_W = 37;
const int   GRID_H = 18;
const int   AVG_WINDOW = 5;

const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 399.695f;

// TCS3200 sensor pins (matches P.A.R.Main).
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

struct Coord { float x; float y; };
Coord grid[GRID_H][GRID_W];

// Top 2, middle 2, bottom 2 rows of an 18-row board.
const int VALIDATION_ROWS[] = {0, 1, 8, 9, 16, 17};
const int N_VALIDATION_ROWS = sizeof(VALIDATION_ROWS) / sizeof(VALIDATION_ROWS[0]);

// Ring buffer for the 5-frame running average fed to the classifier.
static uint32_t ring[AVG_WINDOW][4];
static uint32_t ringSum[4] = {0, 0, 0, 0};
static uint8_t  ringIdx    = 0;
static uint8_t  ringFilled = 0;

void resetRing() {
  memset(ring, 0, sizeof(ring));
  memset(ringSum, 0, sizeof(ringSum));
  ringIdx    = 0;
  ringFilled = 0;
}

// GRBL character-counting streaming protocol (same setup as P.A.R.Main).
#define RX_BUFFER_SAFE 120
#define QUEUE_SIZE 32
int cmdLengths[QUEUE_SIZE];
int qHead = 0;
int qTail = 0;
int bufferFill = 0;

void enqueue(int len) { cmdLengths[qTail] = len; qTail = (qTail + 1) % QUEUE_SIZE; }
int  dequeue()        { int len = cmdLengths[qHead]; qHead = (qHead + 1) % QUEUE_SIZE; return len; }

void drainResponses() {
  while (Serial1.available()) {
    String resp = Serial1.readStringUntil('\n');
    resp.trim();
    if (resp.length() == 0) continue;
    Serial.print("# GRBL: "); Serial.println(resp);
    if (resp == "ok") {
      if (qHead != qTail) bufferFill -= dequeue();
    } else if (resp.startsWith("error") || resp.startsWith("ALARM")) {
      Serial.print("# GRBL halted: "); Serial.println(resp);
      while (true);
    }
  }
}

void sendGcode(const char* cmd) {
  int cmdLen = strlen(cmd) + 1;
  while (bufferFill + cmdLen > RX_BUFFER_SAFE) drainResponses();
  Serial1.print(cmd);
  Serial1.write('\n');
  bufferFill += cmdLen;
  enqueue(cmdLen);
  Serial.print("# > ["); Serial.print(bufferFill); Serial.print("] ");
  Serial.println(cmd);
}

void waitForIdle() { while (bufferFill > 0) drainResponses(); }
void waitForMotion() { sendGcode("G4 P0"); waitForIdle(); }

void moveTo(float x, float y) {
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "G0 X%.2f Y%.2f", x, y);
  sendGcode(cmd);
}

void initGrid() {
  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      // Starting flip-head offset is 25 mm; the inline -23 mm bakes in
      // SCAN_OFFSET_X to land the sensor over the cell centre.
      grid[y][x].x = -X_TRAVEL + 25.0f - 23.0f + 20.045f * x;
      grid[y][x].y = -Y_TRAVEL + 0.0f + 4.0f + 23.40f * ((GRID_H - 1) - y);
    }
  }
}

void tcsSelect(TcsFilter f) {
  digitalWrite(TCS_S2, (f & 0x02) ? HIGH : LOW);
  digitalWrite(TCS_S3, (f & 0x01) ? HIGH : LOW);
}

unsigned long tcsReadFrequencyHz() {
  unsigned long halfUs = pulseIn(TCS_OUT, HIGH, 100000UL);
  if (halfUs == 0) return 0;
  return 500000UL / halfUs;
}

void tcsReadRGBC(unsigned long& r, unsigned long& g,
                 unsigned long& b, unsigned long& c) {
  resetRing();
  for (int i = 0; i < AVG_WINDOW; i++) {
    unsigned long fr, fg, fb, fc;
    tcsSelect(TCS_RED);   delay(2); fr = tcsReadFrequencyHz();
    tcsSelect(TCS_GREEN); delay(2); fg = tcsReadFrequencyHz();
    tcsSelect(TCS_BLUE);  delay(2); fb = tcsReadFrequencyHz();
    tcsSelect(TCS_CLEAR); delay(2); fc = tcsReadFrequencyHz();

    if (ringFilled == AVG_WINDOW) {
      ringSum[0] -= ring[ringIdx][0];
      ringSum[1] -= ring[ringIdx][1];
      ringSum[2] -= ring[ringIdx][2];
      ringSum[3] -= ring[ringIdx][3];
    } else {
      ringFilled++;
    }
    ring[ringIdx][0] = fr;  ringSum[0] += fr;
    ring[ringIdx][1] = fg;  ringSum[1] += fg;
    ring[ringIdx][2] = fb;  ringSum[2] += fb;
    ring[ringIdx][3] = fc;  ringSum[3] += fc;
    ringIdx = (ringIdx + 1) % AVG_WINDOW;
  }
  r = ringSum[0] / AVG_WINDOW;
  g = ringSum[1] / AVG_WINDOW;
  b = ringSum[2] / AVG_WINDOW;
  c = ringSum[3] / AVG_WINDOW;
}

uint8_t expectedColor(int x, int y) {
  return ((x + y) & 1) ? 1 : 0;  // black at (0,0), blue where (x+y) odd
}

int totalSamples = 0;
int correctSamples = 0;

void sampleAndCheck(int x, int y) {
  moveTo(grid[y][x].x, grid[y][x].y);
  waitForMotion();

  unsigned long r, g, b, c;
  tcsReadRGBC(r, g, b, c);

  float rgbc[4] = { (float)r, (float)g, (float)b, (float)c };
  bool predBlue = classifier_is_blue(rgbc);
  bool expBlue  = expectedColor(x, y) != 0;
  bool ok = (predBlue == expBlue);

  totalSamples++;
  if (ok) correctSamples++;

  Serial.print(ok ? "OK  " : "MISS ");
  Serial.print("x="); Serial.print(x);
  Serial.print(" y="); Serial.print(y);
  Serial.print(" exp="); Serial.print(expBlue ? 'b' : 'k');
  Serial.print(" pred="); Serial.print(predBlue ? 'b' : 'k');
  Serial.print(" rgbc=");
  Serial.print(r); Serial.print(',');
  Serial.print(g); Serial.print(',');
  Serial.print(b); Serial.print(',');
  Serial.println(c);
}

bool started = false;

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);
  while (!Serial);

  initGrid();

  pinMode(TCS_S0, OUTPUT);
  pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT);
  pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT, INPUT);
  digitalWrite(TCS_S0, HIGH);
  digitalWrite(TCS_S1, LOW);
  tcsSelect(TCS_CLEAR);

  delay(2000);
  while (Serial1.available()) Serial1.read();

  Serial.println("# homing");
  sendGcode("$H");
  waitForIdle();
  delay(100);

  sendGcode("G21");
  sendGcode("G90");
  waitForIdle();
  Serial.println("# homed; starting validation sweep");
}

void loop() {
  if (started) return;
  started = true;

  for (int i = 0; i < N_VALIDATION_ROWS; i++) {
    int y = VALIDATION_ROWS[i];
    for (int x = 0; x < GRID_W; x++) {
      sampleAndCheck(x, y);
    }
  }

  float pct = (totalSamples > 0)
                ? (100.0f * (float)correctSamples / (float)totalSamples)
                : 0.0f;
  Serial.println();
  Serial.print("DONE  correct=");
  Serial.print(correctSamples);
  Serial.print("/");
  Serial.print(totalSamples);
  Serial.print("  acc=");
  Serial.print(pct, 2);
  Serial.println("%");
}
