#pragma once

#include <stdint.h>

enum class TouchEvent { None, Tap, SwipeLeft, SwipeRight };

// Decide on release so a swipe starting on a Spotify button never also taps it.
// readTouch() already filters short resistive-touch dropouts.
class TouchGesture {
 public:
  TouchEvent update(bool pressed, int x, int y, uint32_t now) {
    if (pressed) {
      if (!down_) {
        down_ = true;
        startX_ = x;
        startY_ = y;
        startedAt_ = now;
        maxDx_ = maxDy_ = 0;
      }
      lastX_ = x;
      lastY_ = y;
      const int dx = magnitude(x - startX_), dy = magnitude(y - startY_);
      if (dx > maxDx_) maxDx_ = dx;
      if (dy > maxDy_) maxDy_ = dy;
      return TouchEvent::None;
    }
    if (!down_) return TouchEvent::None;
    down_ = false;
    const int dx = lastX_ - startX_;
    if (magnitude(dx) >= 60 && magnitude(dx) >= 2 * maxDy_) {
      return dx < 0 ? TouchEvent::SwipeLeft : TouchEvent::SwipeRight;
    }
    if (maxDx_ <= 18 && maxDy_ <= 18 && uint32_t(now - startedAt_) <= 800) {
      return TouchEvent::Tap;
    }
    return TouchEvent::None;
  }

  int tapX() const { return startX_; }
  int tapY() const { return startY_; }
  void reset() { down_ = false; }

 private:
  static int magnitude(int value) { return value < 0 ? -value : value; }
  bool down_ = false;
  int startX_ = 0, startY_ = 0, lastX_ = 0, lastY_ = 0;
  int maxDx_ = 0, maxDy_ = 0;
  uint32_t startedAt_ = 0;
};
