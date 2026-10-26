#include "backend/BackendWorker.h"

namespace {
constexpr uint32_t TASK_STACK_BYTES = 16384;  // TLS handshakes need a deep stack
constexpr uint32_t POLL_AFTER_COMMAND_MS = 150;  // give Spotify a moment to apply it
constexpr uint32_t IDLE_POLL_MS = 3000;         // commands still wake the queue immediately
constexpr uint32_t NETWORK_RETRY_MS = 5000;
bool commandPending(void *queue) {
  return uxQueueMessagesWaiting(static_cast<QueueHandle_t>(queue)) != 0;
}
}  // namespace

void BackendWorker::start() {
  queue_ = xQueueCreate(8, sizeof(CommandMsg));
  lock_ = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(taskEntry, "backend", TASK_STACK_BYTES, this, 1, nullptr, 0);
}

bool BackendWorker::send(CommandType type, uint8_t value) {
  CommandMsg msg{type, value, millis()};
  return xQueueSend(queue_, &msg, 0) == pdTRUE;
}

bool BackendWorker::takeUpdate(Update &out) {
  if (xSemaphoreTake(lock_, 0) != pdTRUE) return false;  // worker is publishing; try next loop
  bool changed = hasNew_;
  if (changed) {
    out = latest_;
    hasNew_ = false;
    latest_.commandFailed = false;
  }
  xSemaphoreGive(lock_);
  return changed;
}

void BackendWorker::taskEntry(void *self) { static_cast<BackendWorker *>(self)->run(); }

BackendStatus BackendWorker::execute(const CommandMsg &cmd) {
  switch (cmd.type) {
    case CommandType::Play: return backend_.play();
    case CommandType::Pause: return backend_.pause();
    case CommandType::Next: return backend_.next();
    case CommandType::Previous: return backend_.previous();
    case CommandType::SetShuffle: return backend_.setShuffle(cmd.value != 0);
    case CommandType::SetRepeat: return backend_.setRepeat(static_cast<RepeatMode>(cmd.value));
  }
  return BackendStatus::Unsupported;
}

void BackendWorker::run() {
  uint32_t nextPollAt = millis();
  NowPlaying np;

  for (;;) {
    int32_t untilPoll = int32_t(nextPollAt - millis());
    CommandMsg cmd;
    if (xQueueReceive(queue_, &cmd, untilPoll > 0 ? pdMS_TO_TICKS(untilPoll) : 0) == pdTRUE) {
      uint32_t started = millis();
      BackendStatus result = execute(cmd);
      Serial.printf("Command %u: queued %lu ms, request %lu ms, status %u\n", unsigned(cmd.type),
                    (unsigned long)(started - cmd.queuedAtMs), (unsigned long)(millis() - started),
                    unsigned(result));
      if (result != BackendStatus::Ok) {
        xSemaphoreTake(lock_, portMAX_DELAY);
        latest_.commandFailed = true;
        latest_.commandStatus = result;
        hasNew_ = true;
        xSemaphoreGive(lock_);
      }
      nextPollAt = millis() + POLL_AFTER_COMMAND_MS;
      continue;
    }

    BackendStatus status = backend_.getNowPlaying(np);
    uint32_t interval = pollIntervalMs_;
    if (status == BackendStatus::NothingPlaying || status == BackendStatus::NoActiveDevice) {
      interval = max(interval, IDLE_POLL_MS);
    } else if (status == BackendStatus::NetworkError) {
      interval = max(interval, NETWORK_RETRY_MS);
    }
    nextPollAt = millis() + interval;

    // A poll that started before a command was queued shows the old state; drop it
    // rather than undo the UI's instant feedback. The poll after the command fixes it.
    if (uxQueueMessagesWaiting(queue_) > 0) continue;

    xSemaphoreTake(lock_, portMAX_DELAY);
    latest_.status = status;
    if (status == BackendStatus::Ok) latest_.nowPlaying = np;
    hasNew_ = true;
    xSemaphoreGive(lock_);

    if (status == BackendStatus::Ok && !commandPending(queue_) &&
        backend_.loadAlbumArt(np, commandPending, queue_) && !commandPending(queue_)) {
      xSemaphoreTake(lock_, portMAX_DELAY);
      latest_.nowPlaying = np;
      hasNew_ = true;
      xSemaphoreGive(lock_);
    }
  }
}
