#include "../src/backend/RepeatMode.h"

#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
  RepeatMode mode = RepeatMode::Off;
  mode = nextRepeatMode(mode);
  assert(mode == RepeatMode::All);
  assert(std::strcmp(spotifyRepeatState(mode), "context") == 0);
  mode = nextRepeatMode(mode);
  assert(mode == RepeatMode::One);
  assert(std::strcmp(spotifyRepeatState(mode), "track") == 0);
  mode = nextRepeatMode(mode);
  assert(mode == RepeatMode::Off);
  assert(std::strcmp(spotifyRepeatState(mode), "off") == 0);
  const RepeatMode modes[] = {RepeatMode::Off, RepeatMode::All, RepeatMode::One};
  for (RepeatMode value : modes) {
    assert(repeatModeFromSpotify(spotifyRepeatState(value)) == value);
  }
  assert(repeatModeFromSpotify(nullptr) == RepeatMode::Off);
  assert(repeatModeFromSpotify("unknown") == RepeatMode::Off);
  std::puts("PASS: repeat cycle and Spotify command/playback state mappings");
}
