#include "ui/PlayerUI.h"

#include "ui/Icons.h"
#include "ui/AlbumArtRenderer.h"
#include "ui/Text.h"
#include "ui/Theme.h"
#include "net/Network.h"
#include <time.h>

using namespace layout;
using namespace theme;

namespace {

constexpr uint32_t NOTICE_MS = 2500;
constexpr uint32_t PROGRESS_JUMP_MS = 2000;  // larger gap than this means a seek, not drift
constexpr uint32_t PI_REPLY_FRESH_MS = 45000;  // longer than the maximum command wait
constexpr int16_t UPDATES_W = 96;
constexpr int16_t UPDATES_X = W - MARGIN - UPDATES_W;

bool piIsOnline(const IdleInfo &info) {
  return net::connected() && info.piOnline && uint32_t(millis() - info.lastPiReplyMs) < PI_REPLY_FRESH_MS;
}

// A small, code-drawn symbol avoids image downloads and extra bitmap storage.
void weatherIcon(TFT_eSPI &tft, int x, int y, int code, bool isDay) {
  const uint16_t sun = rgb565(245, 194, 75);
  const uint16_t cloud = rgb565(168, 186, 204);
  const uint16_t rain = rgb565(90, 174, 239);
  if (code == 0 || code == 1) {
    tft.fillCircle(x, y, 5, isDay ? sun : cloud);
    if (!isDay) tft.fillCircle(x + 3, y - 2, 4, BG);
    else {
      tft.drawFastVLine(x, y - 10, 3, sun);
      tft.drawFastVLine(x, y + 8, 3, sun);
      tft.drawFastHLine(x - 10, y, 3, sun);
      tft.drawFastHLine(x + 8, y, 3, sun);
      tft.drawLine(x - 7, y - 7, x - 5, y - 5, sun);
      tft.drawLine(x + 7, y - 7, x + 5, y - 5, sun);
      tft.drawLine(x - 7, y + 7, x - 5, y + 5, sun);
      tft.drawLine(x + 7, y + 7, x + 5, y + 5, sun);
    }
    return;
  }
  if (code == 2) tft.fillCircle(x + 5, y - 5, 5, sun);
  tft.fillCircle(x - 5, y, 4, cloud);
  tft.fillCircle(x, y - 3, 5, cloud);
  tft.fillCircle(x + 5, y, 4, cloud);
  tft.fillRoundRect(x - 8, y, 17, 5, 2, cloud);
  if (code == 45 || code == 48) {
    tft.drawFastHLine(x - 8, y + 7, 17, cloud);
    tft.drawFastHLine(x - 6, y + 10, 13, cloud);
  } else if (code >= 95) {
    tft.drawLine(x + 1, y + 5, x - 3, y + 9, sun);
    tft.drawLine(x - 3, y + 9, x + 2, y + 9, sun);
    tft.drawLine(x + 2, y + 9, x - 1, y + 12, sun);
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
    tft.fillCircle(x - 4, y + 9, 1, cloud);
    tft.fillCircle(x + 4, y + 9, 1, cloud);
  } else if (code >= 51) {
    tft.drawLine(x - 4, y + 7, x - 6, y + 11, rain);
    tft.drawLine(x + 3, y + 7, x + 1, y + 11, rain);
  }
}

// Centers of the three button columns in the right-hand panel.
int16_t slotCenter(int slot) { return PANEL_X0 + slot * SLOT_W + SLOT_W / 2; }

uint32_t fnv1a(const String &s) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < s.length(); i++) h = (h ^ uint8_t(s[i])) * 16777619u;
  return h;
}

uint16_t hsv565(uint16_t hue, float sat, float val) {
  float c = val * sat;
  float hp = (hue % 360) / 60.0f;
  float x = c * (1 - fabsf(fmodf(hp, 2) - 1));
  float r = 0, g = 0, b = 0;
  if (hp < 1) { r = c; g = x; }
  else if (hp < 2) { r = x; g = c; }
  else if (hp < 3) { g = c; b = x; }
  else if (hp < 4) { g = x; b = c; }
  else if (hp < 5) { r = x; b = c; }
  else { r = c; b = x; }
  float m = val - c;
  return rgb565((r + m) * 255, (g + m) * 255, (b + m) * 255);
}

const char *statusDetail(BackendStatus status) {
  switch (status) {
    case BackendStatus::NothingPlaying: return "Start music on any Spotify device";
    case BackendStatus::NoActiveDevice: return "Open Spotify on your phone or computer";
    case BackendStatus::NotAuthorized: return "Get a new refresh token";
    case BackendStatus::NetworkError: return "Check WiFi and the server address";
    case BackendStatus::RateLimited: return "Waiting before trying again";
    case BackendStatus::Unsupported: return "This device can't do that";
    case BackendStatus::PollingDisabled: return "Tap Spotify OFF to enable Spotify updates";
    default: return "";
  }
}

}  // namespace

const char *statusTitle(BackendStatus status) {
  switch (status) {
    case BackendStatus::Ok: return "OK";
    case BackendStatus::NothingPlaying: return "Nothing is playing";
    case BackendStatus::NoActiveDevice: return "No active device";
    case BackendStatus::NotAuthorized: return "Spotify not linked";
    case BackendStatus::NetworkError: return "Can't connect";
    case BackendStatus::RateLimited: return "Too many requests";
    case BackendStatus::Unsupported: return "Not supported";
    case BackendStatus::PollingDisabled: return "Spotify updates off";
  }
  return "Unknown error";
}

// --- screens ---------------------------------------------------------------

void PlayerUI::begin(const char *backendName) {
  backendName_ = backendName;
  tft_.fillScreen(BG);
}

void PlayerUI::setVisible(bool visible) {
  visible_ = visible;
  if (visible) {
    mode_ = Mode::None;  // force a full redraw after leaving the photo screen
    noticeUntil_ = 0;
  }
}

void PlayerUI::setBrightness(uint8_t percent) {
  if (brightness_ == percent) return;
  brightness_ = percent;
  if (visible_ && mode_ != Mode::None) drawBrightnessButton();
}

void PlayerUI::setIdleInfo(const IdleInfo &info) {
  const bool changed = idleInfo_.pollingSupported != info.pollingSupported ||
                       idleInfo_.pollingEnabled != info.pollingEnabled;
  idleInfo_ = info;
  if (changed && visible_ && mode_ != Mode::None) drawStatusBar();
}

void PlayerUI::showMessage(const char *title, const char *detail) {
  if (!visible_) return;
  mode_ = Mode::Message;
  tft_.fillScreen(BG);
  drawStatusBar();

  tft_.setTextDatum(MC_DATUM);
  tft_.setTextPadding(0);
  tft_.setTextColor(TEXT_PRIMARY, BG);
  tft_.drawString(title, W / 2, H / 2 - 16, 4);
  tft_.setTextColor(TEXT_SECONDARY, BG);
  tft_.drawString(detail, W / 2, H / 2 + 18, 2);
  drawBrightnessButton();
}

void PlayerUI::showStatus(BackendStatus status) {
  if (!visible_) return;
  if (mode_ == Mode::Message && messageStatus_ == status) return;
  messageStatus_ = status;
  showMessage(statusTitle(status), statusDetail(status));
}

void PlayerUI::showClock() {
  if (!visible_) return;
  if (mode_ == Mode::Clock) return;
  mode_ = Mode::Clock;
  Serial.println("Display: idle clock");
  tft_.fillScreen(BG);
  drawStatusBar();
  drawClock(true);
  drawBrightnessButton();
}

void PlayerUI::drawClock(bool force) {
  // Never call getLocalTime() here: it can wait seconds for NTP and block touch.
  const time_t now = time(nullptr);
  const bool synced = now >= 1700000000;
  const int64_t minute = synced ? int64_t(now) / 60 : -1;
  if (!force && minute == lastClockMinute_) {
    drawIdleDetails(false);
    return;
  }
  lastClockMinute_ = minute;
  char clockText[6] = "--:--";
  const char *period = "";
  char dateText[40] = "Syncing time...";
  if (synced) {
    struct tm local;
    localtime_r(&now, &local);
    int hour = local.tm_hour % 12;
    if (hour == 0) hour = 12;
    snprintf(clockText, sizeof(clockText), "%d:%02d", hour, local.tm_min);
    period = local.tm_hour < 12 ? "AM" : "PM";
    strftime(dateText, sizeof(dateText), "%a, %b %d", &local);
  }
  Serial.printf("Clock: %s %s (%s)\n", clockText, period, dateText);
  tft_.fillRect(0, 48, W, 170, BG);
  tft_.setTextPadding(0);
  tft_.setTextDatum(MC_DATUM);
  tft_.setTextColor(TEXT_PRIMARY, BG);
  tft_.setTextSize(2);
  tft_.drawString(clockText, W / 2, 112, 7);
  tft_.setTextSize(1);
  tft_.setTextColor(TEXT_SECONDARY, BG);
  tft_.drawString(period, W / 2, 173, 2);
  tft_.drawString(dateText, W / 2, 199, 4);
  drawIdleDetails(force);
}

void PlayerUI::drawIdleDetails(bool force) {
  const uint32_t now = millis();
  if (!force && uint32_t(now - lastIdleCheckMs_) < 500) return;
  lastIdleCheckMs_ = now;
  const bool wifi = net::connected();
  const bool pi = piIsOnline(idleInfo_);
  const int connection = int(wifi) | (int(pi) << 1) | (int(idleInfo_.piAuthError) << 2) |
                         (int(idleInfo_.usesPi) << 3);
  if (force || connection != lastConnectionState_) {
    lastConnectionState_ = connection;
    drawStatusBar();
  }

  String text;
  String location;
  int code = -1;
  bool day = true;
  if (!idleInfo_.usesPi) text = "Weather available in Pi mode";
  else if (!idleInfo_.weatherSupported) text = pi ? "Weather: update Pi bridge" : "Weather: waiting for Pi";
  else if (!idleInfo_.weatherAvailable) {
    text = idleInfo_.weatherStatus == "loading" ? "Weather: loading..." :
           idleInfo_.weatherStatus == "not_configured" ? "Weather not configured" : "Weather unavailable";
    location = idleInfo_.location;
  } else {
    char temperature[20];
    snprintf(temperature, sizeof(temperature), "%.0f %c", idleInfo_.temperature, idleInfo_.fahrenheit ? 'F' : 'C');
    text = String(temperature) + "  " + idleInfo_.condition;
    if (idleInfo_.weatherStale || !pi) text += " (cached)";
    location = idleInfo_.location;
    code = idleInfo_.weatherCode;
    day = idleInfo_.isDay;
  }
  const bool weatherChanged = force || text != lastWeatherText_ || location != lastWeatherLocation_ ||
                              code != lastWeatherCode_ || day != lastWeatherDay_;
  if (weatherChanged) {
    lastWeatherText_ = text;
    lastWeatherLocation_ = location;
    lastWeatherCode_ = code;
    lastWeatherDay_ = day;
    tft_.fillRect(0, 220, W, 56, BG);
    tft_.setTextSize(1);
    tft_.setTextPadding(0);
    const String fitted = fitText(tft_, toDisplayText(text), 4, W - 2 * MARGIN - 28);
    const int width = tft_.textWidth(fitted, 4) + (code >= 0 ? 28 : 0);
    const int left = (W - width) / 2;
    if (code >= 0) weatherIcon(tft_, left + 10, 237, code, day);
    tft_.setTextDatum(ML_DATUM);
    tft_.setTextColor(TEXT_SECONDARY, BG);
    tft_.drawString(fitted, left + (code >= 0 ? 28 : 0), 237, 4);
    tft_.setTextDatum(MC_DATUM);
    tft_.setTextColor(TEXT_TERTIARY, BG);
    tft_.drawString(fitText(tft_, toDisplayText(location), 2, W - 2 * MARGIN), W / 2, 263, 2);
  }
  const String espAddress = wifi ? net::localAddress() : String("Offline");
  if (weatherChanged || espAddress != lastEspAddress_ || idleInfo_.piAddress != lastPiAddress_) {
    lastEspAddress_ = espAddress;
    lastPiAddress_ = idleInfo_.piAddress;
    tft_.fillRect(MARGIN, 274, 300, 38, BG);
    tft_.setTextSize(1);
    tft_.setTextPadding(0);
    tft_.setTextDatum(ML_DATUM);
    tft_.setTextColor(TEXT_TERTIARY, BG);
    tft_.drawString(fitText(tft_, String("ESP: ") + espAddress, 2, 300), MARGIN, 282, 2);
    if (idleInfo_.usesPi && !idleInfo_.piAddress.isEmpty()) {
      tft_.drawString(fitText(tft_, String("Pi: ") + idleInfo_.piAddress, 2, 300), MARGIN, 300, 2);
    }
  }
}

void PlayerUI::update(const NowPlaying &np) {
  if (!visible_) return;
  if (mode_ != Mode::Player) {
    shown_ = np;
    redrawAll();
    return;
  }

  const NowPlaying old = shown_;
  const uint32_t expected = currentProgress();
  shown_ = np;

  if (np.trackId != old.trackId || np.artUrl != old.artUrl || np.artImage != old.artImage) {
    drawArt();
  }
  if (np.trackId != old.trackId) {
    drawInfo();
    drawProgress(true);
  } else {
    if (np.title != old.title || np.artist != old.artist) drawInfo();
    int32_t drift = int32_t(np.progressMs) - int32_t(expected);
    if (np.durationMs != old.durationMs || abs(drift) > int32_t(PROGRESS_JUMP_MS)) drawProgress(true);
  }

  if (np.isPlaying != old.isPlaying) drawPlayButton();
  if (np.shuffle != old.shuffle) drawShuffleButton();
  if (np.repeat != old.repeat) drawRepeatButton();
  if (np.deviceName != old.deviceName || np.isLastPlayed != old.isLastPlayed ||
      np.isPlaying != old.isPlaying) drawStatusBar();
}

void PlayerUI::redrawAll() {
  mode_ = Mode::Player;
  tft_.fillScreen(BG);
  drawStatusBar();
  drawArt();
  drawInfo();
  drawProgress(true);
  drawControls();
}

void PlayerUI::tick() {
  if (!visible_) return;
  uint32_t now = millis();
  if (noticeUntil_ && now > noticeUntil_) {
    noticeUntil_ = 0;
    drawStatusBar();
  }
  if (mode_ == Mode::Clock) drawClock(false);
  if (mode_ != Mode::Player) return;
  drawProgress(false);
}

void PlayerUI::showNotice(const char *text, bool error) {
  if (!visible_) return;
  notice_ = text;
  noticeError_ = error;
  noticeUntil_ = millis() + NOTICE_MS;
  drawStatusBar();
}

// --- regions ---------------------------------------------------------------

void PlayerUI::drawStatusBar() {
  tft_.fillRect(0, 0, W, STATUS_H, BG);
  tft_.setTextPadding(0);
  tft_.setTextDatum(ML_DATUM);

  if (noticeUntil_) {
    tft_.setTextColor(noticeError_ ? ERROR_RED : TEXT_SECONDARY, BG);
    tft_.drawString(fitText(tft_, notice_, 2, W - 2 * MARGIN), MARGIN, STATUS_H / 2, 2);
    return;
  }

  if (mode_ == Mode::Player) {
    tft_.fillSmoothCircle(MARGIN + 3, STATUS_H / 2, 3,
                         shown_.isLastPlayed ? TEXT_TERTIARY : ACCENT, BG);
    tft_.setTextColor(TEXT_SECONDARY, BG);
    String prefix = shown_.isLastPlayed ? "Last played" : shown_.isPlaying ? "Playing" : "Paused";
    String label = prefix + (shown_.deviceName.isEmpty() ? "" : " on " + toDisplayText(shown_.deviceName));
    const int deviceWidth = idleInfo_.pollingSupported
                                ? UPDATES_X - 12 - (MARGIN + 12)
                                : W - 120;
    String device = fitText(tft_, label, 2, deviceWidth);
    tft_.drawString(device, MARGIN + 12, STATUS_H / 2, 2);
  } else if (mode_ == Mode::Clock) {
    const bool wifi = net::connected();
    tft_.fillCircle(MARGIN + 3, STATUS_H / 2, 3, wifi ? ACCENT : ERROR_RED);
    tft_.setTextColor(TEXT_SECONDARY, BG);
    tft_.drawString(wifi ? "Wi-Fi online" : "Wi-Fi offline", MARGIN + 12, STATUS_H / 2, 2);
    if (idleInfo_.usesPi) {
      const bool pi = piIsOnline(idleInfo_);
      tft_.fillCircle(161, STATUS_H / 2, 3, pi ? ACCENT : ERROR_RED);
      tft_.drawString(pi ? "Pi online" : idleInfo_.piAuthError ? "Pi auth error" : "Pi offline", 173, STATUS_H / 2, 2);
    }
  }
  if (idleInfo_.usesPi && idleInfo_.pollingSupported) {
    const uint16_t color = idleInfo_.pollingEnabled ? ACCENT : ERROR_RED;
    tft_.fillRoundRect(UPDATES_X, 2, UPDATES_W, STATUS_H - 4, 5, color);
    tft_.setTextDatum(MC_DATUM);
    tft_.setTextColor(BG, color);
    tft_.drawString(idleInfo_.pollingEnabled ? "Spotify ON" : "Spotify OFF",
                    UPDATES_X + UPDATES_W / 2, STATUS_H / 2, 2);
  } else {
    tft_.setTextDatum(MR_DATUM);
    tft_.setTextColor(ICON_DISABLED, BG);
    tft_.drawString(backendName_, W - MARGIN, STATUS_H / 2, 2);
  }
}

void PlayerUI::drawArt() {
  if (shown_.artImage && drawAlbumArt(tft_, *shown_.artImage)) return;
  // Keep a placeholder while downloading, or when a track has no usable cover.
  uint16_t hue = fnv1a(shown_.trackId) % 360;
  uint16_t top = hsv565(hue, 0.55f, 0.85f);
  uint16_t bottom = hsv565(hue + 40, 0.75f, 0.30f);
  tft_.fillRectVGradient(ART_X, ART_Y, ART_SIZE, ART_SIZE, top, bottom);

  const int16_t cx = ART_X + ART_SIZE / 2, cy = ART_Y + ART_SIZE / 2;
  const int16_t r = ART_SIZE * 36 / 100;
  tft_.fillCircle(cx, cy, r, VINYL);
  for (int16_t g = r * 45 / 100; g < r - 6; g += 12) tft_.drawCircle(cx, cy, g, VINYL_GROOVE);
  tft_.fillCircle(cx, cy, r * 32 / 100, top);
  tft_.fillCircle(cx, cy, 5, VINYL);
}

void PlayerUI::drawInfo() {
  tft_.fillRect(PANEL_X0 - 8, INFO_Y, W - PANEL_X0 + 8, INFO_H, BG);
  tft_.setTextPadding(0);
  tft_.setTextDatum(TL_DATUM);

  String lines[2];
  int n = wrapText(tft_, toDisplayText(shown_.title), 4, PANEL_W, lines, 2);
  int16_t y = INFO_Y + 4;
  tft_.setTextColor(TEXT_PRIMARY, BG);
  for (int i = 0; i < n; i++, y += TITLE_LINE_H) tft_.drawString(lines[i], PANEL_X0, y, 4);

  y += 6;
  tft_.setTextColor(TEXT_SECONDARY, BG);
  tft_.drawString(fitText(tft_, toDisplayText(shown_.artist), 2, PANEL_W), PANEL_X0, y, 2);
  y += 20;
  tft_.setTextColor(TEXT_TERTIARY, BG);
  tft_.drawString(fitText(tft_, toDisplayText(shown_.album), 2, PANEL_W), PANEL_X0, y, 2);
}

uint32_t PlayerUI::currentProgress() const {
  uint32_t pos = shown_.progressMs;
  if (shown_.isPlaying) pos += millis() - shown_.fetchedAtMs;
  return min(pos, shown_.durationMs);
}

void PlayerUI::drawProgress(bool force) {
  const int16_t x0 = PANEL_X0 + 2, w = PANEL_W - 4;
  const uint32_t pos = currentProgress();
  const int16_t knobX = x0 + (shown_.durationMs ? int16_t(uint64_t(pos) * w / shown_.durationMs) : 0);

  if (force || knobX < lastKnobX_) {
    tft_.fillRect(PANEL_X0 - 8, PROG_TOP, W - PANEL_X0 + 8, PROG_BOTTOM - PROG_TOP, BG);
    tft_.fillRect(x0, PROG_Y - 2, w, 4, TRACK);
    lastKnobX_ = -1;
    lastSecond_ = -1;
    force = true;
  }

  if (knobX != lastKnobX_) {
    // Erase the old knob and restore the track under it, then draw fill + knob.
    if (lastKnobX_ >= 0) {
      tft_.fillRect(lastKnobX_ - 7, PROG_Y - 7, 15, 15, BG);
      int16_t a = max<int16_t>(x0, lastKnobX_ - 7), b = min<int16_t>(x0 + w, lastKnobX_ + 8);
      tft_.fillRect(a, PROG_Y - 2, b - a, 4, TRACK);
    }
    tft_.fillRect(x0, PROG_Y - 2, knobX - x0, 4, TEXT_PRIMARY);
    tft_.fillSmoothCircle(knobX, PROG_Y, 6, TEXT_PRIMARY, BG);
    lastKnobX_ = knobX;
  }

  int32_t second = pos / 1000;
  if (second != lastSecond_) {
    tft_.setTextColor(TEXT_SECONDARY, BG);
    tft_.setTextPadding(50);
    tft_.setTextDatum(TL_DATUM);
    tft_.drawString(formatTime(pos), x0, TIME_Y, 2);
    if (force) {
      tft_.setTextDatum(TR_DATUM);
      tft_.drawString(formatTime(shown_.durationMs), x0 + w, TIME_Y, 2);
    }
    tft_.setTextPadding(0);
    lastSecond_ = second;
  }
}

void PlayerUI::drawControls() {
  tft_.fillRect(PANEL_X0 - 8, MAIN_ROW_TOP, W - PANEL_X0 + 8, H - MAIN_ROW_TOP, BG);
  icons::previous(tft_, slotCenter(0), MAIN_Y, TEXT_PRIMARY);
  drawPlayButton();
  icons::next(tft_, slotCenter(2), MAIN_Y, TEXT_PRIMARY);
  drawShuffleButton();
  drawBrightnessButton();
  drawRepeatButton();
}

void PlayerUI::drawShuffleButton() {
  const int16_t cx = slotCenter(0);
  tft_.fillRect(cx - SLOT_W / 2, SUB_ROW_TOP, SLOT_W, H - SUB_ROW_TOP, BG);
  uint16_t color = shown_.shuffle ? ACCENT : TEXT_SECONDARY;
  icons::shuffle(tft_, cx, SUB_Y, color, BG);
  if (shown_.shuffle) tft_.fillSmoothCircle(cx, SUB_Y + 18, 2, ACCENT, BG);
}

void PlayerUI::drawPlayButton() {
  const int16_t cx = slotCenter(1);
  tft_.fillSmoothCircle(cx, MAIN_Y, PLAY_R, ACCENT, BG);
  if (shown_.isPlaying) {
    icons::pause(tft_, cx, MAIN_Y, BG);
  } else {
    icons::play(tft_, cx, MAIN_Y, BG);
  }
}

void PlayerUI::drawRepeatButton() {
  const int16_t cx = slotCenter(2);
  tft_.fillRect(cx - SLOT_W / 2, SUB_ROW_TOP, SLOT_W, H - SUB_ROW_TOP, BG);
  uint16_t color = shown_.repeat == RepeatMode::Off ? TEXT_SECONDARY : ACCENT;
  icons::repeat(tft_, cx, SUB_Y, color, BG, shown_.repeat == RepeatMode::One);
  tft_.setTextPadding(0);
  tft_.setTextColor(color, BG);
  tft_.setTextDatum(MC_DATUM);
  const char *label = shown_.repeat == RepeatMode::Off ? "OFF" :
                      shown_.repeat == RepeatMode::All ? "ALL" : "ONE";
  tft_.drawString(label, cx, SUB_Y + 18, 1);
}

void PlayerUI::drawBrightnessButton() {
  const int16_t cx = slotCenter(1);
  tft_.fillRect(cx - SLOT_W / 2, SUB_ROW_TOP, SLOT_W, H - SUB_ROW_TOP, BG);
  icons::brightness(tft_, cx, SUB_Y, TEXT_SECONDARY, BG);
  tft_.setTextPadding(0);
  tft_.setTextColor(TEXT_SECONDARY, BG);
  tft_.setTextDatum(MC_DATUM);
  tft_.drawString(String(brightness_) + "%", cx, SUB_Y + 18, 1);
}

// --- touch -----------------------------------------------------------------

UiAction PlayerUI::actionAt(uint16_t x, uint16_t y) const {
  if (!visible_) return UiAction::None;
  if (mode_ != Mode::None && !noticeUntil_ && idleInfo_.usesPi && idleInfo_.pollingSupported &&
      piIsOnline(idleInfo_) && x >= UPDATES_X - 4 && x < W - MARGIN + 4 && y < STATUS_H) {
    return UiAction::TogglePolling;
  }

  const bool inPanel = x >= PANEL_X0 - 12;
  const int slot = constrain((int(x) - PANEL_X0) / SLOT_W, 0, 2);
  if (mode_ != Mode::None && inPanel && y >= SUB_ROW_TOP && y < H && slot == 1) {
    return UiAction::CycleBrightness;
  }
  if (mode_ == Mode::Clock && idleInfo_.usesPi && y >= 220 && y < 274) {
    return UiAction::WeatherInfo;
  }
  if (mode_ != Mode::Player) return UiAction::None;
  const bool onArt = x >= ART_X && x < ART_X + ART_SIZE && y >= ART_Y && y < ART_Y + ART_SIZE;
  if (inPanel && y >= MAIN_ROW_TOP && y < SUB_ROW_TOP) {
    return slot == 0 ? UiAction::Previous : slot == 1 ? UiAction::PlayPause : UiAction::Next;
  }
  if (inPanel && y >= SUB_ROW_TOP && y < H) {
    if (slot == 0) return UiAction::ToggleShuffle;
    if (slot == 2) return UiAction::CycleRepeat;
  }
  return onArt ? UiAction::PlayPause : UiAction::None;
}
