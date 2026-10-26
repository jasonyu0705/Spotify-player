#include "../src/backend/PlaybackState.h"

#include <cassert>
#include <cstdio>
#include <string>

struct Playback {
  std::string title = "Last song", artist = "Artist";
  const void *artImage = this;
  uint32_t progressMs = 10000, durationMs = 200000, fetchedAtMs = 1000;
  bool isPlaying = true, isLastPlayed = false;
};

int main() {
  Playback p;
  const void *cover = p.artImage;
  retainLastPlayback(p, 3500);
  assert(p.progressMs == 12500 && !p.isPlaying && p.isLastPlayed);
  assert(p.title == "Last song" && p.artist == "Artist" && p.artImage == cover);
  retainLastPlayback(p, 90000);
  assert(p.progressMs == 12500);  // repeated empty polls must not move the clock
  p.isPlaying = true;
  p.progressMs = 199500;
  p.fetchedAtMs = 90000;
  retainLastPlayback(p, 91000);
  assert(p.progressMs == p.durationMs);
  p.isPlaying = true;
  p.progressMs = 1000;
  p.fetchedAtMs = UINT32_MAX - 49;
  retainLastPlayback(p, 50);
  assert(p.progressMs == 1100);  // millis() rollover
  Playback paused;
  paused.isPlaying = false;
  retainLastPlayback(paused, 100000);
  assert(paused.progressMs == 10000);
  std::puts("PASS: idle retains song/cover, freezes progress, clamps duration, handles rollover");
}
