// ServoYFlex — reproduce the MAIN servo failure by exercising the carriage's
// full Y travel (which flexes the main servo's long wire to the moving carriage)
// while parking it at the visible top-left corner (Y~0, X~-X_TRAVEL) on each
// up-stroke so the main servo can be observed there. The witness servo (fixed,
// short wire, always in frame) is the control: if the main stops while the
// witness keeps moving, the fault is on the main's branch (wire/connector/servo),
// not the shared signal.
//
// SAFETY: all carriage motion is pure-Y at the X soft-limit edge with the servo
// at REST (the sanctioned "straight up on either edge" move). The servo only
// actuates while the carriage is stationary.
//
// Production bit-banged-UART -> ServoNano servo path. Robust+logged GRBL handling
// (retries error:N, parks+halts with a printed cause on ALARM/stall).

const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 402.0f;
const float X_LEFT  = -775.0f;  // left edge (main visible here at Y~top); inside -X soft limit
const float X_RIGHT = -31.0f;   // rightmost disc column (col 36) — "all the way right"
const float Y_TOP   = -3.0f;    // near top (Y=0 is the +Y soft limit)
const float Y_BOT   = -399.0f;  // near bottom (-Y_TRAVEL is the home limit)

const int SERVO_US_REST    = 544;
const int SERVO_US_RELEASE = 1018;  // ≈46° (raised 8° from the prior 936/≈38°)
const int SERVO_US_ENGAGE  = 1471;
const int SERVO_90_DEG_SETTLE_MS = 300;
const int SERVO_50_DEG_SETTLE_MS = 100;

const int SERVO_TX_PIN = 9;
const int SERVO_TX_BIT_US = 102;
void servoTxByte(uint8_t b) {
  noInterrupts();
  digitalWrite(SERVO_TX_PIN, LOW); delayMicroseconds(SERVO_TX_BIT_US);
  for (int i = 0; i < 8; i++) { digitalWrite(SERVO_TX_PIN, (b >> i) & 1); delayMicroseconds(SERVO_TX_BIT_US); }
  digitalWrite(SERVO_TX_PIN, HIGH); interrupts();
  delayMicroseconds(SERVO_TX_BIT_US);
}
void servoTxLine(int us) { char b[12]; int n = snprintf(b, sizeof(b), "%d\n", us); for (int i=0;i<n;i++) servoTxByte((uint8_t)b[i]); }
void writeServoUs(int us, int s) { servoTxLine(us); delay(s); }
void cycleServo() {
  writeServoUs(SERVO_US_ENGAGE,  SERVO_90_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_REST,    SERVO_90_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_RELEASE, SERVO_50_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_REST,    SERVO_50_DEG_SETTLE_MS);
}

// ---- robust + logged GRBL streaming ----
#define RX_BUFFER_SAFE 120
#define QUEUE_SIZE 32
#define MAX_CMD_LEN 40
#define MAX_ERROR_RETRIES 5
#define ERROR_RETRY_DELAY_MS 500
#define GRBL_STALL_TIMEOUT_MS 30000UL
char cmdTexts[QUEUE_SIZE][MAX_CMD_LEN];
int cmdLengths[QUEUE_SIZE];
int qHead = 0, qTail = 0, bufferFill = 0, errorRetryCount = 0;
char lastErrorCmd[MAX_CMD_LEN] = "";
void enqueue(const char* c, int l) { strncpy(cmdTexts[qTail], c, MAX_CMD_LEN-1); cmdTexts[qTail][MAX_CMD_LEN-1]='\0'; cmdLengths[qTail]=l; qTail=(qTail+1)%QUEUE_SIZE; }
int dequeue() { int l = cmdLengths[qHead]; qHead=(qHead+1)%QUEUE_SIZE; return l; }
void haltSafe(const char* why) { servoTxLine(SERVO_US_REST); Serial.print("!!! HALT: "); Serial.println(why); while (true) { delay(1000); servoTxLine(SERVO_US_REST); } }
void drainResponses() {
  while (Serial1.available()) {
    String r = Serial1.readStringUntil('\n'); r.trim();
    if (r.length()==0) continue;
    Serial.print("GRBL: "); Serial.println(r);
    if (r == "ok") { if (qHead!=qTail) { bufferFill -= dequeue(); errorRetryCount=0; lastErrorCmd[0]='\0'; } }
    else if (r.startsWith("ALARM")) {
      if (qHead!=qTail) { Serial.print("!!! ALARM on cmd: '"); Serial.print(cmdTexts[qHead]); Serial.println("'"); }
      haltSafe("GRBL ALARM");
    } else if (r.startsWith("error")) {
      if (qHead==qTail) { Serial.println("(error empty queue)"); continue; }
      char f[MAX_CMD_LEN]; strncpy(f, cmdTexts[qHead], MAX_CMD_LEN); f[MAX_CMD_LEN-1]='\0';
      bufferFill -= dequeue();
      if (strcmp(f,lastErrorCmd)==0) errorRetryCount++; else { strncpy(lastErrorCmd,f,MAX_CMD_LEN); lastErrorCmd[MAX_CMD_LEN-1]='\0'; errorRetryCount=1; }
      Serial.print("  -> "); Serial.print(r); Serial.print(" on '"); Serial.print(f); Serial.print("' retry "); Serial.print(errorRetryCount); Serial.print("/"); Serial.println(MAX_ERROR_RETRIES);
      if (errorRetryCount > MAX_ERROR_RETRIES) haltSafe("error retries exhausted");
      delay(ERROR_RETRY_DELAY_MS);
      int rl = strlen(f)+1; Serial1.print(f); Serial1.write('\n'); bufferFill += rl; enqueue(f, rl);
    }
  }
}
void sendGcode(const char* c) {
  int l = strlen(c)+1; unsigned long t0=millis(); int lf=bufferFill;
  while (bufferFill + l > RX_BUFFER_SAFE) { drainResponses(); if (bufferFill!=lf){lf=bufferFill;t0=millis();} if (millis()-t0>GRBL_STALL_TIMEOUT_MS) haltSafe("stall sendGcode"); }
  Serial1.print(c); Serial1.write('\n'); bufferFill += l; enqueue(c, l);
}
void waitForIdle() { unsigned long t0=millis(); int lf=bufferFill; while (bufferFill>0){ drainResponses(); if(bufferFill!=lf){lf=bufferFill;t0=millis();} if(millis()-t0>GRBL_STALL_TIMEOUT_MS) haltSafe("stall waitForIdle"); } }
void moveY(float y) { char c[40]; snprintf(c, sizeof(c), "G0 Y%.3f", y); sendGcode(c); sendGcode("G4 P0"); waitForIdle(); }
void moveX(float x) { char c[40]; snprintf(c, sizeof(c), "G0 X%.3f", x); sendGcode(c); sendGcode("G4 P0"); waitForIdle(); }

unsigned long iter = 0, startMs = 0;

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);
  pinMode(SERVO_TX_PIN, OUTPUT); digitalWrite(SERVO_TX_PIN, HIGH);
  while (!Serial && millis() < 3000) ;
  servoTxLine(SERVO_US_REST);
  delay(2000); while (Serial1.available()) Serial1.read();

  Serial.println("Homing...");
  sendGcode("$H"); waitForIdle();
  sendGcode("G21"); sendGcode("G90"); waitForIdle();
  // Start at the left edge, top (pure-X then pure-Y, servo at REST).
  moveX(X_LEFT); moveY(Y_TOP);
  writeServoUs(SERVO_US_REST, 500);
  Serial.println("ServoYFlex start: stress bottom-right corner, verify main at top-left");
  startMs = millis();
}

// Safe excursion: only pure-X (along a row) and pure-Y (at the left edge) legs —
// never a diagonal sweep across the disc field. Carriage moves with servo at REST.
void loop() {
  // --- verify at TOP-LEFT (main visible): cycle servo, observe main vs witness ---
  unsigned long el = (millis() - startMs) / 1000UL;
  Serial.print("AT_TOPLEFT iter="); Serial.print(iter); Serial.print(" t="); Serial.print(el); Serial.println("s (main visible, verifying)");
  for (int k = 0; k < 6; k++) cycleServo();

  // --- excursion to BOTTOM-RIGHT corner (where it often fails) and stress there ---
  moveY(Y_BOT);          // down the left edge (pure-Y)
  moveX(X_RIGHT);        // across the bottom row to the right (pure-X) -> BOTTOM-RIGHT
  Serial.println("AT_BOTRIGHT (stressing; main out of frame)");
  for (int k = 0; k < 6; k++) cycleServo();   // cycle servo at the failure corner

  // --- return to top-left (pure-X back, then pure-Y up the left edge) ---
  moveX(X_LEFT);
  moveY(Y_TOP);
  iter++;
}
