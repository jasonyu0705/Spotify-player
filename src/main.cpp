// Spotify ESP controller.
//
// Hold the screen while pressing RESET, hold BOOT for 1 second, or send 'c' on
// serial to recalibrate touch.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <TouchCalibrator.h>
#include <esp_system.h>

#include "backend/BackendWorker.h"
#include "backend/PlaybackState.h"
#include "config.h"
#include "ui/PlayerUI.h"
#include "ui/Backlight.h"
#include "ui/PhotoFrame.h"
#include "ui/TouchGesture.h"

#if !__has_include("secrets.h")
#error "Copy include/secrets.h.example to include/secrets.h and fill it in (see README.md)"
#endif
#include "secrets.h"
#ifdef USE_PI_BACKEND
#include "backend/PiBackend.h"
#ifndef PI_SERVER_URL
#define PI_SERVER_URL "http://raspberrypi.local:8765"
#endif
#ifndef PI_API_TOKEN
#define PI_API_TOKEN ""
#endif
#else
#include "backend/SpotifyBackend.h"
#endif
#include "net/Network.h"

using Command = BackendWorker::CommandType;

TFT_eSPI tft;
PlayerUI ui(tft);
Backlight backlight;
PhotoFrame photoFrame(tft);
TouchGesture gesture;
bool showingPhotos = false;
#ifdef USE_PI_BACKEND
PiBackend backend(PI_SERVER_URL, PI_API_TOKEN);
#else
SpotifyBackend backend(SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET, SPOTIFY_REFRESH_TOKEN);
#endif
BackendWorker worker(backend, POLL_INTERVAL_MS);

NowPlaying nowPlaying;  // latest state, plus any instant feedback applied locally
BackendStatus lastStatus = BackendStatus::Ok;
bool havePlayback = false;  // a track has loaded at least once since startup
bool showingClock = false;

// Resistive touch flickers off for a few ms mid-press; treat short gaps as still
// pressed so one tap doesn't register twice.
bool readTouch(uint16_t &x, uint16_t &y) {
  static bool pressed = false;
  static uint16_t lastX = 0, lastY = 0;
  static uint32_t lastSeenMs = 0;

  uint16_t tx, ty;
  bool valid = false;
  if (tft.getTouchRawZ() > touchcal::Z_THRESHOLD) {
    uint16_t x1, y1, x2, y2;
    tft.getTouchRaw(&x1, &y1);
    delay(1);
    tft.getTouchRaw(&x2, &y2);
    if (tft.getTouchRawZ() > touchcal::Z_THRESHOLD &&
        abs(int(x1) - x2) <= 20 && abs(int(y1) - y2) <= 20) {
      tx = (uint32_t(x1) + x2) / 2;
      ty = (uint32_t(y1) + y2) / 2;
      tft.convertRawXY(&tx, &ty);
      valid = tx < tft.width() && ty < tft.height();
    }
  }
  if (valid) {
    lastX = tx;
    lastY = ty;
    lastSeenMs = millis();
    pressed = true;
  } else if (pressed && millis() - lastSeenMs > TOUCH_RELEASE_MS) {
    pressed = false;
  }
  x = lastX;
  y = lastY;
  return pressed;
}

void recalibrateIfRequested() {
  static bool bootHeld = false, bootHandled = false;
  static uint32_t bootDownSince = 0;
  bool requested = false;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'c' || c == 'C') requested = true;
  }
  if (digitalRead(BOOT_BUTTON) == LOW) {
    if (!bootHeld) {
      bootHeld = true;
      bootDownSince = millis();
    }
    if (!bootHandled && millis() - bootDownSince >= 1000) {
      bootHandled = true;  // don't repeat until released
      requested = true;
    }
  } else {
    bootHeld = bootHandled = false;
  }

  if (requested) {
    touchcal::loadOrCalibrate(tft, SCREEN_ROTATION, true);
    gesture.reset();
    ui.showMessage("Calibrated", "Loading player...");
    if (showingPhotos) photoFrame.show();
    else if (showingClock) ui.showClock();
    else if (havePlayback) ui.update(nowPlaying);
  }
}

void applyUpdate(const BackendWorker::Update &update) {
  ui.setIdleInfo(update.idleInfo);
  bool idle = update.status == BackendStatus::NothingPlaying ||
              update.status == BackendStatus::NoActiveDevice;
  if (update.commandFailed) {
    Serial.printf("Command failed: %s\n", statusTitle(update.commandStatus));
    ui.showNotice(statusTitle(update.commandStatus));
  }

  bool statusChanged = update.status != lastStatus;
  if (statusChanged) {
    Serial.printf("Backend status: %s\n", statusTitle(update.status));
    lastStatus = update.status;
  }

  if (update.status == BackendStatus::Ok) {
    nowPlaying = update.nowPlaying;
    havePlayback = true;
    showingClock = false;  // a loaded track stays visible even while paused
    ui.update(nowPlaying);
  } else if (idle) {
    if (havePlayback) retainLastPlayback(nowPlaying, millis());
    showingClock = true;
    ui.showClock();
  } else if (showingClock || !havePlayback) {
    // Without a loaded track, every backend error falls back to the clock.
    // Spotify authorization/Premium failures must not hide the local time.
    showingClock = true;
    ui.showClock();
    if (statusChanged) ui.showNotice(statusTitle(update.status));
  } else if (havePlayback) {
    retainLastPlayback(nowPlaying, millis());
    ui.update(nowPlaying);
    if (statusChanged) {
      ui.showNotice(statusTitle(update.status));
    }
  } else {
    ui.showStatus(update.status);
  }
}

// Shows the expected result right away; the next poll confirms or corrects it.
void applyInstantFeedback(UiAction action) {
  if (action == UiAction::PlayPause) {
    uint32_t now = millis();
    if (nowPlaying.isPlaying) nowPlaying.progressMs += now - nowPlaying.fetchedAtMs;
    nowPlaying.progressMs = min(nowPlaying.progressMs, nowPlaying.durationMs);
    nowPlaying.fetchedAtMs = now;
    nowPlaying.isPlaying = !nowPlaying.isPlaying;
  } else if (action == UiAction::ToggleShuffle) {
    nowPlaying.shuffle = !nowPlaying.shuffle;
  } else if (action == UiAction::CycleRepeat) {
    nowPlaying.repeat = nextRepeatMode(nowPlaying.repeat);
  } else {
    return;
  }
  showingClock = false;
  ui.update(nowPlaying);
}

void runAction(UiAction action) {
  bool queued = true;
  switch (action) {
    case UiAction::None: return;
    case UiAction::WeatherInfo:
      ui.showNotice("Weather: open-meteo.com | CC BY 4.0", false);
      return;
    case UiAction::CycleBrightness:
      backlight.cycle();
      ui.setBrightness(backlight.percent());
      return;
    case UiAction::PlayPause:
      queued = worker.send(nowPlaying.isPlaying ? Command::Pause : Command::Play);
      break;
    case UiAction::Next: queued = worker.send(Command::Next); break;
    case UiAction::Previous: queued = worker.send(Command::Previous); break;
    case UiAction::ToggleShuffle: queued = worker.send(Command::SetShuffle, !nowPlaying.shuffle); break;
    case UiAction::CycleRepeat:
      queued = worker.send(Command::SetRepeat, static_cast<uint8_t>(nextRepeatMode(nowPlaying.repeat)));
      break;
  }
  if (queued) {
    applyInstantFeedback(action);
  } else {
    ui.showNotice("Busy - try again");
  }
}

void switchScreen() {
  showingPhotos = !showingPhotos;
  ui.setVisible(!showingPhotos);
  if (showingPhotos) {
    photoFrame.show();
  } else if (showingClock) {
    ui.showClock();
  } else if (havePlayback) {
    ui.update(nowPlaying);
  } else if (lastStatus != BackendStatus::Ok) {
    ui.showStatus(lastStatus);
  } else {
    ui.showMessage("Connecting to Spotify...", "");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  delay(200);
  Serial.println();
  Serial.println("=== Spotify ESP controller ===");
  Serial.printf("Reset reason: %d\n", int(esp_reset_reason()));

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  backlight.begin();
  ui.setBrightness(backlight.percent());

  bool forceCalibration = touchcal::screenPressed(tft);
  if (forceCalibration) touchcal::waitForRelease(tft);
  touchcal::loadOrCalibrate(tft, SCREEN_ROTATION, forceCalibration);

  ui.begin(backend.name());
  ui.showMessage("Connecting to WiFi...", WIFI_SSID);
  if (net::begin(WIFI_SSID, WIFI_PASSWORD, 20000)) {
    ui.showMessage("Connecting to Spotify...", "");
  } else {
    ui.showMessage("WiFi not connected", "Still trying - check WiFi settings");
  }
  photoFrame.begin();
  backend.begin();
  worker.start();
  Serial.printf("Free heap %u, largest block %u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

void loop() {
  recalibrateIfRequested();

  uint16_t x, y;
  bool pressed = readTouch(x, y);
  TouchEvent event = gesture.update(pressed, x, y, millis());
  if (event == TouchEvent::SwipeLeft || event == TouchEvent::SwipeRight) {
    switchScreen();
  } else if (event == TouchEvent::Tap) {
    if (showingPhotos) {
      // The clock badge doubles as a brightness control.
      if (photoFrame.clockContains(gesture.tapX(), gesture.tapY())) {
        backlight.cycle();
        ui.setBrightness(backlight.percent());
      } else {
        photoFrame.next();
      }
    } else {
      runAction(ui.actionAt(gesture.tapX(), gesture.tapY()));
    }
  }

  BackendWorker::Update update;
  if (worker.takeUpdate(update)) applyUpdate(update);

  if (showingPhotos) photoFrame.tick();
  else ui.tick();
  backlight.tick();
  delay(5);
}
