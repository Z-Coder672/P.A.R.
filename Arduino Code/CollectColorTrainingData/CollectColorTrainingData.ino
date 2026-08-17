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

const int   GRID_W = 37;
const int   GRID_H = 18;

const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 412.0f;  // MUST equal GRBL $131 — homing pins the -Y switch at -$131, so this anchors the grid

// TCS3200 sensor pins (matches P.A.R.Main), plus an LED illumination bank on
// D10 (via NPN, HIGH = on) for ambient-subtracted reads.
const int TCS_S0  = D4;
const int TCS_S1  = D5;
const int TCS_S2  = D6;
const int TCS_S3  = D7;
const int TCS_OUT = D8;
const int TCS_LED = D10;

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

struct Coord { float x; float y; };
Coord grid[GRID_H][GRID_W];

// 2x2 mm square -> +/- 1.0 mm from the cell center.
const float SAMPLE_RADIUS = 1.0f;

// Sensor head sits offset from the flip actuator on the gantry. Matches
// P.A.R.Main so grid coords here represent flip-head positions and
// sensor visits land at the same physical points main scans during verify.
const float SCAN_OFFSET_X = -23.0f;
const float SCAN_OFFSET_Y =   4.0f;

// ---- Servo link (hardware UART, NOT a PWM servo output) ------------------
// D9 is the TX line of a hardware 9600-baud UART (Serial2) feeding the
// dedicated 5V ServoNano, which parses one integer µs value per line and
// drives the SG90 flip servo itself. The old Servo.h/attach(9) PWM path is
// RETIRED — writing 50 Hz PWM here would be decoded by the ServoNano as
// garbage frames and can throw the flip arm to a random angle.
// UART: the ESP32-S3 GPIO matrix routes Serial2's TX to D9, so this is a real
// UART peripheral -- GRBL's Serial1 RX ISR cannot disturb its bit timing.
const int SERVO_TX_PIN = D9;

// The flip arm is parked at REST for the entire collection sweep (this sketch
// only scans; it never flips a disc).
const int SERVO_US_REST = 565;  // ≈2°, arm parked
const int SERVO_90_DEG_SETTLE_MS = 300;

// PORT (Arduino Nano ESP32): the RP2040 bit-banged this 9600-baud frame on D9
// with interrupts disabled (servoTxByte + SERVO_TX_BIT_US, both deleted). The
// ESP32-S3 GPIO matrix routes a real UART to any pin, so the link is now
// hardware Serial2 TX on the SAME physical D9 wire -- same 9600 8N1 framing, no
// ISR blackout, no bit-period tuning. RX is unused (-1): the link is still
// one-way; the ack is the separate D2 level line.

// The link is ONE-WAY with no ack, so a dropped byte silently LOSES a command
// and the arm simply stays where it was. That broke the flip arm once: a lost
// REST left it at ENGAGE, and flipDisc then ran both X strokes with the arm
// buried in the board. A receiver-side check cannot help -- a command that never
// arrives cannot be rejected -- so every command is sent SERVO_TX_REPEATS times.
// writeMicroseconds() is idempotent, so the repeats are free: re-commanding the
// position the servo already holds does nothing. Losing a command now takes
// SERVO_TX_REPEATS independent dropouts instead of one.
//
// The repeats also fix LATE application: if only the trailing newline is lost,
// the stranded digits sit in the ServoNano's buffer until the NEXT command's
// leading newline flushes them -- which without repeats is up to a full settle
// period later, i.e. after the stroke has already started. The next repeat
// flushes them SERVO_TX_REPEAT_GAP_MS later instead.
const int SERVO_TX_REPEATS = 3;
const int SERVO_TX_REPEAT_GAP_MS = 6;

void servoTxLine(int us) {
  char buf[12];
  snprintf(buf, sizeof(buf), "\n%d\n", us);
  for (int r = 0; r < SERVO_TX_REPEATS; r++) {
    Serial2.print(buf);
    // Block until the last stop bit is actually on the wire, so this call
    // stays synchronous like the old bit-bang did -- callers time their
    // settle delay from here.
    Serial2.flush();
    if (r + 1 < SERVO_TX_REPEATS) delay(SERVO_TX_REPEAT_GAP_MS);
  }
}

void writeServoUs(int us, int settle_ms) {
  servoTxLine(us);
  delay(settle_ms);
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

// 5-frame averaged RGBC. Called twice per read by readAmbientSubtracted()
// (once LEDs-off, once LEDs-on).
void tcsReadRGBC(unsigned long& r, unsigned long& g,
                 unsigned long& b, unsigned long& c) {
  uint32_t sr = 0, sg = 0, sb = 0, sc = 0;
  for (int i = 0; i < 5; i++) {
    tcsSelect(TCS_RED);   delay(2); sr += tcsReadFrequencyHz();
    tcsSelect(TCS_GREEN); delay(2); sg += tcsReadFrequencyHz();
    tcsSelect(TCS_CLEAR); delay(2); sc += tcsReadFrequencyHz();
    tcsSelect(TCS_BLUE);  delay(2); sb += tcsReadFrequencyHz();
  }
  r = sr / 5; g = sg / 5; b = sb / 5; c = sc / 5;
}

// LED settle after toggling the illumination bank before reading.
const int LED_SETTLE_MS = 20;

// One ambient-subtracted RGBC read: LEDs-off average subtracted from LEDs-on
// average. Room light cancels; result depends only on the disc + our LEDs.
// Logged as training/tuning data so the collected RGBC matches what PARMain's
// threshold actually sees. Negatives clamped to 0. LEDs left off.
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

  long r, g, b, c;
  readAmbientSubtracted(r, g, b, c);

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
  // Serial0 owns D0/D1 by default on the Nano ESP32; hand them to Serial1 so
  // the GRBL link keeps its identifier and its physical wires.
  Serial0.end();
  Serial1.begin(115200, SERIAL_8N1, D0, D1);
  while (!Serial);

  initGrid();

  // Park the flip arm at REST BEFORE any GRBL motion — homing would otherwise
  // drag a randomly-positioned arm across the populated board.
  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);  // TX-only servo link on D9
  delay(100);
  servoTxLine(SERVO_US_REST);
  delay(SERVO_90_DEG_SETTLE_MS);

  pinMode(TCS_S0, OUTPUT);
  pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT);
  pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT, INPUT);
  pinMode(TCS_LED, OUTPUT);
  digitalWrite(TCS_LED, LOW);
  // 20% output frequency scaling (S0=H, S1=L) — keeps pulseIn within range.
  digitalWrite(TCS_S0, HIGH);
  digitalWrite(TCS_S1, LOW);
  tcsSelect(TCS_BLUE);  // idle on the channel classifyDisc reads

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
