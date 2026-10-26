#pragma once

#include <stddef.h>
#include <stdint.h>

enum class ArtReadStatus { Complete, Cancelled, Disconnected, Stalled, TimedOut, ReadError };
struct ArtReadResult {
  ArtReadStatus status;
  size_t received;
};

// Start timing when the body is ready, not before DNS/TLS/HTTP setup. Keep a
// bounded total wait, but let a slow transfer continue while bytes are arriving.
template <typename Stream, typename Clock, typename Yield, typename Cancel>
ArtReadResult readArtBody(Stream &stream, uint8_t *data, size_t length,
                          Clock clock, Yield yield, Cancel cancel) {
  const uint32_t started = clock();
  uint32_t lastData = started;
  size_t received = 0;
  while (received < length) {
    if (cancel()) return {ArtReadStatus::Cancelled, received};
    const uint32_t now = clock();
    if (uint32_t(now - started) >= 12000) return {ArtReadStatus::TimedOut, received};
    if (uint32_t(now - lastData) >= 4000) return {ArtReadStatus::Stalled, received};
    int available = stream.available();
    if (available > 0) {
      size_t count = size_t(available);
      if (count > length - received) count = length - received;
      if (count > 2048) count = 2048;
      int got = stream.read(data + received, count);
      if (got < 0) return {ArtReadStatus::ReadError, received};
      if (got > 0) {
        received += size_t(got);
        lastData = clock();
      }
    } else if (!stream.connected()) {
      return {ArtReadStatus::Disconnected, received};
    }
    yield();
  }
  return {ArtReadStatus::Complete, received};
}

struct ArtRetry {
  uint32_t retryAt = 0;
  uint8_t failures = 0;
  bool waiting = false;

  bool ready(uint32_t now) const { return !waiting || int32_t(now - retryAt) >= 0; }
  void reset() { retryAt = 0; failures = 0; waiting = false; }
  void failed(uint32_t now) {
    const uint32_t delays[] = {2000, 5000, 15000, 30000};
    retryAt = now + delays[failures < 4 ? failures : 3];
    if (failures < 4) ++failures;
    waiting = true;
  }
  void cancelled(uint32_t now) { retryAt = now + 500; waiting = true; }
};
