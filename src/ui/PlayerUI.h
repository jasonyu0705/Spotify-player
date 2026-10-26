#pragma once

#include <TFT_eSPI.h>

#include "backend/NowPlaying.h"

enum class UiAction { None, PlayPause, Next, Previous, ToggleShuffle, CycleRepeat, CycleBrightness };

// Draws the now-playing screen and turns taps into actions. Only redraws the parts
// that changed, so it can be updated every poll without flicker.
class PlayerUI {
 public:
  explicit PlayerUI(TFT_eSPI &tft) : tft_(tft) {}

  void begin(const char *backendName);
  void setBrightness(uint8_t percent);

  // Full-screen message, e.g. "Connecting..." or "Nothing playing".
  void showMessage(const char *title, const char *detail);
  void showStatus(BackendStatus status);
  void showClock();  // stable idle display; returns when a playing or paused track is available

  // Shows `np`, switching from a message screen to the player if needed.
  void update(const NowPlaying &np);

  // Call every loop: moves progress between polls and clears expired notices.
  void tick();

  // Pass the debounced touch state every loop. Returns what the user asked for.
  // Tapping the album art also toggles play/pause.
  UiAction handleTouch(bool pressed, uint16_t x, uint16_t y);

  // Brief red notice in the status bar, e.g. when a command fails.
  void showNotice(const char *text);

 private:
  enum class Mode { None, Message, Player, Clock };

  void redrawAll();
  void drawStatusBar();
  void drawClock(bool force);
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

  bool wasPressed_ = false;
  uint8_t brightness_ = 100;

  String notice_;
  uint32_t noticeUntil_ = 0;
};

const char *statusTitle(BackendStatus status);
