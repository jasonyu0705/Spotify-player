#include "../src/ui/TouchGesture.h"

#include <cassert>
#include <cstdio>

int main() {
  TouchGesture gesture;
  assert(gesture.update(false, 0, 0, 0) == TouchEvent::None);
  assert(gesture.update(true, 350, 230, 10) == TouchEvent::None);
  assert(gesture.update(true, 356, 235, 40) == TouchEvent::None);
  assert(gesture.update(false, 356, 235, 100) == TouchEvent::Tap);
  assert(gesture.tapX() == 350 && gesture.tapY() == 230);
  assert(gesture.update(false, 356, 235, 110) == TouchEvent::None);

  // A swipe beginning on the play button emits no tap, even during the drag.
  assert(gesture.update(true, 380, 234, 200) == TouchEvent::None);
  assert(gesture.update(true, 280, 238, 300) == TouchEvent::None);
  assert(gesture.update(false, 280, 238, 400) == TouchEvent::SwipeLeft);
  gesture.update(true, 50, 100, 500);
  gesture.update(true, 150, 110, 600);
  assert(gesture.update(false, 150, 110, 700) == TouchEvent::SwipeRight);

  // Long holds, vertical drags, diagonal drags, and out-and-back motion do not tap.
  gesture.update(true, 100, 100, 800);
  assert(gesture.update(false, 100, 100, 1700) == TouchEvent::None);
  gesture.update(true, 100, 100, 1800);
  gesture.update(true, 105, 190, 1850);
  assert(gesture.update(false, 105, 190, 1900) == TouchEvent::None);
  gesture.update(true, 100, 100, 2000);
  gesture.update(true, 180, 170, 2050);
  assert(gesture.update(false, 180, 170, 2100) == TouchEvent::None);
  gesture.update(true, 100, 100, 2200);
  gesture.update(true, 200, 100, 2250);
  gesture.update(true, 100, 100, 2300);
  assert(gesture.update(false, 100, 100, 2350) == TouchEvent::None);

  // Recalibration cancels an old touch; millis rollover still permits taps.
  gesture.update(true, 100, 100, 2400);
  gesture.reset();
  assert(gesture.update(false, 100, 100, 2500) == TouchEvent::None);
  gesture.update(true, 100, 100, UINT32_MAX - 50);
  assert(gesture.update(false, 100, 100, 20) == TouchEvent::Tap);
  std::puts("PASS: tap/swipe separation, direction, drag rejection, reset, and timer rollover");
}
