#pragma once

#include <Arduino.h>
#include "backend/TelemetryInfo.h"

// Cached ambient data from the backend. No UI code performs network requests.
struct IdleInfo {
  TelemetryInfo telemetry;
  bool usesPi = false;
  bool piOnline = false;
  bool piAuthError = false;
  uint32_t lastPiReplyMs = 0;
  String piAddress;
  bool pollingSupported = false;
  bool pollingEnabled = true;

  bool weatherSupported = false;
  bool weatherAvailable = false;
  bool weatherStale = false;
  String weatherStatus;
  String location;
  String condition;
  float temperature = 0;
  bool fahrenheit = false;
  int weatherCode = -1;
  bool isDay = true;
  uint32_t weatherAgeSeconds = 0;
  uint32_t weatherReceivedAtMs = 0;
};
