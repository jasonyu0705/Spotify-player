#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// The built-in TFT_eSPI fonts are ASCII only. Converts UTF-8 to the closest ASCII
// ("Café Niño" -> "Cafe Nino"); characters with no equivalent become '?'.
String toDisplayText(const String &utf8);

// Shortens `text` with "..." so it fits in `maxWidth` pixels in `font`.
String fitText(TFT_eSPI &tft, const String &text, uint8_t font, int16_t maxWidth);

// Breaks `text` into at most `maxLines` lines that fit `maxWidth`, splitting at
// spaces where possible; the last line ends in "..." if text is left over.
// Returns the number of lines written to `lines`.
int wrapText(TFT_eSPI &tft, const String &text, uint8_t font, int16_t maxWidth, String lines[],
             int maxLines);

// 83000 -> "1:23"
String formatTime(uint32_t ms);
