// WiFi scan diagnostic for the P.A.R. Nano RP2040 Connect.
//
// Purpose: when PARMain keeps logging `s=1` (WL_NO_SSID_AVAIL), this answers the
// only question that matters — can the NINA radio actually SEE "ab guest" from
// the rig's physical location? It scans and prints every SSID the module finds,
// with channel, RSSI, and encryption, then flags any "ab guest" hit.
//
// The NINA is 2.4 GHz only. Channel 1-13 = 2.4 GHz (joinable); the NINA won't
// report 5 GHz APs at all. So:
//   - "ab guest" appears with decent RSSI  -> radio is fine; the NO_SSID_AVAIL
//     in PARMain is an association/auth/wedged-module issue, look there next.
//   - "ab guest" never appears             -> the AP isn't broadcasting that
//     SSID on 2.4 GHz from here. No firmware change helps; fix the AP side
//     (re-enable/!split the 2.4 GHz radio, move the rig, check the channel).
//
// No credentials needed — scanning is passive. Run with Serial Monitor @115200.

#include <WiFiNINA.h>

const char* TARGET_SSID = "ab guest";
const unsigned long SCAN_INTERVAL_MS = 8000;

const char* encStr(uint8_t t) {
  switch (t) {
    case ENC_TYPE_WEP:  return "WEP";
    case ENC_TYPE_TKIP: return "WPA";
    case ENC_TYPE_CCMP: return "WPA2";
    case ENC_TYPE_NONE: return "open";
    case ENC_TYPE_AUTO: return "auto";
    default:            return "?";
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}
  Serial.println("\n=== WiFi scan test ===");

  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("!!! NINA module not found — check firmware/wiring");
    while (true) delay(1000);
  }
  Serial.print("NINA firmware: ");
  Serial.println(WiFi.firmwareVersion());
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
