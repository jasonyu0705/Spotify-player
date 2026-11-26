#pragma once

#include "backend/NowPlaying.h"
#include "backend/IdleInfo.h"

// Playback interface used by the worker, implemented by SpotifyBackend.
class PlayerBackend {
 public:
  virtual ~PlayerBackend() = default;

  virtual const char *name() const = 0;
  virtual bool begin() = 0;

  virtual BackendStatus getNowPlaying(NowPlaying &out) = 0;
  virtual IdleInfo idleInfo() const { return {}; }
  virtual bool loadAlbumArt(NowPlaying &out, bool (*cancel)(void *), void *context) { return false; }

  virtual BackendStatus play() = 0;
  virtual BackendStatus pause() = 0;
  virtual BackendStatus next() = 0;
  virtual BackendStatus previous() = 0;
  virtual BackendStatus setShuffle(bool on) = 0;
  virtual BackendStatus setRepeat(RepeatMode mode) = 0;
};
