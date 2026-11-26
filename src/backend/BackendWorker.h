#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "backend/PlayerBackend.h"

// Runs a PlayerBackend on its own FreeRTOS task (core 0), so slow network calls
// never block drawing or touch. The UI sends commands and picks up the latest
// state; it never calls the backend directly.
class BackendWorker {
 public:
  enum class CommandType : uint8_t { Play, Pause, Next, Previous, SetShuffle, SetRepeat, SetPolling };

  struct Update {
    BackendStatus status = BackendStatus::Ok;  // result of the latest poll
    NowPlaying nowPlaying;                     // valid when status is Ok
    IdleInfo idleInfo;                         // valid even when Spotify is idle/unavailable
    bool commandFailed = false;                // a command failed since the last update
    BackendStatus commandStatus = BackendStatus::Ok;
  };

  BackendWorker(PlayerBackend &backend, uint32_t pollIntervalMs)
      : backend_(backend), pollIntervalMs_(pollIntervalMs) {}

  void start();

  // Queues a command; returns false if the queue is full. Never blocks.
  bool send(CommandType type, uint8_t value = 0);

  // Copies the latest state into `out` if it changed since the last call.
  bool takeUpdate(Update &out);

 private:
  struct CommandMsg {
    CommandType type;
    uint8_t value;
    uint32_t queuedAtMs;
  };

  static void taskEntry(void *self);
  void run();
  BackendStatus execute(const CommandMsg &cmd);

  PlayerBackend &backend_;
  uint32_t pollIntervalMs_;
  QueueHandle_t queue_ = nullptr;
  SemaphoreHandle_t lock_ = nullptr;
  Update latest_;
  bool hasNew_ = false;
};
