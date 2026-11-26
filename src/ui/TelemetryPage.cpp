#include "ui/TelemetryPage.h"

#include "net/Network.h"
#include "ui/Text.h"
#include "ui/Theme.h"

using namespace layout;
using namespace theme;

namespace {
constexpr int CARD_MARGIN = 12;
constexpr int CARD_GAP = 12;
constexpr int CARD_W = (W - 2 * CARD_MARGIN - CARD_GAP) / 2;
constexpr int CARD_H = 84;
constexpr int CARD_TOP = 44;
constexpr int CARD_ROW_GAP = 6;

String amount(uint64_t bytes, uint64_t unit, const char *suffix) {
  return String(double(bytes) / double(unit), bytes >= unit * 100 ? 0 : 1) + suffix;
}

String uptime(uint64_t seconds) {
  const uint32_t days = seconds / 86400;
  const uint32_t hours = (seconds / 3600) % 24;
  const uint32_t minutes = (seconds / 60) % 60;
  if (days) return String(days) + "d " + String(hours) + "h";
  if (hours) return String(hours) + "h " + String(minutes) + "m";
  return String(minutes) + "m";
}

String spotifyState(const TelemetryInfo &info) {
  if (info.spotifyStatus == "ok") return info.isPlaying ? "Playing" : "Paused";
  if (info.spotifyStatus == "polling_disabled") return "OFF";
  if (info.spotifyStatus == "rate_limited") return "Rate limit";
  if (info.spotifyStatus == "nothing_playing" || info.spotifyStatus == "no_active_device") return "Idle";
  if (info.spotifyStatus == "not_authorized") return "Not linked";
  if (info.spotifyStatus == "unsupported") return "Unavailable";
  return "Retrying";
}
}  // namespace

void TelemetryPage::show() {
  tft_.fillScreen(BG);
  lastDrawMs_ = 0;
  draw(true);
}

void TelemetryPage::tick() {
  if (uint32_t(millis() - lastDrawMs_) >= 1000) draw(false);
}

void TelemetryPage::card(int index, const char *title, const String &value, const String &detail, bool force) {
  if (!force && values_[index] == value && details_[index] == detail) return;
  values_[index] = value;
  details_[index] = detail;
  const int x = CARD_MARGIN + (index % 2) * (CARD_W + CARD_GAP);
  const int y = CARD_TOP + (index / 2) * (CARD_H + CARD_ROW_GAP);
  const uint16_t surface = rgb565(28, 28, 28);
  tft_.fillRoundRect(x, y, CARD_W, CARD_H, 6, surface);
  tft_.setTextDatum(TL_DATUM);
  tft_.setTextColor(TEXT_SECONDARY, surface);
  tft_.drawString(title, x + 12, y + 7, 2);
  tft_.setTextColor(TEXT_PRIMARY, surface);
  tft_.drawString(fitText(tft_, value, 4, CARD_W - 24), x + 12, y + 29, 4);
  tft_.setTextColor(TEXT_TERTIARY, surface);
  tft_.drawString(fitText(tft_, detail, 2, CARD_W - 24), x + 12, y + 62, 2);
}

void TelemetryPage::draw(bool force) {
  lastDrawMs_ = millis();
  tft_.setTextPadding(0);
  tft_.setTextSize(1);
  const bool online = net::connected() && info_.piOnline && uint32_t(millis() - info_.lastPiReplyMs) < 45000;
  const int connection = int(online) | (int(info_.piAuthError) << 1);
  if (force || connection != lastConnection_) {
    lastConnection_ = connection;
    tft_.fillRect(0, 0, W, 36, BG);
    tft_.setTextDatum(ML_DATUM);
    tft_.setTextColor(TEXT_PRIMARY, BG);
    tft_.drawString("PI TELEMETRY", MARGIN, 18, 4);
    tft_.setTextDatum(MR_DATUM);
    tft_.setTextColor(online ? ACCENT : ERROR_RED, BG);
    tft_.drawString(online ? "Pi online" : info_.piAuthError ? "Pi auth error" : "Pi offline", W - MARGIN, 18, 2);
  }
  const TelemetryInfo &health = info_.telemetry;
  const int availability = !health.supported || health.status == "not_configured" ? 0 :
                           health.available ? 3 : health.status == "loading" ? 1 : 2;
  if (availability != lastAvailability_) {
    lastAvailability_ = availability;
    force = true;
  }
  if (force) tft_.fillRect(0, 40, W, H - 40, BG);
  if (availability != 3) {
    if (force) {
      tft_.setTextDatum(MC_DATUM);
      tft_.setTextColor(TEXT_PRIMARY, BG);
      tft_.drawString(availability == 0 ? "Update the Pi bridge" : availability == 1 ? "Reading Pi telemetry..." :
                      "Telemetry unavailable", W / 2, 137, 4);
      tft_.setTextColor(TEXT_SECONDARY, BG);
      tft_.drawString(availability == 0 ? "Install the updated Python files on the Pi" : "Waiting for local system readings",
                      W / 2, 173, 2);
    }
  } else {
    card(0, "CPU temperature", health.hasTemperature ? String(health.temperatureC, 1) + " C" : "--",
         "Processor temperature", force);
    card(1, "CPU usage", health.hasCpuUsage ? String(health.cpuPercent, 0) + "%" : "Sampling...",
         health.hasCpuUsage ? "Average over the last minute" : "First average after one minute", force);
    card(2, "Memory used", health.hasMemory ? String(double(health.memoryUsed) * 100 / health.memoryTotal, 0) + "%" : "--",
         health.hasMemory ? amount(health.memoryUsed, 1024 * 1024, "") + " / " +
                            amount(health.memoryTotal, 1024 * 1024, " MiB") : "Reading unavailable", force);
    card(3, "SSD free", health.hasSsdSpace ? amount(health.ssdFree, uint64_t(1024) * 1024 * 1024, " GiB") :
         health.ssdStatus == "unmounted" ? "Not mounted" : "Unavailable",
         health.hasSsdSpace ? "of " + amount(health.ssdTotal, uint64_t(1024) * 1024 * 1024, " GiB") : "Check SSD connection and mount",
         force);
    card(4, "Pi uptime", health.hasUptime ? uptime(health.uptimeSeconds) : "--", "Since the Pi last booted", force);
    card(5, "Spotify bridge", spotifyState(health), online ? "Bridge connected" : "Last received status", force);
  }
}
