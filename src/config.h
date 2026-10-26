#pragma once

#include <stdint.h>

constexpr uint8_t SCREEN_ROTATION = 1;     // landscape; use 3 if the picture is upside down
constexpr uint32_t POLL_INTERVAL_MS = 1000;
constexpr uint32_t TOUCH_RELEASE_MS = 60;  // ignore resistive-touch dropouts shorter than this
constexpr uint8_t BOOT_BUTTON = 0;         // hold 1s to recalibrate touch
// America/Chicago, including automatic daylight saving time changes.
constexpr const char *CLOCK_TIMEZONE = "CST6CDT,M3.2.0,M11.1.0";
