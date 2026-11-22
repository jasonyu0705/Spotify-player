#include "ui/PhotoFrame.h"

#include <LittleFS.h>
#include <TJpg_Decoder.h>
#include <algorithm>
#include <time.h>
#include "config.h"
#include "ui/Theme.h"

namespace {
constexpr int CLOCK_MARGIN = 8;
constexpr int CLOCK_WIDTH = 148;
constexpr int CLOCK_HEIGHT = 40;
TFT_eSPI *photoDisplay = nullptr;

bool outputPhotoBlock(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *pixels) {
  if (!photoDisplay || x < 0 || y < 0 || x + w > photoDisplay->width() ||
      y + h > photoDisplay->height()) return false;
  photoDisplay->pushImage(x, y, w, h, pixels);
  return true;
}
}  // namespace

void PhotoFrame::begin() {
  // Never format on mount failure: an absent image should prompt uploadfs,
  // and a damaged filesystem must not silently erase the user's photos.
  mounted_ = LittleFS.begin(false);
  if (!mounted_) {
    Serial.println("Photos: upload the LittleFS image with pio run -e controller -t uploadfs");
    return;
  }
  File directory = LittleFS.open("/photos");
  if (!directory || !directory.isDirectory()) {
    Serial.println("Photos: /photos is empty or missing");
    return;
  }
  for (File entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    String name = entry.name();
    String lower = name;
    lower.toLowerCase();
    if (!entry.isDirectory() && !name.startsWith(".") &&
        (lower.endsWith(".jpg") || lower.endsWith(".jpeg"))) {
      if (count_ < MAX_PHOTOS) photos_[count_++] = String("/photos/") + name;
      else Serial.printf("Photos: ignoring %s (limit %u)\n", name.c_str(), unsigned(MAX_PHOTOS));
    }
    entry.close();
  }
  std::sort(photos_, photos_ + count_, [](const String &a, const String &b) { return a < b; });
  Serial.printf("Photos: %u found; flash filesystem %u / %u bytes used\n", unsigned(count_),
                unsigned(LittleFS.usedBytes()), unsigned(LittleFS.totalBytes()));
}

bool PhotoFrame::clockContains(uint16_t x, uint16_t y) const {
  const int left = tft_.width() - CLOCK_MARGIN - CLOCK_WIDTH;
  return x >= left && x < left + CLOCK_WIDTH &&
         y >= CLOCK_MARGIN && y < CLOCK_MARGIN + CLOCK_HEIGHT;
}

void PhotoFrame::show() {
  drawPhoto();
}

void PhotoFrame::next() {
  if (count_) index_ = (index_ + 1) % count_;
  drawPhoto();
}

void PhotoFrame::tick() {
  if (count_ > 1 && uint32_t(millis() - shownAt_) >= PHOTO_INTERVAL_MS) next();
  drawClock(false);
}

void PhotoFrame::showMessage(const char *title, const char *detail) {
  tft_.fillScreen(theme::BG);
  tft_.setTextPadding(0);
  tft_.setTextDatum(MC_DATUM);
  tft_.setTextColor(theme::TEXT_PRIMARY, theme::BG);
  tft_.drawString(title, tft_.width() / 2, 120, 4);
  tft_.setTextColor(theme::TEXT_SECONDARY, theme::BG);
  tft_.drawString(detail, tft_.width() / 2, 157, 2);
  tft_.drawString("Swipe left or right for Spotify", tft_.width() / 2, 210, 2);
}

void PhotoFrame::drawPhoto() {
  if (!mounted_ || !count_) {
    showMessage("Add your photos", "Prepare photos, then upload the filesystem");
  } else {
    const String &path = photos_[index_];
    uint16_t width = 0, height = 0;
    // The desktop helper resizes and encodes baseline JPEGs. Reject oversized
    // manual additions instead of overrunning the display or using extra RAM.
    JRESULT result = TJpgDec.getFsJpgSize(&width, &height, path, LittleFS);
    if (result == JDR_OK && width && height && width <= tft_.width() && height <= tft_.height()) {
      tft_.fillScreen(theme::BG);
      photoDisplay = &tft_;
      TJpgDec.setJpgScale(1);
      TJpgDec.setSwapBytes(false);
      TJpgDec.setCallback(outputPhotoBlock);
      const bool oldSwap = tft_.getSwapBytes();
      tft_.setSwapBytes(true);
      result = TJpgDec.drawFsJpg((tft_.width() - width) / 2, (tft_.height() - height) / 2,
                               path, LittleFS);
      tft_.setSwapBytes(oldSwap);
      photoDisplay = nullptr;
      if (result != JDR_OK) showMessage("Photo unavailable", "Tap for the next photo");
    } else {
      showMessage("Photo needs preparation", "Run tools/prepare_photos.py, then uploadfs");
    }
    Serial.printf("Photo %u/%u: %s (%ux%u, decoder %d)\n", unsigned(index_ + 1),
                  unsigned(count_), path.c_str(), width, height, int(result));
  }
  shownAt_ = millis();
  drawClock(true);
}

void PhotoFrame::drawClock(bool force) {
  const time_t now = time(nullptr);
  const bool synced = now >= 1700000000;
  const int64_t minute = synced ? int64_t(now) / 60 : -1;
  if (!force && minute == lastClockMinute_) return;
  lastClockMinute_ = minute;
  char text[12] = "--:--";
  if (synced) {
    struct tm local;
    localtime_r(&now, &local);
    const int hour = local.tm_hour % 12;
    snprintf(text, sizeof(text), "%d:%02d %s", hour ? hour : 12, local.tm_min,
             local.tm_hour < 12 ? "AM" : "PM");
  }
  // An opaque, fixed-size badge keeps time legible on any photo. Redraw just
  // this badge once a minute; do not decode the whole photo for clock updates.
  const int x = tft_.width() - CLOCK_MARGIN - CLOCK_WIDTH;
  tft_.fillRoundRect(x, CLOCK_MARGIN, CLOCK_WIDTH, CLOCK_HEIGHT, 6, theme::BG);
  tft_.setTextPadding(0);
  tft_.setTextSize(1);
  tft_.setTextDatum(MC_DATUM);
  tft_.setTextColor(theme::TEXT_PRIMARY, theme::BG);
  tft_.drawString(text, x + CLOCK_WIDTH / 2, CLOCK_MARGIN + CLOCK_HEIGHT / 2, 4);
}
