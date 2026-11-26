#include "net/Network.h"

#include <WiFi.h>
#include <time.h>
#include "config.h"

namespace net {

static bool waitForClock(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (time(nullptr) < 1700000000) {  // still near 1970
    if (millis() - start > timeoutMs) return false;
    delay(100);
  }
  return true;
}

bool begin(const char *ssid, const char *password, uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);  // lower latency for polling and taps
  WiFi.begin(ssid, password);
  // SNTP keeps retrying in the background, so the clock still syncs if WiFi
  // only connects after the timeout below.
  configTzTime(CLOCK_TIMEZONE, "pool.ntp.org", "time.google.com");

  Serial.printf("WiFi: connecting to \"%s\"", ssid);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > timeoutMs) {
      Serial.println(" timed out");
      return false;
    }
    Serial.print('.');
    delay(250);
  }
  Serial.printf(" connected, IP %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());

  if (!waitForClock(10000)) {
    Serial.println("NTP: clock sync timed out; HTTPS will fail until it syncs");
    return false;
  }
  Serial.println("NTP: clock synced");
  return true;
}

bool connected() { return WiFi.status() == WL_CONNECTED; }
String localAddress() { return connected() ? WiFi.localIP().toString() : String(); }

}  // namespace net
