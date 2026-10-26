#include "ui/Icons.h"

namespace icons {

void play(TFT_eSPI &tft, int16_t cx, int16_t cy, uint16_t color) {
  // Shifted right a little so it looks centered inside a circle.
  tft.fillTriangle(cx - 7, cy - 11, cx - 7, cy + 11, cx + 12, cy, color);
}

void pause(TFT_eSPI &tft, int16_t cx, int16_t cy, uint16_t color) {
  tft.fillRect(cx - 9, cy - 11, 6, 22, color);
  tft.fillRect(cx + 3, cy - 11, 6, 22, color);
}

void next(TFT_eSPI &tft, int16_t cx, int16_t cy, uint16_t color) {
  tft.fillTriangle(cx - 10, cy - 10, cx - 10, cy + 10, cx + 6, cy, color);
  tft.fillRect(cx + 6, cy - 10, 4, 20, color);
}

void previous(TFT_eSPI &tft, int16_t cx, int16_t cy, uint16_t color) {
  tft.fillTriangle(cx + 10, cy - 10, cx + 10, cy + 10, cx - 6, cy, color);
  tft.fillRect(cx - 10, cy - 10, 4, 20, color);
}

void shuffle(TFT_eSPI &tft, int16_t cx, int16_t cy, uint16_t color, uint16_t bg) {
  // Two crossing paths, each ending in an arrowhead on the right.
  tft.drawWideLine(cx - 11, cy + 7, cx + 6, cy - 7, 2.5, color, bg);
  tft.drawWideLine(cx - 11, cy - 7, cx + 6, cy + 7, 2.5, color, bg);
  tft.fillTriangle(cx + 12, cy - 7, cx + 5, cy - 12, cx + 5, cy - 2, color);
  tft.fillTriangle(cx + 12, cy + 7, cx + 5, cy + 2, cx + 5, cy + 12, color);
}

void repeat(TFT_eSPI &tft, int16_t cx, int16_t cy, uint16_t color, uint16_t bg, bool one) {
  tft.drawRoundRect(cx - 12, cy - 8, 25, 17, 5, color);
  tft.drawRoundRect(cx - 11, cy - 7, 23, 15, 4, color);
  tft.fillTriangle(cx + 15, cy - 8, cx + 8, cy - 13, cx + 8, cy - 3, color);
  tft.fillTriangle(cx - 15, cy + 8, cx - 8, cy + 3, cx - 8, cy + 13, color);
  if (one) {
    tft.setTextPadding(0);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(color, bg);
    tft.drawString("1", cx, cy + 1, 1);
  }
}

void brightness(TFT_eSPI &tft, int16_t cx, int16_t cy, uint16_t color, uint16_t bg) {
  tft.drawCircle(cx, cy, 4, color);
  tft.drawCircle(cx, cy, 5, color);
  const int8_t rays[8][4] = {
      {0, -9, 0, -13}, {6, -6, 9, -9}, {9, 0, 13, 0}, {6, 6, 9, 9},
      {0, 9, 0, 13}, {-6, 6, -9, 9}, {-9, 0, -13, 0}, {-6, -6, -9, -9}};
  for (const auto &ray : rays) {
    tft.drawWideLine(cx + ray[0], cy + ray[1], cx + ray[2], cy + ray[3], 2, color, bg);
  }
}

}  // namespace icons
