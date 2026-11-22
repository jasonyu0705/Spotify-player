#pragma once

#include <WiFiClient.h>

#include "backend/ArtTransfer.h"
#include "backend/PlayerBackend.h"

// Reads the Pi's cache over LAN HTTP. Spotify credentials and HTTPS stay on the Pi.
class PiBackend : public PlayerBackend {
 public:
  PiBackend(const char *serverUrl, const char *apiToken)
      : serverUrl_(serverUrl), apiToken_(apiToken) {}

  const char *name() const override { return "SPOTIFY / PI"; }
  bool begin() override;
  BackendStatus getNowPlaying(NowPlaying &out) override;
  bool loadAlbumArt(NowPlaying &out, bool (*cancel)(void *), void *context) override;
  BackendStatus play() override;
  BackendStatus pause() override;
  BackendStatus next() override;
  BackendStatus previous() override;
  BackendStatus setShuffle(bool on) override;
  BackendStatus setRepeat(RepeatMode mode) override;

 private:
  BackendStatus request(const char *method, const char *path, const String &payload, String &body);
  BackendStatus command(const String &payload);

  String serverUrl_;
  String apiToken_;
  bool configured_ = false;
  String cachedArtPath_;
  std::shared_ptr<const AlbumArt> cachedArt_;
  String attemptedArtPath_;
  ArtRetry artRetry_;
};
