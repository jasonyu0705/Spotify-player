#pragma once

#include <WiFiClientSecure.h>

#include "backend/PlayerBackend.h"
#include "backend/ArtTransfer.h"

// Talks to the Spotify Web API directly over HTTPS. Needs WiFi and a synced clock
// (see net::begin) and a refresh token from tools/get_refresh_token.py.
//
// Keeps one TLS connection to api.spotify.com open between calls; every call
// blocks for a round trip, so run it from BackendWorker, not the UI loop.
class SpotifyBackend : public PlayerBackend {
 public:
  SpotifyBackend(const char *clientId, const char *clientSecret, const char *refreshToken)
      : clientId_(clientId), clientSecret_(clientSecret), configuredRefreshToken_(refreshToken) {}

  const char *name() const override { return "SPOTIFY"; }
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
  // HTTP status codes, or a negative value for connection errors.
  int call(const char *method, const String &path, String &body);
  int request(const char *host, const char *method, const String &path, const String &payload,
              const String &authorization, String &body);
  int ensureAccessToken();
  int refreshAccessToken();
  BackendStatus command(const char *method, const String &path);
  BackendStatus statusFor(int code) const;
  bool rateLimited() const;

  const char *clientId_;
  const char *clientSecret_;
  const char *configuredRefreshToken_;
  String refreshToken_;

  WiFiClientSecure client_;
  String connectedHost_;

  String accessToken_;
  uint32_t tokenExpiresAt_ = 0;
  uint32_t authRetryAt_ = 0;     // back off after the refresh token is rejected
  uint32_t rateLimitedUntil_ = 0;
  String cachedArtUrl_;
  std::shared_ptr<const AlbumArt> cachedArt_;
  String attemptedArtUrl_;
  ArtRetry artRetry_;
};
