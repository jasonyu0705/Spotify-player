#pragma once

#include <stdint.h>

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

namespace theme {
constexpr uint16_t BG = rgb565(18, 18, 18);
constexpr uint16_t TEXT_PRIMARY = rgb565(255, 255, 255);
constexpr uint16_t TEXT_SECONDARY = rgb565(179, 179, 179);
constexpr uint16_t TEXT_TERTIARY = rgb565(120, 120, 120);
constexpr uint16_t ICON_DISABLED = rgb565(90, 90, 90);
constexpr uint16_t TRACK = rgb565(77, 77, 77);    // unfilled part of the progress bar
constexpr uint16_t ACCENT = rgb565(30, 215, 96);  // Spotify green
constexpr uint16_t ERROR_RED = rgb565(241, 94, 108);
constexpr uint16_t VINYL = rgb565(22, 22, 22);
constexpr uint16_t VINYL_GROOVE = rgb565(48, 48, 48);
}  // namespace theme

// Landscape 480x320. Album art on the left; the right-hand panel stacks
// title/artist/album, progress, then two button rows.
namespace layout {
constexpr int16_t W = 480;
constexpr int16_t H = 320;
constexpr int16_t MARGIN = 16;

constexpr int16_t STATUS_H = 28;

constexpr int16_t ART_X = 16;
constexpr int16_t ART_Y = 40;
constexpr int16_t ART_SIZE = 264;

constexpr int16_t PANEL_X0 = 298;
constexpr int16_t PANEL_X1 = 464;
constexpr int16_t PANEL_W = PANEL_X1 - PANEL_X0;

constexpr int16_t INFO_Y = 40;     // title (font 4, up to 2 lines), artist, album
constexpr int16_t INFO_H = 108;
constexpr int16_t TITLE_LINE_H = 28;

constexpr int16_t PROG_TOP = 154;
constexpr int16_t PROG_Y = 168;    // bar center line
constexpr int16_t TIME_Y = 178;    // top of time text
constexpr int16_t PROG_BOTTOM = 198;

constexpr int16_t MAIN_ROW_TOP = 200;  // previous / play / next
constexpr int16_t MAIN_Y = 234;
constexpr int16_t PLAY_R = 28;
constexpr int16_t SUB_ROW_TOP = 266;   // shuffle / brightness / repeat
constexpr int16_t SUB_Y = 292;
constexpr int16_t SLOT_W = PANEL_W / 3;

}  // namespace layout
