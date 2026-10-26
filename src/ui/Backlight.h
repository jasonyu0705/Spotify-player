#pragma once

#include <Arduino.h>

// Local display setting, independent of Spotify and network latency.
class Backlight {
 public:
  void begin();
  void cycle();
  void tick();
  uint8_t percent() const { return percent_; }

 private:
  void apply();
  uint8_t percent_ = 100;
  uint32_t changedAt_ = 0;
  bool dirty_ = false;
};
