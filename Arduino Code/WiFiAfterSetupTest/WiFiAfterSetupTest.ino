// Reproduces PARMain's full setup() ordering up to the point where ensureWiFi()
// runs, then polls /gallery.php like WiFiConnectionTest. The plain WiFi smoke
// test passes on this rig, but the real sketch sits ~60s after scanGrid() and
// reboots via the WiFi stall watchdog. This sketch lets us bisect *which* part
// of PARMain's pre-WiFi setup wedges the NINA module.
//
// Toggle these defines and reflash to isolate the culprit:
//   STEP_SERIAL1   — open Serial1 to GRBL (no traffic, just the port)
//   STEP_TCS_PINS  — configure TCS3200 pins + S0/S1 scaling
//   STEP_SERVO     — attach Servo on D9 and command REST
//   STEP_PULSEIN   — run a scanGrid-shaped pulseIn loop (no motion, no Serial1
//                    traffic) to stress the same blocking-read path
//   STEP_GRBL_BOOT — actually send $H / $1=255 / G21 / G90 like PARMain does
//                    (only enable if the rig is wired up and ready to home)
//   STEP_GCODE_STREAM — stream a long burst of `G4 P0` dwells through Serial1
//                    using the same character-counting protocol PARMain uses,
//                    matching the UART pressure scanGrid produces without
//                    moving the gantry. Requires STEP_GRBL_BOOT (GRBL must be
//                    out of alarm state before it'll ack anything else).
//   STEP_FULL_SCAN — replicate scanGrid for real: G0-move the head across the
//                    full grid, doing tcsReadRGBC at each cell, with a rehome
//                    halfway through. Requires STEP_GRBL_BOOT and a live rig.
//
// Recommended sweep:
//   1. All STEP_* off → confirms WiFi still works in this sketch's skeleton.
//   2. Enable STEP_SERIAL1. Reflash. If WiFi breaks, it's the UART init.
//   3. Add STEP_TCS_PINS. If WiFi breaks, it's the TCS pin config.
//   4. Add STEP_SERVO. If WiFi breaks now and was fine before, it's the Servo
//      lib grabbing a timer/PWM slice the NINA driver needs.
//   5. Add STEP_PULSEIN. If WiFi breaks, the long blocking pulseIn storm is
//      starving the NINA SPI link.
//   6. Add STEP_GRBL_BOOT (only with the rig live). If WiFi only breaks here,
//      it's power sag from the steppers, not a software conflict.
//   7. Add STEP_GCODE_STREAM. If WiFi breaks, sustained Serial1 traffic is the
//      culprit (UART driver / NINA SPI contention).
//   8. Add STEP_FULL_SCAN. If WiFi only breaks here, the difference between
//      this and step 7 is *gantry motion* — almost certainly power sag.

#include <WiFiNINA.h>
#include <Servo.h>
#include "env.h"

// ── Bisection toggles ────────────────────────────────────────────────────────
#define STEP_SERIAL1   1
#define STEP_TCS_PINS  1
#define STEP_SERVO     1
#define STEP_PULSEIN   1
#define STEP_GRBL_BOOT    1  // requires the gantry to actually be ready to home
#define STEP_GCODE_STREAM 1  // sustained Serial1 traffic; needs STEP_GRBL_BOOT
#define STEP_FULL_SCAN    0  // real scanGrid replica; needs STEP_GRBL_BOOT + live rig

// ── WiFi config (mirrors PARMain / WiFiConnectionTest) ───────────────────────
const char* SSID     = "ab guest";
const char* PASSWORD = WIFI_PASSWORD;
const char* SERVER   = "par.zimmzimm.com";
const int   PORT     = 443;

const unsigned long WIFI_ATTEMPT_TIMEOUT_MS  = 10000;
const unsigned long WIFI_STALL_TIMEOUT_MS    = 60000;
const unsigned long HTTP_RESPONSE_TIMEOUT_MS = 15000;
const unsigned long POLL_INTERVAL_MS         = 10000;

// ── TCS3200 / Servo pins (mirror PARMain) ────────────────────────────────────
const int TCS_S0  = 4;
const int TCS_S1  = 5;
const int TCS_S2  = 6;
const int TCS_S3  = 7;
const int TCS_OUT = 8;

const int SERVO_PIN       = 9;
const int SERVO_US_REST   = 544;

Servo flipServo;

// scanGrid is 18*37 = 666 cells; each cell does 5 frames × 4 filters = 20
// pulseIn calls. We approximate the same blocking-read load here without any
// gantry motion or Serial1 traffic, so we're testing only the pulseIn pressure.
const int PULSEIN_CELL_COUNT = 666;
const int PULSEIN_READS_PER_CELL = 20;

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.println("WiFi not connected, (re)connecting...");
  unsigned long stallT0 = millis();
  for (int attempt = 1; ; attempt++) {
    Serial.print("WiFi attempt "); Serial.print(attempt); Serial.print(": ");
    WiFi.disconnect();
    WiFi.end();
    delay(100);
    WiFi.begin(SSID, PASSWORD);
    unsigned long t0 = millis();
    while (millis() - t0 < WIFI_ATTEMPT_TIMEOUT_MS) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println(" connected.");
        Serial.print("IP: "); Serial.println(WiFi.localIP());
        Serial.print("RSSI: "); Serial.println(WiFi.RSSI());
        return;
      }
      delay(500);
      Serial.print(".");
    }
    Serial.println(" timeout");
    if (millis() - stallT0 > WIFI_STALL_TIMEOUT_MS) {
      Serial.println("!!! WiFi stall, forcing MCU reset");
      Serial.flush();
      delay(50);
      NVIC_SystemReset();
    }
  }
}

// Same call PARMain's loop() makes: GET /next.php with chunked-aware body
// decode and X-Gallery-Id header capture. Copied verbatim from fetchNext() so
// any wedge specific to that code path reproduces here too.
//
// IMPORTANT: /next.php POPs a queue item when one exists, so running this with
// a non-empty queue will consume entries (they'll be created as gallery
// pending entries that never get a real photo). The smoke test is safe to run
// only when the queue is empty (body will be "NONE").
bool fetchNext(int& outStatus, String& outGalleryId, String& outBody) {
  WiFiSSLClient ssl;
  if (!ssl.connect(SERVER, PORT)) {
    Serial.println("connect() failed");
    return false;
  }

  ssl.print("GET /next.php HTTP/1.1\r\n");
  ssl.print("Host: "); ssl.print(SERVER); ssl.print("\r\n");
  ssl.print("User-Agent: P.A.R./1.0\r\n");
  ssl.print("Accept: */*\r\n");
  ssl.print("Connection: close\r\n");
  ssl.print("\r\n");

  unsigned long t0 = millis();
  while (ssl.connected() && !ssl.available()) {
    if (millis() - t0 > HTTP_RESPONSE_TIMEOUT_MS) {
      Serial.println("response timeout");
      ssl.stop();
      return false;
    }
    delay(10);
  }

  String statusLine = ssl.readStringUntil('\n');
  int sp1 = statusLine.indexOf(' ');
  int sp2 = statusLine.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) { ssl.stop(); return false; }
  outStatus = statusLine.substring(sp1 + 1, sp2).toInt();

  const size_t MAX_HEADER_BYTES = 4096;
  size_t headerBytes = 0;
  bool chunked = false;
  while (ssl.connected()) {
    String line = ssl.readStringUntil('\n');
    headerBytes += line.length() + 1;
    if (headerBytes > MAX_HEADER_BYTES) {
      Serial.println("headers too large");
      ssl.stop();
      return false;
    }
    line.trim();
    if (line.length() == 0) break;
    int colon = line.indexOf(':');
    if (colon < 0) continue;
    String name = line.substring(0, colon);
    String value = line.substring(colon + 1);
    name.trim(); value.trim();
    if (name.equalsIgnoreCase("X-Gallery-Id")) outGalleryId = value;
    if (name.equalsIgnoreCase("Transfer-Encoding") && value.equalsIgnoreCase("chunked")) chunked = true;
  }

  const unsigned long BODY_TIMEOUT_MS = 60000;
  outBody = "";
  if (chunked) {
    unsigned long lastProgress = millis();
    while (ssl.connected() || ssl.available()) {
      if (millis() - lastProgress > BODY_TIMEOUT_MS) {
        Serial.println("chunked body timeout");
        ssl.stop();
        return false;
      }
      String sizeLine = ssl.readStringUntil('\n');
      sizeLine.trim();
      if (sizeLine.length() == 0) continue;
      int chunkSize = (int)strtol(sizeLine.c_str(), nullptr, 16);
      if (chunkSize <= 0) break;
      lastProgress = millis();
      while (chunkSize > 0) {
        if (ssl.available()) {
          outBody += (char)ssl.read();
          chunkSize--;
          lastProgress = millis();
        } else if (millis() - lastProgress > BODY_TIMEOUT_MS) {
          Serial.println("chunk read timeout");
          ssl.stop();
          return false;
        }
      }
      unsigned long tr = millis();
      int got = 0;
      while (got < 2 && millis() - tr < 1000) {
        if (ssl.available()) { ssl.read(); got++; }
      }
    }
  } else {
    unsigned long lastProgress = millis();
    while (ssl.connected() || ssl.available()) {
      if (ssl.available()) {
        outBody += (char)ssl.read();
        lastProgress = millis();
      } else if (millis() - lastProgress > BODY_TIMEOUT_MS) {
        Serial.println("body timeout");
        ssl.stop();
        return false;
      }
    }
  }

  outBody.trim();
  ssl.stop();
  return true;
}

// Fake-scanGrid: just hammers pulseIn on TCS_OUT with no gantry motion. If the
// sensor is wired in this'll see real edges; if not, each pulseIn will time
// out after 100ms, which is also fine — we're measuring whether the storm of
// blocking reads alone wedges WiFiNINA's SPI link.
void fakeScanPulseInStorm() {
  Serial.print("pulseIn storm: "); Serial.print(PULSEIN_CELL_COUNT);
  Serial.print(" cells x "); Serial.print(PULSEIN_READS_PER_CELL);
  Serial.println(" reads...");
  unsigned long t0 = millis();
  unsigned long totalEdges = 0;
  for (int i = 0; i < PULSEIN_CELL_COUNT; i++) {
    for (int j = 0; j < PULSEIN_READS_PER_CELL; j++) {
      unsigned long half = pulseIn(TCS_OUT, HIGH, 100000UL);
      if (half) totalEdges++;
      delay(2);
    }
  }
  Serial.print("pulseIn done in "); Serial.print(millis() - t0);
  Serial.print(" ms, edges seen: "); Serial.println(totalEdges);
}

// Mirror PARMain's GRBL boot dance — only enable if the rig is plugged in and
// ready to home. We don't bother with full ok-accounting; we just push the
// commands and pause long enough for homing to plausibly finish.
void grblBootMirror() {
  Serial.println("GRBL boot: $H ...");
  Serial1.print("$H\n");
  delay(20000); // homing budget
  Serial1.print("$1=255\n"); delay(50);
  Serial1.print("G21\n");    delay(50);
  Serial1.print("G90\n");    delay(50);
  Serial.println("GRBL boot mirror done.");
  // Drain any leftover GRBL chatter so the streaming protocol starts from a
  // clean queue/buffer state.
  while (Serial1.available()) Serial1.read();
}

// ── PARMain's GRBL character-counting streaming (verbatim) ──────────────────
const int RX_BUFFER_SAFE = 120;
const int QUEUE_SIZE = 32;
int  cmdLengths[QUEUE_SIZE];
int  qHead = 0;
int  qTail = 0;
int  bufferFill = 0;

void streamEnqueue(int len) {
  cmdLengths[qTail] = len;
  qTail = (qTail + 1) % QUEUE_SIZE;
}
int streamDequeue() {
  int len = cmdLengths[qHead];
  qHead = (qHead + 1) % QUEUE_SIZE;
  return len;
}
void streamDrain() {
  while (Serial1.available()) {
    String resp = Serial1.readStringUntil('\n');
    resp.trim();
    if (resp.length() == 0) continue;
    if (resp == "ok") {
      if (qHead != qTail) bufferFill -= streamDequeue();
    }
    // Ignore everything else (status reports, ALARM, errors) — this is a
    // smoke test, not a production driver.
  }
}
void streamSend(const char* cmd) {
  int cmdLen = strlen(cmd) + 1;
  while (bufferFill + cmdLen > RX_BUFFER_SAFE) streamDrain();
  Serial1.print(cmd);
  Serial1.write('\n');
  bufferFill += cmdLen;
  streamEnqueue(cmdLen);
}
void streamWaitIdle() {
  while (bufferFill > 0) streamDrain();
}

// Match scanGrid's UART pressure without moving the gantry. `G4 P0` is a
// zero-duration dwell — GRBL plans it, syncs, and acks. 666 cells × ~5 ops
// per cell ≈ 3000 acks; runs for similar wall-clock to a real scanGrid.
void gcodeStreamStress() {
  Serial.println("G-code stream stress: 3000 x G4 P0...");
  unsigned long t0 = millis();
  for (int i = 0; i < 3000; i++) {
    streamSend("G4 P0");
    if ((i % 200) == 0) {
      Serial.print("  streamed "); Serial.print(i);
      Serial.print(", bufferFill="); Serial.println(bufferFill);
    }
  }
  streamWaitIdle();
  Serial.print("G-code stream done in "); Serial.print(millis() - t0);
  Serial.println(" ms");
}

// Real-scan replica: small motions across a synthetic 37×18 grid in the same
// coordinate frame PARMain uses, plus a midway rehome. The TCS reads are real
// (5-frame averaged RGBC) so the full pulseIn pressure stays in the loop too.
const int   GRID_W = 37;
const int   GRID_H = 18;
const float X_TRAVEL = 777.695f;
const float Y_TRAVEL = 399.695f;
const float SCAN_OFFSET_X = -23.0f;
const float SCAN_OFFSET_Y =   4.0f;

float gridX(int x) { return -X_TRAVEL + 25.0f + 20.045f * x; }
float gridY(int y) { return -Y_TRAVEL + 23.40f * ((GRID_H - 1) - y); }

void streamMoveTo(float x, float y) {
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "G0 X%.3f Y%.3f", x, y);
  streamSend(cmd);
}
void streamMoveToYSafe(float targetX, float targetY) {
  char cmd[40];
  float xLimit = (targetX > -X_TRAVEL / 2.0f) ? 0.0f : -X_TRAVEL;
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", xLimit);  streamSend(cmd);
  snprintf(cmd, sizeof(cmd), "G0 Y%.3f", targetY); streamSend(cmd);
  snprintf(cmd, sizeof(cmd), "G0 X%.3f", targetX); streamSend(cmd);
}
void streamWaitMotion() {
  streamSend("G4 P0");
  streamWaitIdle();
}

void tcsSelect(int s2, int s3) {
  digitalWrite(TCS_S2, s2 ? HIGH : LOW);
  digitalWrite(TCS_S3, s3 ? HIGH : LOW);
}
unsigned long tcsHz() {
  unsigned long h = pulseIn(TCS_OUT, HIGH, 100000UL);
  if (h == 0) return 0;
  return 500000UL / h;
}
void tcsReadRGBC(unsigned long& r, unsigned long& g,
                 unsigned long& b, unsigned long& c) {
  uint32_t sr=0, sg=0, sb=0, sc=0;
  for (int i = 0; i < 5; i++) {
    tcsSelect(0, 0); delay(2); sr += tcsHz();
    tcsSelect(1, 1); delay(2); sg += tcsHz();
    tcsSelect(0, 1); delay(2); sb += tcsHz();
    tcsSelect(1, 0); delay(2); sc += tcsHz();
  }
  r = sr/5; g = sg/5; b = sb/5; c = sc/5;
}

void rehomeReplica() {
  streamSend("$H");
  streamWaitIdle();
  streamSend("$1=255");
  streamSend("G21");
  streamSend("G90");
  streamWaitIdle();
}

void fullScanReplica() {
  Serial.println("Full-scan replica starting...");
  unsigned long t0 = millis();
  bool ltr = true;
  streamMoveToYSafe(gridX(0) + SCAN_OFFSET_X, gridY(0) + SCAN_OFFSET_Y);
  for (int y = 0; y < GRID_H; y++) {
    int startCol = ltr ? 0 : GRID_W - 1;
    int endCol   = ltr ? GRID_W - 1 : 0;
    int step     = ltr ? +1 : -1;
    for (int x = startCol; x != endCol + step; x += step) {
      streamMoveTo(gridX(x) + SCAN_OFFSET_X, gridY(y) + SCAN_OFFSET_Y);
      streamWaitMotion();
      unsigned long r, g, b, c;
      tcsReadRGBC(r, g, b, c);
    }
    if (y + 1 < GRID_H) {
      if (y == 8) rehomeReplica();
      streamMoveToYSafe(gridX(endCol) + SCAN_OFFSET_X,
                        gridY(y + 1) + SCAN_OFFSET_Y);
      ltr = !ltr;
    }
    Serial.print("  row "); Serial.print(y); Serial.print(" done at t=");
    Serial.println(millis() - t0);
  }
  Serial.print("Full-scan replica done in "); Serial.print(millis() - t0);
  Serial.println(" ms");
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}
  Serial.println("\n=== WiFi-after-setup test ===");
  Serial.print("Steps enabled:");
  if (STEP_SERIAL1)   Serial.print(" SERIAL1");
  if (STEP_TCS_PINS)  Serial.print(" TCS_PINS");
  if (STEP_SERVO)     Serial.print(" SERVO");
  if (STEP_PULSEIN)   Serial.print(" PULSEIN");
  if (STEP_GRBL_BOOT)    Serial.print(" GRBL_BOOT");
  if (STEP_GCODE_STREAM) Serial.print(" GCODE_STREAM");
  if (STEP_FULL_SCAN)    Serial.print(" FULL_SCAN");
  Serial.println();

#if STEP_SERIAL1
  Serial1.begin(115200);
  Serial.println("Serial1 opened.");
#endif

#if STEP_TCS_PINS
  pinMode(TCS_S0, OUTPUT);
  pinMode(TCS_S1, OUTPUT);
  pinMode(TCS_S2, OUTPUT);
  pinMode(TCS_S3, OUTPUT);
  pinMode(TCS_OUT, INPUT);
  digitalWrite(TCS_S0, HIGH);
  digitalWrite(TCS_S1, LOW);
  digitalWrite(TCS_S2, HIGH);  // TCS_CLEAR
  digitalWrite(TCS_S3, LOW);
  Serial.println("TCS pins configured.");
#endif

#if STEP_SERVO
  flipServo.attach(SERVO_PIN);
  flipServo.writeMicroseconds(SERVO_US_REST);
  Serial.println("Servo attached at REST.");
#endif

  delay(2000); // matches PARMain's GRBL boot wait

#if STEP_GRBL_BOOT
  grblBootMirror();
#endif

#if STEP_PULSEIN
  fakeScanPulseInStorm();
#endif

#if STEP_GCODE_STREAM
  gcodeStreamStress();
#endif

#if STEP_FULL_SCAN
  fullScanReplica();
#endif

  Serial.println("--- pre-WiFi setup done, calling ensureWiFi() ---");
  ensureWiFi();
}

unsigned long iter = 0;
void loop() {
  ensureWiFi();
  iter++;
  Serial.print("[#"); Serial.print(iter); Serial.print("] GET /next.php ... ");
  unsigned long t0 = millis();
  int status = 0;
  String galleryId = "";
  String body = "";
  bool ok = fetchNext(status, galleryId, body);
  unsigned long dt = millis() - t0;
  if (ok) {
    Serial.print("status="); Serial.print(status);
    Serial.print(" id='"); Serial.print(galleryId); Serial.print("'");
    Serial.print(" bodyLen="); Serial.print(body.length());
    Serial.print(" body='"); Serial.print(body); Serial.print("'");
    Serial.print(" t="); Serial.print(dt); Serial.println("ms");
  } else {
    Serial.print("FAILED after "); Serial.print(dt); Serial.println("ms");
  }
  delay(POLL_INTERVAL_MS);
}
