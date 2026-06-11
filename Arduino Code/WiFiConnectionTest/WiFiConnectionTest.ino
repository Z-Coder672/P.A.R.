// WiFi + HTTPS smoke test for the P.A.R. Nano RP2040 Connect.
//
// Connects to WiFi (same retry/watchdog pattern as PARMain), then GETs
// /gallery.php every 10s and prints status + body length. /gallery.php is
// read-only — it never mutates queue.txt or the gallery dir, so it's safe to
// hammer while testing.
//
// Things to try once it's running:
//   - Kill the AP and watch ensureWiFi() retry, then reset the MCU after 60s.
//   - Pull the antenna mid-fetch; the next loop should reconnect cleanly.
//   - Block par.zimmzimm.com at the router; ssl.connect() should fail and the
//     loop should keep polling without wedging.

#include <WiFiNINA.h>
#include "env.h"

const char* SSID     = "ab guest";
const char* PASSWORD = WIFI_PASSWORD;
const char* SERVER   = "par.zimmzimm.com";
const int   PORT     = 443;

const unsigned long WIFI_ATTEMPT_TIMEOUT_MS = 10000;
const unsigned long WIFI_STALL_TIMEOUT_MS   = 60000;
const unsigned long HTTP_RESPONSE_TIMEOUT_MS = 15000;
const unsigned long POLL_INTERVAL_MS         = 10000;

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

// Raw GET, mirroring fetchNext() in PARMain — fresh WiFiSSLClient per call,
// hand-written HTTP request, parse status + body length. We don't decode the
// body (gallery.php can be a few KB) — we just count bytes so we know TLS is
// actually working end-to-end.
bool getGallery(int& outStatus, size_t& outBodyBytes) {
  outStatus = 0;
  outBodyBytes = 0;

  WiFiSSLClient ssl;
  if (!ssl.connect(SERVER, PORT)) {
    Serial.println("connect() failed");
    return false;
  }

  ssl.print("GET /gallery.php HTTP/1.1\r\n");
  ssl.print("Host: "); ssl.print(SERVER); ssl.print("\r\n");
  ssl.print("User-Agent: P.A.R.-WiFiTest/1.0\r\n");
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

  // Skip headers
  while (ssl.connected()) {
    String line = ssl.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
  }

  // Count body bytes until close (or timeout). We don't honor chunked framing
  // here — Cloudflare sends chunked, but we're only counting bytes for a
  // smoke test, so the chunk-size lines just inflate the count slightly.
  unsigned long lastProgress = millis();
  while (ssl.connected() || ssl.available()) {
    if (ssl.available()) {
      ssl.read();
      outBodyBytes++;
      lastProgress = millis();
    } else if (millis() - lastProgress > HTTP_RESPONSE_TIMEOUT_MS) {
      Serial.println("body timeout");
      ssl.stop();
      return false;
    }
  }
  ssl.stop();
  return true;
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}
  Serial.println("\n=== WiFi connection test ===");
  ensureWiFi();
}

unsigned long iter = 0;
void loop() {
  ensureWiFi();
  iter++;
  Serial.print("[#"); Serial.print(iter); Serial.print("] GET /gallery.php ... ");
  unsigned long t0 = millis();
  int status = 0;
  size_t bodyBytes = 0;
  bool ok = getGallery(status, bodyBytes);
  unsigned long dt = millis() - t0;
  if (ok) {
    Serial.print("status="); Serial.print(status);
    Serial.print(" body="); Serial.print(bodyBytes); Serial.print(" bytes");
    Serial.print(" t="); Serial.print(dt); Serial.println("ms");
  } else {
    Serial.print("FAILED after "); Serial.print(dt); Serial.println("ms");
  }
  delay(POLL_INTERVAL_MS);
}
