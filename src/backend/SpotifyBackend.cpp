#include "backend/SpotifyBackend.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <base64.h>
#include <new>

#include "backend/SpotifyCerts.h"
#include "backend/HttpResponse.h"
#include "net/Network.h"

namespace {

constexpr const char *API_HOST = "api.spotify.com";
constexpr const char *ACCOUNTS_HOST = "accounts.spotify.com";
constexpr const char *PLACEHOLDER_TOKEN = "your-refresh-token";

constexpr int ERR_NOT_CONNECTED = -100;   // no WiFi
constexpr int ERR_AUTH_BACKOFF = -101;    // refresh token was rejected recently
constexpr int ERR_RATE_LIMITED = -102;    // waiting out a 429
constexpr int ERR_AUTH_FAILED = -103;     // refresh token missing or rejected
constexpr uint32_t AUTH_RETRY_MS = 30000;

bool timeReached(uint32_t deadline) { return int32_t(millis() - deadline) >= 0; }

bool isSuccess(int code) { return code >= 200 && code < 300; }

String urlEncode(const char *s) {
  String out;
  for (; *s; s++) {
    char c = *s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", uint8_t(c));
      out += buf;
    }
  }
  return out;
}

// Picks the image closest to 300px wide (Spotify sends 640, 300 and 64).
String pickImage(JsonArrayConst images) {
  String best;
  int bestDiff = INT32_MAX;
  for (JsonObjectConst img : images) {
    int diff = abs((img["width"] | 0) - 300);
    if (diff < bestDiff) {
      bestDiff = diff;
      best = img["url"] | "";
    }
  }
  return best;
}

}  // namespace

bool SpotifyBackend::begin() {
  client_.setCACert(SPOTIFY_ROOT_CAS);
  client_.setTimeout(8);  // seconds
  client_.setHandshakeTimeout(5);  // don't wait minutes when an idle connection expires

  // Spotify sometimes issues a new refresh token on refresh. That gets saved to
  // flash, and used until you put a different token in secrets.h.
  Preferences prefs;
  prefs.begin("spotify", false);  // read-write so a missing namespace is created, not an error
  bool sameSeed = prefs.getString("seed", "") == configuredRefreshToken_;
  refreshToken_ = sameSeed ? prefs.getString("refresh", configuredRefreshToken_)
                           : String(configuredRefreshToken_);
  prefs.end();

  if (refreshToken_.isEmpty() || refreshToken_ == PLACEHOLDER_TOKEN) {
    Serial.println("Spotify: no refresh token in secrets.h; run tools/get_refresh_token.py");
  }
  return true;
}

// --- HTTP ------------------------------------------------------------------

int SpotifyBackend::request(const char *host, const char *method, const String &path,
                            const String &payload, const String &authorization, String &body) {
  // HTTPClient reuses an open connection without checking which host it's to.
  if (connectedHost_ != host) {
    client_.stop();
    connectedHost_ = host;
  }

  int code = 0;
  for (int attempt = 0; attempt < 2; attempt++) {
    HTTPClient http;
    http.setReuse(true);
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    if (!http.begin(client_, String("https://") + host + path)) return HTTPC_ERROR_CONNECTION_REFUSED;

    http.addHeader("Authorization", authorization);
    if (payload.length()) {
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    } else if (strcmp(method, "GET") != 0) {
      http.addHeader("Content-Length", "0");  // Spotify rejects bodiless PUT/POST without it
    }
    const char *headerKeys[] = {"Retry-After"};
    http.collectHeaders(headerKeys, 1);

    code = http.sendRequest(method, payload);
    if (code == 429) {
      int seconds = max(1L, http.header("Retry-After").toInt());
      rateLimitedUntil_ = millis() + seconds * 1000UL;
      Serial.printf("Spotify: rate limited for %d s\n", seconds);
    }
    body = readHttpResponseBody(http, method, code);
    http.end();

    if (code > 0) break;
    // A kept-alive connection the server already closed fails on first use; reconnect once.
    Serial.printf("Spotify: %s %s failed (%s), retrying\n", method, path.c_str(),
                  HTTPClient::errorToString(code).c_str());
    client_.stop();
  }
  return code;
}

int SpotifyBackend::refreshAccessToken() {
  if (refreshToken_.isEmpty() || refreshToken_ == PLACEHOLDER_TOKEN) return ERR_AUTH_FAILED;

  String auth = "Basic " + base64::encode(String(clientId_) + ":" + clientSecret_);
  String payload = "grant_type=refresh_token&refresh_token=" + urlEncode(refreshToken_.c_str());
  String body;
  int code = request(ACCOUNTS_HOST, "POST", "/api/token", payload, auth, body);
  client_.stop();  // next call goes to api.spotify.com
  connectedHost_ = "";

  if (!isSuccess(code)) {
    Serial.printf("Spotify: token refresh failed (%d): %s\n", code, body.substring(0, 200).c_str());
    if (code == 400 || code == 401) {  // invalid_grant: token revoked, or wrong client secret
      authRetryAt_ = millis() + AUTH_RETRY_MS;
      return ERR_AUTH_FAILED;
    }
    return code;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) return HTTPC_ERROR_NO_STREAM;
  accessToken_ = doc["access_token"] | "";
  uint32_t expiresIn = doc["expires_in"] | 3600;
  tokenExpiresAt_ = millis() + (expiresIn > 120 ? expiresIn - 60 : expiresIn) * 1000UL;

  const char *rotated = doc["refresh_token"] | "";
  if (*rotated && refreshToken_ != rotated) {
    refreshToken_ = rotated;
    Preferences prefs;
    prefs.begin("spotify", false);
    prefs.putString("seed", configuredRefreshToken_);
    prefs.putString("refresh", refreshToken_);
    prefs.end();
    Serial.println("Spotify: saved new refresh token");
  }
  Serial.printf("Spotify: access token refreshed (valid %lu s)\n", (unsigned long)expiresIn);
  return accessToken_.isEmpty() ? ERR_AUTH_FAILED : 200;
}

int SpotifyBackend::ensureAccessToken() {
  if (!accessToken_.isEmpty() && !timeReached(tokenExpiresAt_)) return 200;
  if (authRetryAt_ && !timeReached(authRetryAt_)) return ERR_AUTH_BACKOFF;
  authRetryAt_ = 0;
  accessToken_ = "";
  return refreshAccessToken();
}

bool SpotifyBackend::rateLimited() const {
  return rateLimitedUntil_ && !timeReached(rateLimitedUntil_);
}

int SpotifyBackend::call(const char *method, const String &path, String &body) {
  if (!net::connected()) return ERR_NOT_CONNECTED;
  if (rateLimited()) return ERR_RATE_LIMITED;

  int tokenCode = ensureAccessToken();
  if (!isSuccess(tokenCode)) return tokenCode;

  int code = request(API_HOST, method, path, "", "Bearer " + accessToken_, body);
  if (code == 401) {  // token revoked or expired early: refresh once and retry
    accessToken_ = "";
    tokenCode = ensureAccessToken();
    if (!isSuccess(tokenCode)) return tokenCode;
    code = request(API_HOST, method, path, "", "Bearer " + accessToken_, body);
  }
  if (!isSuccess(code) && code > 0) {
    Serial.printf("Spotify: %s %s -> %d %s\n", method, path.c_str(), code,
                  body.substring(0, 200).c_str());
  }
  return code;
}

BackendStatus SpotifyBackend::statusFor(int code) const {
  if (isSuccess(code)) return BackendStatus::Ok;
  switch (code) {
    case 401:
    case ERR_AUTH_FAILED:
    case ERR_AUTH_BACKOFF: return BackendStatus::NotAuthorized;
    case 400:
    case 403: return BackendStatus::Unsupported;  // Premium required, or a restricted action
    case 404: return BackendStatus::NoActiveDevice;
    case 429:
    case ERR_RATE_LIMITED: return BackendStatus::RateLimited;
    default: return BackendStatus::NetworkError;  // connection errors, timeouts, 5xx
  }
}

// --- playback state --------------------------------------------------------

BackendStatus SpotifyBackend::getNowPlaying(NowPlaying &out) {
  String body;
  int code = call("GET", "/v1/me/player?additional_types=episode&market=from_token", body);
  static int lastCode = 0;
  if (code != lastCode) {
    Serial.printf("Spotify: playback poll -> %d (%u bytes)\n", code, unsigned(body.length()));
    lastCode = code;
  }
  if (code == 204 || (code == 200 && body.isEmpty())) return BackendStatus::NothingPlaying;
  if (code != 200) return statusFor(code);
  out.artImage.reset();

  // The full response is several KB; keep only what the screen uses.
  JsonDocument filter;
  filter["is_playing"] = true;
  filter["progress_ms"] = true;
  filter["shuffle_state"] = true;
  filter["repeat_state"] = true;
  filter["currently_playing_type"] = true;
  filter["device"]["name"] = true;
  JsonObject item = filter["item"].to<JsonObject>();
  item["id"] = true;
  item["uri"] = true;
  item["name"] = true;
  item["duration_ms"] = true;
  item["artists"][0]["name"] = true;
  item["album"]["name"] = true;
  item["album"]["images"][0]["url"] = true;
  item["album"]["images"][0]["width"] = true;
  item["show"]["name"] = true;            // podcasts
  item["images"][0]["url"] = true;
  item["images"][0]["width"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("Spotify: couldn't parse player state: %s\n", err.c_str());
    return BackendStatus::NetworkError;
  }

  JsonObjectConst device = doc["device"];
  out.deviceName = device["name"] | "";
  out.isPlaying = doc["is_playing"] | false;
  out.isLastPlayed = false;
  out.shuffle = doc["shuffle_state"] | false;
  out.repeat = repeatModeFromSpotify(doc["repeat_state"] | "off");
  static int lastRepeat = -1;
  if (lastRepeat != int(out.repeat)) {
    Serial.printf("Spotify: repeat state %s\n", spotifyRepeatState(out.repeat));
    lastRepeat = int(out.repeat);
  }
  out.progressMs = doc["progress_ms"] | 0;
  out.fetchedAtMs = millis();

  JsonObjectConst track = doc["item"];
  if (track.isNull()) {
    // Ads (and some local files) come with no item.
    if (String(doc["currently_playing_type"] | "") != "ad") return BackendStatus::NothingPlaying;
    out.trackId = "ad";
    out.title = "Advertisement";
    out.artist = "Spotify";
    out.album = "";
    out.artUrl = "";
    out.durationMs = max<uint32_t>(out.progressMs, 30000);
    return BackendStatus::Ok;
  }

  out.trackId = track["id"] | (track["uri"] | (track["name"] | "unknown"));
  out.title = track["name"] | "";
  out.durationMs = track["duration_ms"] | 0;

  if (!track["show"].isNull()) {  // podcast episode
    out.artist = track["show"]["name"] | "";
    out.album = out.artist;
    out.artUrl = pickImage(track["images"]);
  } else {
    String artists;
    for (JsonObjectConst a : track["artists"].as<JsonArrayConst>()) {
      if (artists.length()) artists += ", ";
      artists += a["name"] | "";
    }
    out.artist = artists;
    out.album = track["album"]["name"] | "";
    out.artUrl = pickImage(track["album"]["images"]);
  }
  if (out.artUrl == cachedArtUrl_) {
    out.artImage = cachedArt_;
  } else {
    // The UI owns its own shared reference until it consumes this update. Drop
    // the backend's obsolete cover before the next TLS handshake/allocation.
    cachedArt_.reset();
    cachedArtUrl_ = "";
  }
  return BackendStatus::Ok;
}

bool SpotifyBackend::loadAlbumArt(NowPlaying &out, bool (*cancel)(void *), void *context) {
  if (out.artUrl.isEmpty() || out.artImage || !net::connected() || cancel(context)) return false;
  if (out.artUrl == cachedArtUrl_ && cachedArt_) {
    out.artImage = cachedArt_;
    return true;
  }
  if (out.artUrl != attemptedArtUrl_) {
    attemptedArtUrl_ = out.artUrl;
    artRetry_.reset();
  }
  if (!artRetry_.ready(millis())) return false;
  auto failed = [&]() {
    artRetry_.failed(millis());
    Serial.printf("Album art: retry in %lu ms\n", (unsigned long)(artRetry_.retryAt - millis()));
    return false;
  };

  // One TLS connection at a time keeps this no-PSRAM board within its proven
  // memory budget. Publish metadata first and yield the transfer for commands.
  client_.stop();
  connectedHost_ = "";
  WiFiClientSecure imageClient;
  imageClient.setCACert(SPOTIFY_ROOT_CAS);
  imageClient.setTimeout(4);
  imageClient.setHandshakeTimeout(5);
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  if (!out.artUrl.startsWith("https://") || !http.begin(imageClient, out.artUrl)) {
    Serial.println("Album art: invalid URL or HTTP setup failed");
    return failed();
  }
  uint32_t started = millis();
  int code = http.GET();
  int length = http.getSize();
  if (cancel(context)) {
    http.end();
    artRetry_.cancelled(millis());
    return false;
  }
  constexpr size_t MAX_JPEG_BYTES = 64 * 1024;
  if (code != 200 || length < 4 || size_t(length) > MAX_JPEG_BYTES) {
    Serial.printf("Album art: download skipped (HTTP %d, size %d)\n", code, length);
    if (code < 0) {
      char tlsError[128] = {};
      imageClient.lastError(tlsError, sizeof(tlsError));
      Serial.printf("Album art: %s; TLS %s\n", HTTPClient::errorToString(code).c_str(), tlsError);
    }
    http.end();
    return failed();
  }
  auto image = std::make_shared<AlbumArt>();
  image->jpeg.reset(new (std::nothrow) uint8_t[length]);
  if (!image->jpeg) {
    Serial.printf("Album art: cannot allocate %d bytes (heap %u, largest block %u)\n",
                  length, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    http.end();
    return failed();
  }
  WiFiClient *stream = http.getStreamPtr();
  const uint32_t bodyStarted = millis();
  ArtReadResult result = readArtBody(*stream, image->jpeg.get(), size_t(length),
                                   []() { return millis(); }, []() { delay(1); },
                                   [&]() { return cancel(context); });
  const size_t received = result.received;
  http.end();
  if (result.status == ArtReadStatus::Cancelled) {
    artRetry_.cancelled(millis());
    Serial.println("Album art: yielding transfer to playback command");
    return false;
  }
  if (result.status != ArtReadStatus::Complete || image->jpeg[0] != 0xff || image->jpeg[1] != 0xd8) {
    Serial.printf("Album art: incomplete or invalid JPEG (%u/%d bytes, reason %u, body %lu ms)\n",
                  unsigned(received), length, unsigned(result.status),
                  (unsigned long)(millis() - bodyStarted));
    return failed();
  }
  image->length = received;
  cachedArtUrl_ = out.artUrl;
  cachedArt_ = image;
  out.artImage = image;
  artRetry_.reset();
  Serial.printf("Album art: downloaded %u bytes in %lu ms\n", unsigned(received),
                (unsigned long)(millis() - started));
  return true;
}

// --- commands --------------------------------------------------------------

BackendStatus SpotifyBackend::command(const char *method, const String &path) {
  String body;
  return statusFor(call(method, path, body));
}

BackendStatus SpotifyBackend::play() { return command("PUT", "/v1/me/player/play"); }
BackendStatus SpotifyBackend::pause() { return command("PUT", "/v1/me/player/pause"); }
BackendStatus SpotifyBackend::next() { return command("POST", "/v1/me/player/next"); }
BackendStatus SpotifyBackend::previous() { return command("POST", "/v1/me/player/previous"); }

BackendStatus SpotifyBackend::setShuffle(bool on) {
  return command("PUT", String("/v1/me/player/shuffle?state=") + (on ? "true" : "false"));
}

BackendStatus SpotifyBackend::setRepeat(RepeatMode mode) {
  Serial.printf("Spotify: setting repeat %s\n", spotifyRepeatState(mode));
  return command("PUT", String("/v1/me/player/repeat?state=") + spotifyRepeatState(mode));
}
