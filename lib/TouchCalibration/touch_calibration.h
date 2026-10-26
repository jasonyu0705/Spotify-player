#pragma once

#include <stdint.h>

struct RawTouchPoint {
  uint16_t x, y;
};

// Samples are top-left, top-right, bottom-left, bottom-right. Account for
// the inset target positions: these samples are not the edges of the panel.
inline bool fitTouchCalibration(const RawTouchPoint points[4], int width, int height,
                                int inset, uint16_t cal[5]) {
  if (width <= 2 * inset + 1 || height <= 2 * inset + 1 || inset < 0) return false;
  for (int i = 0; i < 4; ++i) {
    if (points[i].x > 4095 || points[i].y > 4095) return false;
  }
  int dx = int(points[1].x) + points[3].x - points[0].x - points[2].x;
  int dy = int(points[1].y) + points[3].y - points[0].y - points[2].y;
  bool swap = (dy * dy > dx * dx);
  cal[4] = swap ? 1 : 0;
  for (int axis = 0; axis < 2; ++axis) {
    int32_t raw[4];
    for (int i = 0; i < 4; ++i) {
      raw[i] = (swap != (axis == 1)) ? points[i].y : points[i].x;
    }
    int32_t low = axis == 0 ? (raw[0] + raw[2]) / 2 : (raw[0] + raw[1]) / 2;
    int32_t high = axis == 0 ? (raw[1] + raw[3]) / 2 : (raw[2] + raw[3]) / 2;
    int size = axis == 0 ? width : height;
    int32_t delta = high - low;
    if (delta > -1000 && delta < 1000) return false;
    int32_t edge0 = low - delta * inset / (size - 1 - 2 * inset);
    int32_t edge1 = low + delta * (size - inset) / (size - 1 - 2 * inset);
    bool inverted = delta < 0;
    int32_t origin = inverted ? edge1 : edge0;
    int32_t span = inverted ? edge0 - edge1 : edge1 - edge0;
    if (origin < 1 || origin > 4095 || span < 1000 || span > 5000) return false;
    cal[axis * 2] = origin;
    cal[axis * 2 + 1] = span;
    if (inverted) cal[4] |= axis == 0 ? 2 : 4;
  }
  return true;
}
