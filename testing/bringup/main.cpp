// Bring-up test for the Hosyond 4.0" ESP32 board (ST7796 320x480 + XPT2046 touch).
//
// Runs in order, tap the screen to skip ahead on the first three:
//   1. Color bars      - each bar is labeled; label must match the color
//   2. Draw speed      - times full-screen fills
//   3. Text and info   - resolution, fonts, free memory
//   4. Touch calibrate - first boot, hold BOOT during the touch test, or send 'c' on serial
//   5. Touch test      - tap the 5 targets; your finger leaves a trail; CLEAR resets
//
// Everything is also printed to the serial monitor (115200 baud).

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <TouchCalibrator.h>

static constexpr uint8_t ROTATION = 0;          // 0 = portrait (320x480), 1 = landscape (480x320)
static constexpr uint16_t TOUCH_Z_THRESHOLD = 350;
static constexpr uint8_t BOOT_BUTTON = 0;
static constexpr int16_t HEADER_H = 40;
static constexpr int16_t BUTTON_H = 50;
static constexpr int16_t TARGET_R = 12;
static constexpr int16_t HIT_TOLERANCE = 12;    // radial distance from the target center

TFT_eSPI tft;

struct Target {
  int16_t x, y;
  bool hit;
};
Target targets[5];
bool allHitReported = false;

// --- helpers ---------------------------------------------------------------

bool screenPressed() { return tft.getTouchRawZ() > TOUCH_Z_THRESHOLD; }

void waitForRelease() {
  uint32_t releasedAt = millis();
  while (millis() - releasedAt < 150) {
    if (screenPressed()) releasedAt = millis();
    delay(5);
  }
}

// Waits for `ms`, returning early if the screen is tapped.
void holdOrTap(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    if (screenPressed()) {
      waitForRelease();
      return;
    }
    delay(10);
  }
}

// --- 1. color bars ---------------------------------------------------------

void colorTest() {
  struct Bar {
    uint16_t color;
    const char *name;
    uint16_t textColor;
  };
  const Bar bars[] = {
      {TFT_RED, "RED", TFT_WHITE},     {TFT_GREEN, "GREEN", TFT_BLACK},
      {TFT_BLUE, "BLUE", TFT_WHITE},   {TFT_WHITE, "WHITE", TFT_BLACK},
      {TFT_BLACK, "BLACK", TFT_WHITE},
  };
  const int n = sizeof(bars) / sizeof(bars[0]);
  const int h = tft.height() / n;

  for (int i = 0; i < n; i++) {
    int y = i * h;
    int barH = (i == n - 1) ? tft.height() - y : h;
    tft.fillRect(0, y, tft.width(), barH, bars[i].color);
    tft.setTextColor(bars[i].textColor, bars[i].color);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(bars[i].name, tft.width() / 2, y + barH / 2, 4);
  }
  tft.drawRect(0, 0, tft.width(), tft.height(), TFT_DARKGREY);  // shows the screen edges

  Serial.println("[1] Color bars: labels should match colors.");
  Serial.println("    Negative-looking colors -> enable TFT_INVERSION_ON in platformio.ini");
  Serial.println("    Red/blue swapped         -> enable TFT_RGB_ORDER=TFT_BGR in platformio.ini");
  holdOrTap(5000);
}

// --- 2. draw speed ---------------------------------------------------------

uint32_t speedTest() {
  const uint16_t colors[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_BLACK};
  const int frames = 12;
  uint32_t start = millis();
  for (int i = 0; i < frames; i++) tft.fillScreen(colors[i % 4]);
  uint32_t perFrame = (millis() - start) / frames;
  Serial.printf("[2] Full-screen fill: %lu ms per frame\n", perFrame);
  return perFrame;
}

// --- 3. text and info ------------------------------------------------------

void infoScreen(uint32_t fillMs) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  int y = 10;

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String(tft.width()) + " x " + String(tft.height()), 10, y, 4);
  y += 34;

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Rotation " + String(ROTATION) + "   fill " + String(fillMs) + " ms", 10, y, 2);
  y += 20;
  tft.drawString("Free heap " + String(ESP.getFreeHeap() / 1024) + " KB   largest block " +
                     String(ESP.getMaxAllocHeap() / 1024) + " KB",
                 10, y, 2);
  y += 30;

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("Font 1: tiny text 0123456789", 10, y, 1);
  y += 16;
  tft.drawString("Font 2: small text", 10, y, 2);
  y += 22;
  tft.drawString("Font 4: Song Title", 10, y, 4);
  y += 34;
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("1:23", 10, y, 6);
  y += 52;
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString("12:34", 10, y, 7);
  y += 60;

  // Fine-detail checks: 1px lines and a circle should look crisp, not smeared.
  tft.drawFastHLine(10, y, tft.width() - 20, TFT_WHITE);
  for (int x = 10; x < tft.width() - 10; x += 4) tft.drawFastVLine(x, y + 4, 20, TFT_WHITE);
  tft.drawCircle(tft.width() - 40, y + 60, 28, TFT_YELLOW);
  tft.fillRoundRect(10, y + 40, 150, 40, 8, TFT_DARKGREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Button", 85, y + 60, 4);

  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Tap to continue", tft.width() / 2, tft.height() - 6, 2);

  Serial.printf("[3] %d x %d, rotation %d, free heap %u, largest block %u\n", tft.width(),
                tft.height(), ROTATION, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  holdOrTap(15000);
}

// --- 4. touch calibration --------------------------------------------------
// The calibration itself lives in lib/TouchCalibration so the controller shares it.

void loadOrCalibrateTouch(bool force) {
  Serial.println("[4] Touch calibration");
  touchcal::loadOrCalibrate(tft, ROTATION, force);
}

// --- 5. touch test ---------------------------------------------------------

bool inClearButton(int16_t x, int16_t y) { return y >= tft.height() - BUTTON_H; }

void drawTarget(const Target &t) {
  uint16_t c = t.hit ? TFT_GREEN : TFT_RED;
  tft.drawCircle(t.x, t.y, TARGET_R, c);
  tft.drawFastHLine(t.x - TARGET_R - 4, t.y, TARGET_R * 2 + 9, c);
  tft.drawFastVLine(t.x, t.y - TARGET_R - 4, TARGET_R * 2 + 9, c);
  if (t.hit) tft.fillCircle(t.x, t.y, 5, TFT_GREEN);
}

void setHeader(const String &text, uint16_t color) {
  tft.fillRect(0, 0, tft.width(), HEADER_H, TFT_NAVY);
  tft.setTextColor(color, TFT_NAVY);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(text, 8, HEADER_H / 2, 2);
}

void drawTouchScreen() {
  const int16_t w = tft.width(), h = tft.height();
  const int16_t inset = 40;
  const int16_t top = HEADER_H + inset, bottom = h - BUTTON_H - inset;

  targets[0] = {inset, top, false};
  targets[1] = {int16_t(w - inset), top, false};
  targets[2] = {int16_t(w / 2), int16_t((top + bottom) / 2), false};
  targets[3] = {inset, bottom, false};
  targets[4] = {int16_t(w - inset), bottom, false};
  allHitReported = false;

  tft.fillScreen(TFT_BLACK);
  setHeader("Tap the 5 red targets", TFT_WHITE);
  for (const Target &t : targets) drawTarget(t);

  tft.fillRect(0, h - BUTTON_H, w, BUTTON_H, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("CLEAR", w / 2, h - BUTTON_H / 2, 4);
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Hold BOOT 1s to recalibrate", w / 2, h - BUTTON_H - 4, 2);

  Serial.println("[5] Touch test: tap the targets, drag to draw, CLEAR to reset.");
  Serial.println("    Hold BOOT for 1 second or send 'c' to recalibrate.");
}

void handleTouch() {
  static uint32_t lastPrint = 0;
  static uint32_t lastRejectedPrint = 0;
  uint16_t x, y;
  if (!tft.getTouch(&x, &y, TOUCH_Z_THRESHOLD)) {
    // Report pressure even when calibration maps a touch outside the display.
    // Otherwise rejected touches look exactly like an unresponsive controller.
    if (millis() - lastRejectedPrint >= 500) {
      lastRejectedPrint = millis();
      uint16_t z = tft.getTouchRawZ();
      if (z > TOUCH_Z_THRESHOLD) {
        uint16_t rx, ry;
        tft.getTouchRaw(&rx, &ry);
        uint16_t mx = rx, my = ry;
        tft.convertRawXY(&mx, &my);
        Serial.printf("    Unstable or out-of-bounds touch: raw=%u,%u mapped=%u,%u z=%u\n",
                      rx, ry, mx, my, z);
      }
    }
    return;
  }

  if (inClearButton(x, y)) {
    Serial.printf("    CLEAR touch x=%u y=%u\n", x, y);
    waitForRelease();
    drawTouchScreen();
    return;
  }

  if (y > HEADER_H) tft.fillCircle(x, y, 3, TFT_YELLOW);

  for (Target &t : targets) {
    int dx = int(x) - t.x, dy = int(y) - t.y;
    if (!t.hit && dx * dx + dy * dy <= HIT_TOLERANCE * HIT_TOLERANCE) {
      t.hit = true;
      drawTarget(t);
      Serial.printf("    Hit target at (%d, %d), touch was (%u, %u), off by %d px\n", t.x, t.y,
                    x, y, int(sqrt(sq(int(x) - t.x) + sq(int(y) - t.y))));
    }
  }

  bool allHit = true;
  for (const Target &t : targets) allHit &= t.hit;

  if (allHit && !allHitReported) {
    allHitReported = true;
    setHeader("All targets hit within 12px", TFT_GREEN);
    Serial.println("    All targets hit within 12px; check trail alignment with stylus.");
  } else if (!allHit && millis() - lastPrint > 60) {
    uint16_t rx, ry;
    tft.getTouchRaw(&rx, &ry);
    setHeader("x=" + String(x) + " y=" + String(y) + "   raw " + String(rx) + "," + String(ry) +
                  "  z=" + String(tft.getTouchRawZ()),
              TFT_WHITE);
  }

  if (millis() - lastPrint > 60) {
    lastPrint = millis();
    Serial.printf("    touch x=%u y=%u z=%u\n", x, y, tft.getTouchRawZ());
  }
}

void handleCalibrationRequest() {
  static bool bootHeld = false;
  static bool bootHandled = false;
  static uint32_t bootStarted = 0;
  bool requested = false;

  while (Serial.available()) {
    char command = Serial.read();
    if (command == 'c' || command == 'C') requested = true;
  }

  if (digitalRead(BOOT_BUTTON) == LOW) {
    if (!bootHeld) {
      bootHeld = true;
      bootStarted = millis();
    }
    if (!bootHandled && millis() - bootStarted >= 1000) {
      bootHandled = true;
      requested = true;
    }
  } else {
    bootHeld = false;
    bootHandled = false;
  }

  if (requested) {
    loadOrCalibrateTouch(true);
    drawTouchScreen();
  }
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  delay(300);
  Serial.println();
  Serial.println("=== Spotify ESP bring-up test ===");
  Serial.printf("Chip %s rev %d, %d cores @ %lu MHz, flash %u KB, PSRAM %u KB\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getChipCores(), ESP.getCpuFreqMHz(),
                ESP.getFlashChipSize() / 1024, ESP.getPsramSize() / 1024);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);

  tft.init();
  tft.setRotation(ROTATION);

  // Holding the screen during boot forces recalibration.
  bool forceCalibration = screenPressed();
  if (forceCalibration) {
    Serial.println("Screen held at boot -> recalibrating touch.");
    waitForRelease();
  } else {
    colorTest();
    infoScreen(speedTest());
  }

  loadOrCalibrateTouch(forceCalibration);
  drawTouchScreen();
}

void loop() {
  handleCalibrationRequest();
  handleTouch();
  delay(5);
}
