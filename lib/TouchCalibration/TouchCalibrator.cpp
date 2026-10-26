#include "TouchCalibrator.h"

#include <Preferences.h>

#include "touch_calibration.h"

namespace touchcal {

static constexpr int16_t CAL_INSET = 30;
static constexpr uint8_t CAL_VERSION = 2;

bool screenPressed(TFT_eSPI &tft) { return tft.getTouchRawZ() > Z_THRESHOLD; }

void waitForRelease(TFT_eSPI &tft) {
  uint32_t releasedAt = millis();
  while (millis() - releasedAt < 150) {
    if (screenPressed(tft)) releasedAt = millis();
    delay(5);
  }
}

static RawTouchPoint readCalibrationPoint(TFT_eSPI &tft, int16_t x, int16_t y,
                                          const String &label) {
  waitForRelease(tft);
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(label, tft.width() / 2, tft.height() / 2 - 30, 4);
  tft.drawString("Hold stylus on crosshair center", tft.width() / 2, tft.height() / 2, 2);
  tft.drawString("Lift after it turns green", tft.width() / 2, tft.height() / 2 + 24, 2);
  tft.drawCircle(x, y, 12, TFT_MAGENTA);
  tft.drawFastHLine(x - 18, y, 37, TFT_MAGENTA);
  tft.drawFastVLine(x, y - 18, 37, TFT_MAGENTA);

  uint32_t sumX = 0, sumY = 0;
  uint16_t minX = 4095, minY = 4095, maxX = 0, maxY = 0;
  int count = 0;
  while (count < 20) {
    uint16_t rx, ry;
    tft.getTouchRaw(&rx, &ry);
    uint16_t z = tft.getTouchRawZ();
    if (z <= Z_THRESHOLD || rx == 0 || ry == 0 || rx >= 4095 || ry >= 4095) {
      count = 0;
    } else {
      if (count == 0) {
        sumX = sumY = 0;
        minX = maxX = rx;
        minY = maxY = ry;
      }
      minX = min(minX, rx); maxX = max(maxX, rx);
      minY = min(minY, ry); maxY = max(maxY, ry);
      if (maxX - minX > 60 || maxY - minY > 60) {
        count = 0;
      } else {
        sumX += rx; sumY += ry;
        ++count;
      }
    }
    delay(8);
  }
  RawTouchPoint point = {uint16_t(sumX / count), uint16_t(sumY / count)};
  tft.fillCircle(x, y, 7, TFT_GREEN);
  Serial.printf("    %s: raw=%u,%u\n", label.c_str(), point.x, point.y);
  waitForRelease(tft);
  return point;
}

static void calibrateTouchAccurately(TFT_eSPI &tft, uint16_t cal[5]) {
  for (;;) {
    RawTouchPoint points[4];
    points[0] = readCalibrationPoint(tft, CAL_INSET, CAL_INSET, "1 / 4 - top left");
    points[1] = readCalibrationPoint(tft, tft.width() - 1 - CAL_INSET, CAL_INSET, "2 / 4 - top right");
    points[2] = readCalibrationPoint(tft, CAL_INSET, tft.height() - 1 - CAL_INSET, "3 / 4 - bottom left");
    points[3] = readCalibrationPoint(tft, tft.width() - 1 - CAL_INSET,
                                     tft.height() - 1 - CAL_INSET, "4 / 4 - bottom right");
    bool valid = fitTouchCalibration(points, tft.width(), tft.height(), CAL_INSET, cal);
    if (valid) {
      tft.setTouch(cal);
      // Check all corners against the fitted map before asking for the center.
      for (int i = 0; i < 4; ++i) {
        uint16_t x = points[i].x, y = points[i].y;
        tft.convertRawXY(&x, &y);
        int expectedX = (i % 2) ? tft.width() - 1 - CAL_INSET : CAL_INSET;
        int expectedY = (i / 2) ? tft.height() - 1 - CAL_INSET : CAL_INSET;
        if (abs(int(x) - expectedX) > 10 || abs(int(y) - expectedY) > 10) valid = false;
      }
    }
    if (valid) {
      RawTouchPoint center = readCalibrationPoint(tft, tft.width() / 2, tft.height() / 2,
                                                  "Check center");
      tft.convertRawXY(&center.x, &center.y);
      int dx = int(center.x) - tft.width() / 2;
      int dy = int(center.y) - tft.height() / 2;
      Serial.printf("    Center check: dx=%d dy=%d px\n", dx, dy);
      valid = dx * dx + dy * dy <= 100;
    }
    if (valid) return;
    Serial.println("    Calibration inconsistent; repeating without saving.");
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Please try again", tft.width() / 2, tft.height() / 2, 4);
    delay(1800);
  }
}

// One calibration per rotation ("cal0".."cal3"), so the portrait bring-up test and
// the landscape player don't overwrite each other.
static bool loadStored(Preferences &prefs, uint8_t rotation, uint16_t cal[5]) {
  String key = "cal" + String(rotation), ver = "ver" + String(rotation);
  if (prefs.isKey(key.c_str()) && prefs.getBytesLength(key.c_str()) == 5 * sizeof(uint16_t) &&
      prefs.getUChar(ver.c_str(), 0) == CAL_VERSION) {
    prefs.getBytes(key.c_str(), cal, 5 * sizeof(uint16_t));
    return true;
  }
  // Older single-slot format: "cal" + "rot" + "ver".
  if (prefs.isKey("cal") && prefs.getBytesLength("cal") == 5 * sizeof(uint16_t) &&
      prefs.getUChar("rot", 255) == rotation &&
      prefs.getUChar("ver", 0) == CAL_VERSION) {
    prefs.getBytes("cal", cal, 5 * sizeof(uint16_t));
    return true;
  }
  return false;
}

bool loadOrCalibrate(TFT_eSPI &tft, uint8_t rotation, bool force) {
  uint16_t cal[5];
  Preferences prefs;
  prefs.begin("touch", false);

  if (!force && loadStored(prefs, rotation, cal)) {
    tft.setTouch(cal);
    prefs.end();
    Serial.printf("Loaded touch calibration (rotation %u): %u %u %u %u %u\n", rotation, cal[0],
                  cal[1], cal[2], cal[3], cal[4]);
    return false;
  }

  Serial.printf("Calibrating touch for rotation %u: hold each crosshair until green, then lift.\n",
                rotation);
  calibrateTouchAccurately(tft, cal);

  prefs.putBytes(("cal" + String(rotation)).c_str(), cal, sizeof(cal));
  prefs.putUChar(("ver" + String(rotation)).c_str(), CAL_VERSION);
  prefs.end();
  Serial.printf("Saved touch calibration: %u %u %u %u %u\n", cal[0], cal[1], cal[2], cal[3], cal[4]);
  return true;
}

}  // namespace touchcal
