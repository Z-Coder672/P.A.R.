// ServoWiggleTest — localize the intermittent open in the MAIN servo's cable.
//
// Diagnosis: the main servo fails on the right side (carriage at max +X
// extension) while the witness (short fixed cable, same ServoNano output) stays
// fine -> the fault is an intermittent open in the MAIN servo's own long cable
// (or a connector), worst when the cable is pulled taut at the right extreme.
//
// This parks the carriage at the bottom-right (cable stretched) and cycles the
// servo continuously. Flex the main cable along its length and wiggle each
// connector by hand: the spot where the MAIN drops out / comes back (while the
// witness keeps cycling steadily) is the bad connection. Then fix it (re-seat /
// re-crimp / replace that segment; add a service loop so it isn't pulled taut).
//
// SAFETY: the carriage is moved to the corner once with the servo at REST via
// pure-X / pure-Y legs at the limits (no diagonal field sweep); after that it
// stays put and only the servo actuates.

const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 412.0f;
const float X_LEFT  = -775.0f;
const float X_RIGHT = -31.0f;   // rightmost disc column
const float Y_TOP   = -3.0f;
const float Y_BOT   = -399.0f;

const int SERVO_US_REST    = 565;
const int SERVO_US_RELEASE = 1018;  // ≈46° (raised 8° from the prior 936/≈38°)
const int SERVO_US_ENGAGE  = 1471;
const int SERVO_90_DEG_SETTLE_MS = 300;
const int SERVO_50_DEG_SETTLE_MS = 100;

const int SERVO_TX_PIN = D9;
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

void servoTxLine(int us) { char b[12]; snprintf(b, sizeof(b), "\n%d\n", us); for (int r=0;r<SERVO_TX_REPEATS;r++) { Serial2.print(b); Serial2.flush(); if (r+1<SERVO_TX_REPEATS) delay(SERVO_TX_REPEAT_GAP_MS); } }
void writeServoUs(int us, int s) { servoTxLine(us); delay(s); }
void cycleServo() {
  writeServoUs(SERVO_US_ENGAGE,  SERVO_90_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_REST,    SERVO_90_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_RELEASE, SERVO_50_DEG_SETTLE_MS);
  writeServoUs(SERVO_US_REST,    SERVO_50_DEG_SETTLE_MS);
}

#define RX_BUFFER_SAFE 120
#define QUEUE_SIZE 32
#define MAX_CMD_LEN 40
#define GRBL_STALL_TIMEOUT_MS 30000UL
int cmdLengths[QUEUE_SIZE]; int qHead=0,qTail=0,bufferFill=0;
void enqueue(int l){cmdLengths[qTail]=l;qTail=(qTail+1)%QUEUE_SIZE;}
int dequeue(){int l=cmdLengths[qHead];qHead=(qHead+1)%QUEUE_SIZE;return l;}
void haltSafe(const char* why){ servoTxLine(SERVO_US_REST); Serial.print("!!! HALT: "); Serial.println(why); while(true){delay(1000);servoTxLine(SERVO_US_REST);} }
void drainResponses(){
  while(Serial1.available()){
    String r=Serial1.readStringUntil('\n'); r.trim(); if(!r.length()) continue;
    Serial.print("GRBL: "); Serial.println(r);
    if(r=="ok"){ if(qHead!=qTail) bufferFill-=dequeue(); }
    else if(r.startsWith("ALARM")) haltSafe("GRBL ALARM");
    else if(r.startsWith("error")){ if(qHead!=qTail) bufferFill-=dequeue(); }
  }
}
void sendGcode(const char* c){ int l=strlen(c)+1; unsigned long t0=millis(); int lf=bufferFill;
  while(bufferFill+l>RX_BUFFER_SAFE){drainResponses(); if(bufferFill!=lf){lf=bufferFill;t0=millis();} if(millis()-t0>GRBL_STALL_TIMEOUT_MS) haltSafe("stall");}
  Serial1.print(c); Serial1.write('\n'); bufferFill+=l; enqueue(l); }
void waitForIdle(){ unsigned long t0=millis(); int lf=bufferFill; while(bufferFill>0){drainResponses(); if(bufferFill!=lf){lf=bufferFill;t0=millis();} if(millis()-t0>GRBL_STALL_TIMEOUT_MS) haltSafe("stall");} }
void moveX(float x){char c[40];snprintf(c,sizeof(c),"G0 X%.3f",x);sendGcode(c);sendGcode("G4 P0");waitForIdle();}
void moveY(float y){char c[40];snprintf(c,sizeof(c),"G0 Y%.3f",y);sendGcode(c);sendGcode("G4 P0");waitForIdle();}

unsigned long n = 0;

void setup() {
  Serial.begin(115200);
  Serial0.end();  // D0/D1 belong to the GRBL link, not the console UART
  Serial1.begin(115200, SERIAL_8N1, D0, D1);
  Serial2.begin(9600, SERIAL_8N1, -1, SERVO_TX_PIN);  // TX-only servo link on D9
  while (!Serial && millis() < 3000) ;
  servoTxLine(SERVO_US_REST);
  delay(2000); while (Serial1.available()) Serial1.read();

  Serial.println("Homing...");
  sendGcode("$H"); waitForIdle();
  sendGcode("G21"); sendGcode("G90"); waitForIdle();
  // Go to bottom-right (cable stretched), pure legs, servo at REST.
  moveX(X_LEFT); moveY(Y_BOT); moveX(X_RIGHT);
  writeServoUs(SERVO_US_REST, 500);
  Serial.println("AT BOTTOM-RIGHT. Cycling servo forever — wiggle the MAIN cable/connectors to find the open.");
}

void loop() {
  cycleServo();
  n++;
  if (n % 5 == 0) { Serial.print("cycle "); Serial.println(n); }
}
