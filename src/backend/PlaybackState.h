#pragma once

#include <stdint.h>

// Stop the clock once when current playback becomes unavailable. Metadata and
// the shared cover stay intact, and repeated idle polls cannot advance progress.
template <typename Playback>
void retainLastPlayback(Playback &playback, uint32_t now) {
  if (playback.isPlaying) {
    uint64_t progress = uint64_t(playback.progressMs) + uint32_t(now - playback.fetchedAtMs);
    playback.progressMs = progress < playback.durationMs ? uint32_t(progress) : playback.durationMs;
  }
  playback.isPlaying = false;
  playback.isLastPlayed = true;
  playback.fetchedAtMs = now;
}
