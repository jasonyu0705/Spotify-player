#pragma once

#include <TFT_eSPI.h>

// Control icons drawn from primitives, centered on (cx, cy). `bg` is the color
// behind the icon, used for anti-aliased edges.
namespace icons {
void play(TFT_eSPI &tft, int16_t cx, int16_t cy, uint16_t color);
void pause(TFT_eSPI &tft, int16_t cx, int16_t cy, uint16_t color);
void next(TFT_eSPI &tft, int16_t cx, int16_t cy, uint16_t color);
void previous(TFT_eSPI &tft, int16_t cx, int16_t cy, uint16_t color);
void shuffle(TFT_eSPI &tft, int16_t cx, int16_t cy, uint16_t color, uint16_t bg);
void repeat(TFT_eSPI &tft, int16_t cx, int16_t cy, uint16_t color, uint16_t bg, bool one);
void brightness(TFT_eSPI &tft, int16_t cx, int16_t cy, uint16_t color, uint16_t bg);
}  // namespace icons
