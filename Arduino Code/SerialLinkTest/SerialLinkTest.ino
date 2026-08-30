// SerialLinkTest — bench triage for the main-MCU ↔ GRBL-Mega serial link.
//
// WHY THIS EXISTS
// 2026-08-23: 12 V was accidentally shorted onto the RX line. The flash log
// shows the signature of exactly that, in two stages:
//   1. Three corrupted responses arrived as 0xFF bytes glued to real acks
//      (`\xffok`), which killed gallery jobs 79 and 80.
//   2. Then GRBL appeared to go silent entirely — 12 consecutive boots timed
//      out waiting for `$H` to ack.
//
// Stage 2 is very probably NOT a dead Mega. D0 (GPIO44) is the ESP32-S3's
// RECEIVE pin. If 12 V destroyed it, GRBL would still be homing perfectly well
// while the MCU sees nothing at all. THE GANTRY MAY HAVE BEEN MOVING ON EVERY
// ONE OF THOSE 12 BOOTS. Assume it did until this sketch says otherwise.
//
// This sketch NEVER commands motion. It only ever sends `?` (realtime status
// query) and `$$` (dump settings) — both are motion-free. There is no `$H`
// anywhere in this file, deliberately. It also never opens the servo UART, so
// the flip arm is not touched.
//
// TESTS (menu-driven over USB serial @ 115200)
//   p  pin integrity   — D0/D1 as plain GPIO, pullup vs pulldown. No wiring
//                        change needed, but UNPLUG THE MEGA first or its idle-
//                        high TX will hold D0 up and fake a pass.
//   l  loopback        — needs a jumper D0 ↔ D1 and the MEGA UNPLUGGED. Uses
//                        only ESP32 pins, so it isolates the MCU from the Mega
//                        and the cable. THIS IS THE DECISIVE TEST.
//   g  GRBL listen     — Mega reconnected. Sends `?` / `$$`, hex-dumps replies.
//   m  remap loopback  — same as `l` but with RX moved to a spare GPIO, to
//                        prove the recovery path if D0 is dead. Jumper that
//                        pin ↔ D1.
//   r  raw monitor     — continuous hex dump of whatever lands on RX. Use it to
//                        watch for the 0xFF noise with the Mega connected.
//
// READING THE RESULT
//   loopback passes, GRBL listen fails  -> ESP32 is fine; blame the Mega's TX1
//                                          pin or the cable.
//   loopback fails, remap loopback ok   -> D0/GPIO44 is destroyed. Move Serial1
//                                          RX to the spare pin in PARMain and
//                                          rewire; no board swap needed.
//   both loopbacks fail                 -> damage is not confined to D0.

#include <Arduino.h>

// Spare pins per PINOUT.md: A0–A3 = GPIO1–4, A6/A7 = GPIO13/14,
// D12/D13 = GPIO47/48. A0 is the first choice for a remapped RX.
#define ALT_RX_PIN A0

static const unsigned long LINK_BAUD = 115200;

// ---------------------------------------------------------------------------

static void hexDump(const uint8_t* b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (b[i] >= 0x20 && b[i] <= 0x7E) {
      Serial.printf("%02X'%c' ", b[i], (char)b[i]);
    } else {
      Serial.printf("%02X    ", b[i]);
    }
    if ((i % 12) == 11) Serial.println();
  }
  Serial.println();
}

// Release the UART so D0/D1 go back to being ordinary GPIOs.
static void freeLinkPins() {
  Serial1.end();
  delay(50);
}

// ---- p: pin integrity -----------------------------------------------------
// A healthy ESP32-S3 GPIO follows its own internal pull: pullup reads HIGH,
// pulldown reads LOW. A pin damaged by an overvoltage event typically fails one
// of the two — stuck HIGH (blown high-side clamp / leakage to rail) or stuck
// LOW (blown low-side clamp / short to ground). Either way, "both states track"
// is the only passing answer.
static bool probePin(int pin, const char* label) {
  pinMode(pin, INPUT_PULLUP);
  delay(20);
  int up = digitalRead(pin);
  pinMode(pin, INPUT_PULLDOWN);
  delay(20);
  int dn = digitalRead(pin);
  pinMode(pin, INPUT);

  bool ok = (up == HIGH && dn == LOW);
  Serial.printf("  %-4s pullup=%s  pulldown=%s  -> %s\n", label,
                up == HIGH ? "HIGH" : "LOW ",
                dn == HIGH ? "HIGH" : "LOW ",
                ok ? "OK (pin follows its pull)"
                   : (up == dn
                        ? (up == HIGH ? "FAIL - STUCK HIGH" : "FAIL - STUCK LOW")
                        : "FAIL - inverted?!"));
  return ok;
}

static void testPins() {
  Serial.println();
  Serial.println("[p] pin integrity — UNPLUG THE MEGA before trusting a pass:");
  Serial.println("    its idle-high TX1 holds D0 HIGH and masks a stuck-high pin.");
  freeLinkPins();
  bool d0 = probePin(D0, "D0");
  bool d1 = probePin(D1, "D1");
  bool alt = probePin(ALT_RX_PIN, "A0");
  Serial.printf("  verdict: D0 %s, D1 %s, spare A0 %s\n",
                d0 ? "alive" : "SUSPECT", d1 ? "alive" : "SUSPECT",
                alt ? "alive" : "SUSPECT");
  if (!d0) {
    Serial.println("  D0 is the RX pin. A stuck reading here matches the 12V event");
    Serial.println("  and would fully explain 'GRBL went silent'. Run [l] then [m].");
  }
  Serial.println();
}

// ---- loopback core --------------------------------------------------------
// Sends a fixed pattern out D1 and reads it back on rxPin. Counts byte-exact
// matches. The pattern deliberately includes 0x00 and 0xFF so a line that is
// stuck at either rail cannot accidentally "pass".
static void loopbackOn(int rxPin, const char* what) {
  Serial.printf("\n[loopback] %s  (TX=D1, RX=%s) @ %lu baud\n",
                what, rxPin == D0 ? "D0" : "A0", LINK_BAUD);
  Serial.println("  Requires a jumper between those two pins and the MEGA UNPLUGGED.");

  freeLinkPins();
  Serial1.begin(LINK_BAUD, SERIAL_8N1, rxPin, D1);
  delay(100);
  while (Serial1.available()) Serial1.read();

  static const uint8_t pattern[] = {
    0x55, 0xAA, 0x00, 0xFF, 0x0F, 0xF0, 0x41, 0x42,
    'o',  'k',  '\n', 0x7E, 0x01, 0x80, 0x33, 0xCC
  };
  const size_t N = sizeof(pattern);
  const int ROUNDS = 16;

  size_t sent = 0, got = 0, matched = 0;
  uint8_t rx[64];

  for (int r = 0; r < ROUNDS; r++) {
    Serial1.write(pattern, N);
    Serial1.flush();
    sent += N;

    size_t n = 0;
    unsigned long t0 = millis();
    while (n < N && millis() - t0 < 200) {
      if (Serial1.available()) rx[n++] = (uint8_t)Serial1.read();
    }
    got += n;
    for (size_t i = 0; i < n && i < N; i++) {
      if (rx[i] == pattern[i]) matched++;
    }
    if (r == 0) {
      Serial.print("  first round returned "); Serial.print(n); Serial.println(" byte(s):");
      hexDump(rx, n);
    }
  }

  Serial.printf("  sent=%u received=%u byte-exact=%u\n",
                (unsigned)sent, (unsigned)got, (unsigned)matched);
  if (matched == sent) {
    Serial.println("  RESULT: PASS — this RX pin and the UART are healthy.");
  } else if (got == 0) {
    Serial.println("  RESULT: FAIL — nothing came back at all.");
    Serial.println("          Either the jumper is missing or this RX pin is dead.");
    Serial.println("          Check the jumper FIRST, then re-run.");
  } else {
    Serial.println("  RESULT: FAIL — bytes returned but corrupted.");
    Serial.println("          Marginal/damaged pin, or something else driving the line.");
  }
  Serial1.end();
}

// ---- g: GRBL listen -------------------------------------------------------
// Motion-free. `?` is a realtime status query, `$$` dumps settings. Neither
// moves the gantry. A healthy Mega answers `?` with a `<Idle|MPos:...>` frame
// almost instantly.
static void testGrbl() {
  Serial.println("\n[g] GRBL listen — Mega must be CONNECTED. No motion is commanded.");
  freeLinkPins();
  Serial1.begin(LINK_BAUD, SERIAL_8N1, D0, D1);
  delay(200);
  while (Serial1.available()) Serial1.read();

  const char* probes[] = { "\n", "?", "$$" };
  for (int p = 0; p < 3; p++) {
    Serial.printf("  --> sending %s\n",
                  p == 0 ? "<newline>" : probes[p]);
    if (p == 1) {
      Serial1.write('?');            // realtime — no newline
    } else {
      Serial1.print(probes[p]);
      if (p == 2) Serial1.write('\n');
    }
    Serial1.flush();

    uint8_t buf[256];
    size_t n = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < 1500 && n < sizeof(buf)) {
      if (Serial1.available()) { buf[n++] = (uint8_t)Serial1.read(); t0 = millis(); }
    }
    if (n == 0) {
      Serial.println("      <silence>");
    } else {
      Serial.printf("      %u byte(s):\n", (unsigned)n);
      hexDump(buf, n);
    }
  }
  Serial.println("  Silence on all three, with a PASSING loopback, means the Mega's");
  Serial.println("  TX1 (D18) or the cable is the casualty rather than the ESP32.");
  Serial1.end();
}

// ---- r: raw monitor -------------------------------------------------------
static void rawMonitor() {
  Serial.println("\n[r] raw monitor on D0 — any key stops. Watching for 0xFF noise.");
  freeLinkPins();
  Serial1.begin(LINK_BAUD, SERIAL_8N1, D0, D1);
  while (Serial.available()) Serial.read();

  unsigned long total = 0, ffCount = 0;
  unsigned long lastReport = millis();
  while (!Serial.available()) {
    while (Serial1.available()) {
      uint8_t b = (uint8_t)Serial1.read();
      total++;
      if (b == 0xFF) ffCount++;
      if (b >= 0x20 && b <= 0x7E) Serial.write((char)b);
      else Serial.printf("<%02X>", b);
    }
    if (millis() - lastReport > 5000) {
      lastReport = millis();
      Serial.printf("\n  [%lu bytes, %lu of them 0xFF]\n", total, ffCount);
    }
    delay(2);
  }
  while (Serial.available()) Serial.read();
  Serial.printf("\n  stopped: %lu bytes, %lu were 0xFF\n", total, ffCount);
  Serial1.end();
}

// ---------------------------------------------------------------------------

static void menu() {
  Serial.println();
  Serial.println("=== SerialLinkTest — GRBL link triage (no motion is ever commanded) ===");
  Serial.println("  p  pin integrity  (unplug Mega)");
  Serial.println("  l  loopback on D0 (jumper D0<->D1, unplug Mega)   <-- decisive");
  Serial.println("  m  loopback on A0 (jumper A0<->D1, unplug Mega)   <-- recovery path");
  Serial.println("  g  GRBL listen    (reconnect Mega)");
  Serial.println("  r  raw monitor    (reconnect Mega)");
  Serial.println("  ?  this menu");
  Serial.print("> ");
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(300);

  // Serial0 owns D0/D1 by default on this core; release it before we touch
  // them as GPIOs or reassign them to Serial1.
  Serial0.end();

  Serial.println();
  Serial.println("SerialLinkTest ready.");
  Serial.println("Context: 12V was shorted onto the RX line. D0 = GPIO44 = Serial1 RX.");
  Serial.println("This sketch sends only `?` and `$$` — it never homes or moves anything.");
  menu();
}

void loop() {
  if (!Serial.available()) return;
  int c = Serial.read();
  if (c == '\r' || c == '\n') return;
  Serial.println((char)c);

  switch (c) {
    case 'p': testPins();                       break;
    case 'l': loopbackOn(D0, "D0 / GPIO44 — the production RX pin"); break;
    case 'm': loopbackOn(ALT_RX_PIN, "A0 / GPIO1 — spare pin, recovery path"); break;
    case 'g': testGrbl();                       break;
    case 'r': rawMonitor();                     break;
    default:                                    break;
  }
  menu();
}
