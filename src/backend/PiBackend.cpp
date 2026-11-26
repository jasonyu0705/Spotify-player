#include "backend/PiBackend.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <new>

#include "net/Network.h"

namespace {
BackendStatus parseStatus(const char *status) {
  if (strcmp(status, "ok") == 0) return BackendStatus::Ok;
  if (strcmp(status, "nothing_playing") == 0) return BackendStatus::NothingPlaying;
  if (strcmp(status, "no_active_device") == 0) return BackendStatus::NoActiveDevice;
  if (strcmp(status, "not_authorized") == 0) return BackendStatus::NotAuthorized;
  if (strcmp(status, "rate_limited") == 0) return BackendStatus::RateLimited;
  if (strcmp(status, "unsupported") == 0) return BackendStatus::Unsupported;
  if (strcmp(status, "polling_disabled") == 0) return BackendStatus::PollingDisabled;
  return BackendStatus::NetworkError;
}
}  // namespace

bool PiBackend::begin() {
  idleInfo_.usesPi = true;
  while (serverUrl_.endsWith("/")) serverUrl_.remove(serverUrl_.length() - 1);
  configured_ = serverUrl_.startsWith("http://") && apiToken_.length() >= 24;
  if (serverUrl_.startsWith("http://")) {
    idleInfo_.piAddress = serverUrl_.substring(7);
    int port = idleInfo_.piAddress.indexOf(':');
    if (port >= 0) idleInfo_.piAddress.remove(port);
  }
  if (!configured_) Serial.println("Pi: set PI_SERVER_URL and PI_API_TOKEN in secrets.h");
  else Serial.printf("Pi: reading cached playback from %s\n", serverUrl_.c_str());
  return configured_;
}

BackendStatus PiBackend::request(const char *method, const char *path,
                                 const String &payload, String &body) {
  if (!configured_) return BackendStatus::NotAuthorized;
  if (!net::connected()) return BackendStatus::NetworkError;
  WiFiClient client;
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(1500);
  // State reads are local and immediate. Commands may wait for an in-flight
  // Spotify call, then refresh the cache before the Pi acknowledges them.
  http.setTimeout(strcmp(method, "GET") == 0 ? 3000 : 35000);
  if (!http.begin(client, serverUrl_ + path)) return BackendStatus::NetworkError;
  http.addHeader("Authorization", "Bearer " + apiToken_);
  if (payload.length()) http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(method, payload);
  const String peerAddress = client.remoteIP().toString();
  int length = http.getSize();
  if (code != 200 || length < 0 || length > 8192) {
    if (strcmp(path, "/state") == 0) {
      idleInfo_.piOnline = false;
      idleInfo_.piAuthError = code == 401;
    }
    Serial.printf("Pi: %s %s failed (HTTP %d, size %d)\n", method, path, code, length);
    http.end();
    return code == 401 ? BackendStatus::NotAuthorized : BackendStatus::NetworkError;
  }
  body = http.getString();
  http.end();
  if (body.length() != size_t(length)) {
    idleInfo_.piOnline = false;
    return BackendStatus::NetworkError;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body) || (doc["version"] | 0) != 1) {
    idleInfo_.piOnline = false;
    body = "";
    return BackendStatus::NetworkError;
  }
  if (strcmp(path, "/state") == 0) {
    if (!idleInfo_.piOnline) Serial.println("Pi: connected (valid state reply)");
    idleInfo_.piOnline = true;
    idleInfo_.piAuthError = false;
    idleInfo_.lastPiReplyMs = millis();
    if (peerAddress != "0.0.0.0") idleInfo_.piAddress = peerAddress;
  }
  return parseStatus(doc["status"] | "network_error");
}

BackendStatus PiBackend::getNowPlaying(NowPlaying &out) {
  String body;
  BackendStatus status = request("GET", "/state", "", body);
  JsonDocument doc;
  if (body.isEmpty() || deserializeJson(doc, body) || (doc["version"] | 0) != 1) return status;
  idleInfo_.pollingSupported = doc["polling"]["enabled"].is<bool>();
  idleInfo_.pollingEnabled = doc["polling"]["enabled"] | true;
  JsonObjectConst weather = doc["weather"];
  idleInfo_.weatherSupported = !weather.isNull();
  idleInfo_.weatherStatus = weather["status"] | "unavailable";
  idleInfo_.weatherAvailable = (idleInfo_.weatherStatus == "ok" || idleInfo_.weatherStatus == "stale") &&
                             weather["temperature"].is<float>();
  idleInfo_.weatherStale = idleInfo_.weatherStatus == "stale";
  idleInfo_.location = weather["location"] | "";
  idleInfo_.weatherReceivedAtMs = millis();
  if (idleInfo_.weatherAvailable) {
    idleInfo_.temperature = weather["temperature"].as<float>();
    idleInfo_.condition = weather["condition"] | "";
    idleInfo_.fahrenheit = strcmp(weather["temperature_unit"] | "celsius", "fahrenheit") == 0;
    idleInfo_.weatherCode = weather["weather_code"] | -1;
    idleInfo_.isDay = weather["is_day"] | true;
    idleInfo_.weatherAgeSeconds = weather["age_seconds"] | 0;
  }
  if (status != BackendStatus::Ok) return status;
  if (!doc["now_playing"].is<JsonObject>()) return BackendStatus::NetworkError;
  JsonObjectConst state = doc["now_playing"];
  out = NowPlaying{};
  out.trackId = state["track_id"] | "";
  out.title = state["title"] | "";
  out.artist = state["artist"] | "";
  out.album = state["album"] | "";
  out.deviceName = state["device_name"] | "";
  out.isPlaying = state["is_playing"] | false;
  out.shuffle = state["shuffle"] | false;
  out.repeat = repeatModeFromSpotify(state["repeat"] | "off");
  out.durationMs = state["duration_ms"] | 0;
  out.progressMs = state["progress_ms"] | 0;
  out.fetchedAtMs = millis();
  // Only relative cached-art paths are accepted; never download from Spotify.
  String path = state["art_path"] | "";
  if (path.startsWith("/art/") && path.endsWith(".jpg")) out.artUrl = path;
  if (out.artUrl == cachedArtPath_) out.artImage = cachedArt_;
  else {
    cachedArt_.reset();
    cachedArtPath_ = "";
  }
  return BackendStatus::Ok;
}

bool PiBackend::loadAlbumArt(NowPlaying &out, bool (*cancel)(void *), void *context) {
  if (out.artUrl.isEmpty() || out.artImage || !net::connected() || cancel(context)) return false;
  if (out.artUrl != attemptedArtPath_) {
    attemptedArtPath_ = out.artUrl;
    artRetry_.reset();
  }
  if (!artRetry_.ready(millis())) return false;
  auto failed = [&]() { artRetry_.failed(millis()); return false; };
  WiFiClient client;
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(1500);
  http.setTimeout(3000);
  if (!http.begin(client, serverUrl_ + out.artUrl)) return failed();
  http.addHeader("Authorization", "Bearer " + apiToken_);
  int code = http.GET();
  int length = http.getSize();
  if (cancel(context)) {
    http.end();
    artRetry_.cancelled(millis());
    return false;
  }
  if (code != 200 || length < 4 || length > 64 * 1024) {
    http.end();
    return failed();
  }
  auto image = std::make_shared<AlbumArt>();
  image->jpeg.reset(new (std::nothrow) uint8_t[length]);
  if (!image->jpeg) { http.end(); return failed(); }
  ArtReadResult result = readArtBody(*http.getStreamPtr(), image->jpeg.get(), size_t(length),
                                   []() { return millis(); }, []() { delay(1); },
                                   [&]() { return cancel(context); });
  http.end();
  if (result.status == ArtReadStatus::Cancelled) {
    artRetry_.cancelled(millis());
    return false;
  }
  if (result.status != ArtReadStatus::Complete || image->jpeg[0] != 0xff || image->jpeg[1] != 0xd8) return failed();
  image->length = result.received;
  cachedArtPath_ = out.artUrl;
  cachedArt_ = image;
  out.artImage = image;
  artRetry_.reset();
  Serial.printf("Pi: loaded cached artwork (%u bytes)\n", unsigned(image->length));
  return true;
}

BackendStatus PiBackend::command(const String &payload) {
  String body;
  return request("POST", "/command", payload, body);
}

BackendStatus PiBackend::play() { return command("{\"command\":\"play\"}"); }
BackendStatus PiBackend::pause() { return command("{\"command\":\"pause\"}"); }
BackendStatus PiBackend::next() { return command("{\"command\":\"next\"}"); }
BackendStatus PiBackend::previous() { return command("{\"command\":\"previous\"}"); }
BackendStatus PiBackend::setShuffle(bool on) {
  return command(String("{\"command\":\"shuffle\",\"value\":") + (on ? "true}" : "false}"));
}
BackendStatus PiBackend::setRepeat(RepeatMode mode) {
  return command(String("{\"command\":\"repeat\",\"value\":\"") + spotifyRepeatState(mode) + "\"}");
}

BackendStatus PiBackend::setPolling(bool enabled) {
  String body;
  return request("POST", "/polling", String("{\"enabled\":") + (enabled ? "true}" : "false}"), body);
}
