#pragma once

#include <TFT_eSPI.h>
#include "backend/IdleInfo.h"

// The UI only draws cached readings supplied by the backend worker.
class TelemetryPage {
 public:
  explicit TelemetryPage(TFT_eSPI &tft) : tft_(tft) {}
  void setInfo(const IdleInfo &info) { info_ = info; }
  void show();
  void tick();  // only while this page is visible

 private:
  void draw(bool force);
  void card(int index, const char *title, const String &value, const String &detail, bool force);
  TFT_eSPI &tft_;
  IdleInfo info_;
  String values_[6], details_[6];
  int lastConnection_ = -1, lastAvailability_ = -1;
  uint32_t lastDrawMs_ = 0;
};
