#pragma once

#include <TFT_eSPI.h>

// Shared by the bring-up test and the controller, so both use one saved calibration
// (Preferences namespace "touch").
namespace touchcal {

constexpr uint16_t Z_THRESHOLD = 350;

bool screenPressed(TFT_eSPI &tft);
void waitForRelease(TFT_eSPI &tft);

// Applies the saved calibration for `rotation`, or runs the 4-corner calibration
// when none is saved or `force` is set. Returns true if it calibrated.
bool loadOrCalibrate(TFT_eSPI &tft, uint8_t rotation, bool force);

}  // namespace touchcal
