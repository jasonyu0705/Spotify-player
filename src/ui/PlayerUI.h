#pragma once

#include <TFT_eSPI.h>

#include "backend/NowPlaying.h"
#include "backend/IdleInfo.h"

enum class UiAction { None, PlayPause, Next, Previous, ToggleShuffle, CycleRepeat, CycleBrightness, WeatherInfo, TogglePolling };

// Draws the now-playing screen and turns taps into actions. Only redraws the parts
// that changed, so it can be updated every poll without flicker.
class PlayerUI {
 public:
  explicit PlayerUI(TFT_eSPI &tft) : tft_(tft) {}

  void begin(const char *backendName);
  void setVisible(bool visible);  // Spotify keeps syncing while the photo screen is open
  void setBrightness(uint8_t percent);
  void setIdleInfo(const IdleInfo &info);

  // Full-screen message, e.g. "Connecting..." or "Nothing playing".
  void showMessage(const char *title, const char *detail);
  void showStatus(BackendStatus status);
  void showClock();  // stable idle display; returns when a playing or paused track is available

  // Shows `np`, switching from a message screen to the player if needed.
  void update(const NowPlaying &np);

  // Call every loop: moves progress between polls and clears expired notices.
  void tick();

  // Called only after a completed tap (swipes are handled by the app).
  // Tapping the album art also toggles play/pause.
  UiAction actionAt(uint16_t x, uint16_t y) const;

  // Brief status-bar notice; errors are red, informational notices are neutral.
  void showNotice(const char *text, bool error = true);

 private:
  enum class Mode { None, Message, Player, Clock };

  void redrawAll();
  void drawStatusBar();
  void drawClock(bool force);
  void drawIdleDetails(bool force);
  void drawArt();
  void drawInfo();
  void drawProgress(bool force);
  void drawControls();
  void drawShuffleButton();
  void drawPlayButton();
  void drawRepeatButton();
  void drawBrightnessButton();
  uint32_t currentProgress() const;

  TFT_eSPI &tft_;
  const char *backendName_ = "";
  Mode mode_ = Mode::None;
  BackendStatus messageStatus_ = BackendStatus::Ok;

  NowPlaying shown_;
  int16_t lastKnobX_ = -1;
  int32_t lastSecond_ = -1;
  int64_t lastClockMinute_ = -2;
  IdleInfo idleInfo_;
  uint32_t lastIdleCheckMs_ = 0;
  int lastConnectionState_ = -1;
  String lastWeatherText_;
  String lastWeatherLocation_;
  String lastPiAddress_;
  String lastEspAddress_;
  int lastWeatherCode_ = -2;
  bool lastWeatherDay_ = true;

  bool visible_ = true;
  uint8_t brightness_ = 100;

  String notice_;
  bool noticeError_ = true;
  uint32_t noticeUntil_ = 0;
};

const char *statusTitle(BackendStatus status);
