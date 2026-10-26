#pragma once

#include <Arduino.h>
#include <memory>
#include "backend/RepeatMode.h"

struct AlbumArt {
  std::unique_ptr<uint8_t[]> jpeg;
  size_t length = 0;
};

// Everything the screen needs about playback. Backends fill this in; the UI only
// ever reads it, so it never sees Spotify JSON or HTTP details.
struct NowPlaying {
  String trackId;          // changes when the song changes
  String title;
  String artist;
  String album;
  String artUrl;           // ~300px JPEG; empty if none
  std::shared_ptr<const AlbumArt> artImage;  // shared across worker/UI; immutable after publication
  String deviceName;       // e.g. "Kitchen Speaker"

  uint32_t progressMs = 0;
  uint32_t durationMs = 0;
  uint32_t fetchedAtMs = 0;  // millis() when progressMs was read, for smooth interpolation

  bool isPlaying = false;
  bool isLastPlayed = false;  // retained metadata, not an active playback response
  bool shuffle = false;
  RepeatMode repeat = RepeatMode::Off;
};

enum class BackendStatus {
  Ok,
  NothingPlaying,  // reachable, but no track loaded
  NoActiveDevice,  // command needs a device that is playing or paused
  NotAuthorized,   // token missing, expired or revoked
  NetworkError,    // WiFi down, server unreachable, timeout
  RateLimited,     // too many requests; try again later
  Unsupported,     // the device restricts the requested playback action
};
