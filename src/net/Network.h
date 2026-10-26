#pragma once

#include <Arduino.h>

// WiFi + clock setup shared by every networked backend. HTTPS certificate checks
// need the real date, so the clock is synced over NTP after connecting.
namespace net {

// Connects to WiFi and syncs the clock. Returns false on timeout; WiFi keeps
// retrying in the background either way.
bool begin(const char *ssid, const char *password, uint32_t timeoutMs);

bool connected();

}  // namespace net
