// Run on the host: c++ -std=c++11 test/host_touch_calibration.cpp -o /tmp/touch-cal-test && /tmp/touch-cal-test
#include "../lib/TouchCalibration/touch_calibration.h"
#include <assert.h>
#include <cmath>
#include <cstdio>

static RawTouchPoint sensorPoint(int x, int y, int width, int height, int flags) {
  double u = (flags & 2) ? width - x : x;
  double v = (flags & 4) ? height - y : y;
  uint16_t a = uint16_t(std::lround(200 + u * 3600 / width));
  uint16_t b = uint16_t(std::lround(180 + v * 3650 / height));
  return (flags & 1) ? RawTouchPoint{b, a} : RawTouchPoint{a, b};
}

int main() {
  for (int orientation = 0; orientation < 2; ++orientation) {
    int w = orientation ? 480 : 320, h = orientation ? 320 : 480;
    for (int flags = 0; flags < 8; ++flags) {
      RawTouchPoint points[] = {sensorPoint(30, 30, w, h, flags),
                               sensorPoint(w - 31, 30, w, h, flags),
                               sensorPoint(30, h - 31, w, h, flags),
                               sensorPoint(w - 31, h - 31, w, h, flags)};
      uint16_t cal[5];
      assert(fitTouchCalibration(points, w, h, 30, cal));
      assert(cal[4] == flags);
      // Check independent points, including extrapolation to the screen edges.
      for (int x = 0; x < w; x += 17) {
        for (int y = 0; y < h; y += 19) {
          RawTouchPoint p = sensorPoint(x, y, w, h, flags);
          double u = (flags & 1) ? p.y : p.x;
          double v = (flags & 1) ? p.x : p.y;
          double mx = (u - cal[0]) * w / cal[1];
          double my = (v - cal[2]) * h / cal[3];
          if (flags & 2) mx = w - mx;
          if (flags & 4) my = h - my;
          assert(std::abs(mx - x) < 2);
          assert(std::abs(my - y) < 2);
        }
      }
    }
  }
  uint16_t cal[5];
  RawTouchPoint repeated[] = {{2000, 2000}, {2000, 2000}, {2000, 2000}, {2000, 2000}};
  assert(!fitTouchCalibration(repeated, 320, 480, 30, cal));
  RawTouchPoint narrow[] = {{1000, 1000}, {1100, 1000}, {1000, 1100}, {1100, 1100}};
  assert(!fitTouchCalibration(narrow, 320, 480, 30, cal));
  RawTouchPoint bad[] = {{5000, 2000}, {3000, 2000}, {1000, 3500}, {3000, 3500}};
  assert(!fitTouchCalibration(bad, 320, 480, 30, cal));
  assert(!fitTouchCalibration(repeated, 60, 480, 30, cal));
  std::puts("PASS: portrait/landscape, all 8 axis orientations, full-screen mapping, invalid samples");
}
