#pragma once

#include <stdint.h>
#include <cstring>

enum class RepeatMode : uint8_t { Off, All, One };

inline RepeatMode nextRepeatMode(RepeatMode mode) {
  switch (mode) {
    case RepeatMode::Off: return RepeatMode::All;
    case RepeatMode::All: return RepeatMode::One;
    default: return RepeatMode::Off;
  }
}

inline const char *spotifyRepeatState(RepeatMode mode) {
  switch (mode) {
    case RepeatMode::All: return "context";
    case RepeatMode::One: return "track";
    default: return "off";
  }
}

inline RepeatMode repeatModeFromSpotify(const char *state) {
  if (state && std::strcmp(state, "context") == 0) return RepeatMode::All;
  if (state && std::strcmp(state, "track") == 0) return RepeatMode::One;
  return RepeatMode::Off;
}
