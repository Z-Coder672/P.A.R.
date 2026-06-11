// Walks a known-pattern board, sampling RGBC at 5 positions per cell
// (center + 4 corners of a 2x2 mm square) and printing labeled lines for
// the python collector at `Color Sensor ML/collect_auto.py`.
//
// Each printed line:   <label>,<r>,<g>,<b>,<c>
//   label = 'b' (blue) or 'k' (black), determined by the expected pattern.
//
// Expected board pattern (37x18, x=0 left, y=0 top): full checkerboard.
// New x=0 maps to the original col-1 disc (the leftmost column of the
// 38-wide board is intentionally not addressed), so on the back-side
// checkerboard the new (0,0) cell reads BLACK.
//
// Set the discs to that pattern using the reversed PNG shipped alongside
// (the camera/sensor sees the back, so the PNG is mirrored).

#include <Servo.h>

const int   GRID_W = 37;
const int   GRID_H = 18;

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

// 2x2 mm square -> +/- 1.0 mm from the cell center.
const float SAMPLE_RADIUS = 1.0f;

// Sensor head sits offset from the flip actuator on the gantry. Matches
// P.A.R.Main so grid coords here represent flip-head positions and
// sensor visits land at the same physical points main scans during verify.
const float SCAN_OFFSET_X = -23.0f;
const float SCAN_OFFSET_Y =   4.0f;

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
      // grbl-Mega can emit a duplicate `ok` after $H (one when alarm clears,
      // one when homing completes). Without this guard the spurious ack
      // dequeues a stale slot, desyncing bufferFill so waitForIdle hangs.
      if (qHead != qTail) bufferFill -= dequeue();
    } else if (resp.startsWith("error") || resp.startsWith("ALARM")) {
      // ALARM:N is async (not tied to a queued command, so it never produces
      // the `ok` waitForIdle is waiting on) — without halting on it, a failed
      // homing cycle silently spins waitForIdle forever.
      Serial.print("# GRBL halted: "); Serial.println(resp);
      while (true);
    }
  }
}

void sendGcode(const char* cmd) {
  int cmdLen = strlen(cmd) + 1;
  while (bufferFill + cmdLen > RX_BUFFER_SAFE) drainResponses();
  // Send `\n` only, not `\r\n` — grbl-Mega treats `\r` as a line end then
  // acks the trailing `\n` as an empty line, producing a duplicate ok per
  // command. The duplicate desyncs cmdLengths queue accounting.
  Serial1.print(cmd);
  Serial1.write('\n');
  bufferFill += cmdLen;
  enqueue(cmdLen);
  Serial.print("# > ["); Serial.print(bufferFill); Serial.print("] ");
  Serial.println(cmd);
}

void waitForIdle() { while (bufferFill > 0) drainResponses(); }

// G4 P0 forces a planner sync, so GRBL's `ok` lands only once motion has
// actually finished (not just been parsed). See CLAUDE.md GRBL gotcha.
void waitForMotion() { sendGcode("G4 P0"); waitForIdle(); }

void moveTo(float x, float y) {
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "G0 X%.2f Y%.2f", x, y);
  sendGcode(cmd);
}

void initGrid() {
  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      //           starting offset⌄
      // 25 mm starting X offset, matching P.A.R.Main.
      grid[y][x].x = -X_TRAVEL + 25.0f + 20.045f * x;
      grid[y][x].y = -Y_TRAVEL +  0.0f + 23.40f * ((GRID_H - 1) - y);
      //                                  ⌃grid spacing
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

// 5-frame averaged RGBC, matches the input distribution P.A.R.Main feeds
// the classifier.
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

// Returns 1 if the cell at (x,y) is expected to be blue, 0 if black.
// Sensor sees the BACK of each disc (opposite side from front). Full
// checkerboard. Since new x=0 maps to original col 1 (one step into the
// physical pattern), the parity is flipped relative to the old 38-wide
// grid: back-(0,0) is black, blue where (x+y) is odd.
uint8_t expectedColor(int x, int y) {
  return ((x + y) & 1) ? 1 : 0;  // black at (0,0), blue where (x+y) odd
}

void sampleAndPrint(int x, int y, float dx, float dy) {
  moveTo(grid[y][x].x + SCAN_OFFSET_X + dx,
         grid[y][x].y + SCAN_OFFSET_Y + dy);
  waitForMotion();

  unsigned long r, g, b, c;
  tcsReadRGBC(r, g, b, c);

  char prefix = expectedColor(x, y) ? 'b' : 'k';
  Serial.print(prefix); Serial.print(',');
  Serial.print(r);      Serial.print(',');
  Serial.print(g);      Serial.print(',');
  Serial.print(b);      Serial.print(',');
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
  // 20% output frequency scaling (S0=H, S1=L) — keeps pulseIn within range.
  digitalWrite(TCS_S0, HIGH);
  digitalWrite(TCS_S1, LOW);
  tcsSelect(TCS_CLEAR);

  delay(2000); // GRBL boot
  while (Serial1.available()) Serial1.read();

  // Home first — GRBL boots into Alarm state when $22=1 and rejects any
  // G-code (G21/G90 included) with error:9 until $H or $X clears the alarm.
  // Send $X afterwards to defensively kill any residual alarm flag (some
  // grbl-Mega builds still report locked-out right after $H acks ok).
  Serial.println("# homing");
  sendGcode("$H");
  waitForIdle();
  delay(100);

  sendGcode("G21");
  sendGcode("G90");
  waitForIdle();
  Serial.println("# homed; starting sweep");
}

void loop() {
  if (started) return;
  started = true;

  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      sampleAndPrint(x, y,            0.0f,            0.0f);
      sampleAndPrint(x, y, -SAMPLE_RADIUS, -SAMPLE_RADIUS);
      sampleAndPrint(x, y,  SAMPLE_RADIUS, -SAMPLE_RADIUS);
      sampleAndPrint(x, y, -SAMPLE_RADIUS,  SAMPLE_RADIUS);
      sampleAndPrint(x, y,  SAMPLE_RADIUS,  SAMPLE_RADIUS);
    }
  }

  Serial.println("DONE");
}
