#include "ui/PlayerUI.h"

#include "ui/Icons.h"
#include "ui/AlbumArtRenderer.h"
#include "ui/Text.h"
#include "ui/Theme.h"
#include <time.h>

using namespace layout;
using namespace theme;

namespace {

constexpr uint32_t NOTICE_MS = 2500;
constexpr uint32_t PROGRESS_JUMP_MS = 2000;  // larger gap than this means a seek, not drift

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
  tft_.setTextDatum(MC_DATUM);
  tft_.setTextColor(TEXT_TERTIARY, BG);
  tft_.drawString("Nothing is playing", W / 2, 248, 2);
  drawBrightnessButton();
}

void PlayerUI::drawClock(bool force) {
  // Never call getLocalTime() here: it can wait seconds for NTP and block touch.
  const time_t now = time(nullptr);
  const bool synced = now >= 1700000000;
  const int64_t minute = synced ? int64_t(now) / 60 : -1;
  if (!force && minute == lastClockMinute_) return;
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
  tft_.fillRect(0, 60, W, 174, BG);
  tft_.setTextPadding(0);
  tft_.setTextDatum(MC_DATUM);
  tft_.setTextColor(TEXT_PRIMARY, BG);
  tft_.setTextSize(2);
  tft_.drawString(clockText, W / 2, 128, 7);
  tft_.setTextSize(1);
  tft_.setTextColor(TEXT_SECONDARY, BG);
  tft_.drawString(period, W / 2, 189, 2);
  tft_.drawString(dateText, W / 2, 212, 4);
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

void PlayerUI::showNotice(const char *text) {
  if (!visible_) return;
  notice_ = text;
  noticeUntil_ = millis() + NOTICE_MS;
  drawStatusBar();
}

// --- regions ---------------------------------------------------------------

void PlayerUI::drawStatusBar() {
  tft_.fillRect(0, 0, W, STATUS_H, BG);
  tft_.setTextPadding(0);
  tft_.setTextDatum(ML_DATUM);

  if (noticeUntil_) {
    tft_.setTextColor(ERROR_RED, BG);
    tft_.drawString(fitText(tft_, notice_, 2, W - 2 * MARGIN), MARGIN, STATUS_H / 2, 2);
    return;
  }

  if (mode_ == Mode::Player) {
    tft_.fillSmoothCircle(MARGIN + 3, STATUS_H / 2, 3,
                         shown_.isLastPlayed ? TEXT_TERTIARY : ACCENT, BG);
    tft_.setTextColor(TEXT_SECONDARY, BG);
    String prefix = shown_.isLastPlayed ? "Last played" : shown_.isPlaying ? "Playing" : "Paused";
    String label = prefix + (shown_.deviceName.isEmpty() ? "" : " on " + toDisplayText(shown_.deviceName));
    String device = fitText(tft_, label, 2, W - 120);
    tft_.drawString(device, MARGIN + 12, STATUS_H / 2, 2);
  }
  tft_.setTextDatum(MR_DATUM);
  tft_.setTextColor(ICON_DISABLED, BG);
  tft_.drawString(backendName_, W - MARGIN, STATUS_H / 2, 2);
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

  const bool inPanel = x >= PANEL_X0 - 12;
  const int slot = constrain((int(x) - PANEL_X0) / SLOT_W, 0, 2);
  if (mode_ != Mode::None && inPanel && y >= SUB_ROW_TOP && y < H && slot == 1) {
    return UiAction::CycleBrightness;
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
