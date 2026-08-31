// WiFi scan diagnostic for the P.A.R. Arduino Nano ESP32.
//
// Purpose: when PARMain keeps logging `s=1` (WL_NO_SSID_AVAIL), this answers the
// only question that matters — can the radio actually SEE "ab guest" from the
// rig's physical location? It scans and prints every SSID it finds, with
// channel, RSSI, and encryption, then flags any "ab guest" hit.
//
// The ESP32-S3's radio is 2.4 GHz only, same as the NINA it replaced. Channel
// 1-13 = 2.4 GHz (joinable); 5 GHz APs are not reported at all. So:
//   - "ab guest" appears with decent RSSI  -> radio is fine; the NO_SSID_AVAIL
//     in PARMain is an association/auth issue, look there next.
//   - "ab guest" never appears             -> the AP isn't broadcasting that
//     SSID on 2.4 GHz from here. No firmware change helps; fix the AP side
//     (re-enable/!split the 2.4 GHz radio, move the rig, check the channel).
//
// PORT: the WL_NO_MODULE probe and WiFi.firmwareVersion() print are gone. Both
// described the NINA co-processor's SPI link and its own firmware image; the
// ESP32-S3 radio is on-die, so there is no separate module to be missing and no
// second firmware version to report. Faking either would only report on the
// host core.
//
// No credentials needed — scanning is passive. Run with Serial Monitor @115200.

#include <WiFi.h>

const char* TARGET_SSID = "ab guest";
const unsigned long SCAN_INTERVAL_MS = 8000;

// On the ESP32 core encryptionType() returns a wifi_auth_mode_t, which draws
// finer distinctions than the NINA's five ENC_TYPE_* values did.
const char* encStr(wifi_auth_mode_t t) {
  switch (t) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ent";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "?";
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}
  Serial.println("\n=== WiFi scan test ===");

  // Station mode without connecting: scanNetworks() needs the interface up.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
}

unsigned long scanNum = 0;
void loop() {
  scanNum++;
  Serial.print("\n[scan #"); Serial.print(scanNum); Serial.println("] scanning...");

  int n = WiFi.scanNetworks();  // blocking; returns count or -1 on failure
  if (n < 0) {
    Serial.println("  scanNetworks() failed (-1) — module may be wedged");
    delay(SCAN_INTERVAL_MS);
    return;
  }
  if (n == 0) {
    Serial.println("  no networks found at all (radio or RF problem)");
    delay(SCAN_INTERVAL_MS);
    return;
  }

  bool sawTarget = false;
  Serial.print("  "); Serial.print(n); Serial.println(" network(s):");
  for (int i = 0; i < n; i++) {
    int rssi = WiFi.RSSI(i);
    int ch   = WiFi.channel(i);
    String s = WiFi.SSID(i);
    Serial.print("    ");
    Serial.print(s);
    Serial.print("  ch="); Serial.print(ch);
    Serial.print("  rssi="); Serial.print(rssi); Serial.print("dBm");
    Serial.print("  ["); Serial.print(encStr(WiFi.encryptionType(i))); Serial.print("]");
    if (s == TARGET_SSID) {
      sawTarget = true;
      Serial.print("   <<< TARGET");
    }
    Serial.println();
  }

  Serial.print("  => \"");
  Serial.print(TARGET_SSID);
  Serial.println(sawTarget ? "\" IS visible on 2.4 GHz from here."
                           : "\" NOT seen — AP-side 2.4 GHz problem.");

  delay(SCAN_INTERVAL_MS);
}
