#pragma once

#include <TFT_eSPI.h>

// JPEGs stream from flash straight to the display; no full image buffer in RAM.
// Only call from the main/UI task, which also owns the Spotify JPEG decoder.
class PhotoFrame {
 public:
  explicit PhotoFrame(TFT_eSPI &tft) : tft_(tft) {}
  void begin();
  void show();
  void next();
  bool clockContains(uint16_t x, uint16_t y) const;
  void tick();  // call only while the photo screen is visible

 private:
  void drawPhoto();
  void drawClock(bool force);
  void showMessage(const char *title, const char *detail);

  static constexpr size_t MAX_PHOTOS = 32;
  TFT_eSPI &tft_;
  String photos_[MAX_PHOTOS];
  size_t count_ = 0, index_ = 0;
  bool mounted_ = false;
  uint32_t shownAt_ = 0;
  int64_t lastClockMinute_ = -2;
};
