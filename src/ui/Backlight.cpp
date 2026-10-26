#include "ui/Backlight.h"

#include <Preferences.h>
#include <TFT_eSPI.h>

namespace {
constexpr uint8_t PWM_CHANNEL = 0;
constexpr uint32_t PWM_FREQUENCY = 5000;
constexpr uint8_t PWM_BITS = 8;
}

void Backlight::begin() {
  Preferences prefs;
  if (prefs.begin("display", false)) {
    if (prefs.isKey("brightness")) percent_ = prefs.getUChar("brightness", 100);
    prefs.end();
  }
  if (percent_ < 10 || percent_ > 100) percent_ = 100;
  ledcSetup(PWM_CHANNEL, PWM_FREQUENCY, PWM_BITS);
  ledcAttachPin(TFT_BL, PWM_CHANNEL);
  apply();
}

void Backlight::apply() {
  uint32_t duty = (uint32_t(percent_) * 255 + 50) / 100;
  if (TFT_BACKLIGHT_ON == LOW) duty = 255 - duty;
  ledcWrite(PWM_CHANNEL, duty);
  Serial.printf("Display brightness: %u%%\n", percent_);
}

void Backlight::cycle() {
  percent_ = percent_ > 75 ? 75 : percent_ > 50 ? 50 : percent_ > 25 ? 25 : percent_ > 10 ? 10 : 100;
  apply();
  changedAt_ = millis();
  dirty_ = true;
}

void Backlight::tick() {
  // Coalesce quick taps into one flash write once the setting settles.
  if (!dirty_ || millis() - changedAt_ < 1000) return;
  Preferences prefs;
  if (prefs.begin("display", false)) {
    bool saved = prefs.putUChar("brightness", percent_) == sizeof(percent_);
    prefs.end();
    if (saved) {
      dirty_ = false;
      Serial.printf("Saved display brightness: %u%%\n", percent_);
    }
  }
  if (dirty_) changedAt_ = millis();
}
