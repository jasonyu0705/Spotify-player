#include "../src/backend/ArtTransfer.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

struct FakeStream {
  std::vector<uint8_t> bytes;
  size_t offset = 0;
  int batch = 2048;
  bool open = true, readError = false, noProgress = false;
  explicit FakeStream(size_t size) : bytes(size, 0x55) {}
  int available() { return std::min<size_t>(bytes.size() - offset, batch); }
  bool connected() { return open; }
  int read(uint8_t *out, size_t count) {
    assert(count <= 2048 && count <= size_t(available()));
    if (readError) return -1;
    if (noProgress) return 0;
    std::memcpy(out, bytes.data() + offset, count);
    offset += count;
    return int(count);
  }
};

int main() {
  uint8_t buffer[5002] = {};
  uint32_t now = 0;
  auto clock = [&]() { return now; };
  auto noCancel = []() { return false; };

  // An 8-second handshake must not consume the body budget. This transfer then
  // needs another 7 seconds of steady progress and must still finish.
  now = 8000;
  FakeStream slow(64);
  slow.batch = 8;
  auto result = readArtBody(slow, buffer, 64, clock, [&]() { now += 1000; }, noCancel);
  assert(result.status == ArtReadStatus::Complete && result.received == 64);

  // Bound reads and never overwrite either guard byte, including the last chunk.
  FakeStream large(6000);
  large.batch = 6000;
  buffer[0] = 0xa5;
  buffer[5001] = 0x5a;
  result = readArtBody(large, buffer + 1, 5000, clock, [&]() { ++now; }, noCancel);
  assert(result.status == ArtReadStatus::Complete && result.received == 5000);
  assert(buffer[0] == 0xa5 && buffer[5001] == 0x5a);

  FakeStream cancelled(5000);
  uint32_t cancelAt = now + 1;
  result = readArtBody(cancelled, buffer, 5000, clock, [&]() { ++now; },
                       [&]() { return now >= cancelAt; });
  assert(result.status == ArtReadStatus::Cancelled && result.received == 2048);

  FakeStream stalled(0);
  result = readArtBody(stalled, buffer, 10, clock, [&]() { now += 1000; }, noCancel);
  assert(result.status == ArtReadStatus::Stalled && result.received == 0);

  FakeStream zeroReads(10);
  zeroReads.noProgress = true;
  result = readArtBody(zeroReads, buffer, 10, clock, [&]() { now += 1000; }, noCancel);
  assert(result.status == ArtReadStatus::Stalled);

  FakeStream dripping(20);
  dripping.batch = 1;
  result = readArtBody(dripping, buffer, 20, clock, [&]() { now += 1000; }, noCancel);
  assert(result.status == ArtReadStatus::TimedOut && result.received == 12);

  FakeStream disconnected(5);
  disconnected.open = false;
  result = readArtBody(disconnected, buffer, 10, clock, [&]() { ++now; }, noCancel);
  assert(result.status == ArtReadStatus::Disconnected && result.received == 5);

  FakeStream broken(10);
  broken.readError = true;
  result = readArtBody(broken, buffer, 10, clock, [&]() { ++now; }, noCancel);
  assert(result.status == ArtReadStatus::ReadError);

  now = UINT32_MAX - 2000;
  FakeStream wrapped(32);
  wrapped.batch = 8;
  result = readArtBody(wrapped, buffer, 32, clock, [&]() { now += 1000; }, noCancel);
  assert(result.status == ArtReadStatus::Complete);

  ArtRetry retry;
  assert(retry.ready(now));
  for (uint32_t delay : {2000u, 5000u, 15000u, 30000u, 30000u}) {
    retry.failed(now);
    assert(!retry.ready(now + delay - 1) && retry.ready(now + delay));
    now += delay;
  }
  const uint8_t failures = retry.failures;
  retry.cancelled(now);
  assert(retry.failures == failures && !retry.ready(now + 499) && retry.ready(now + 500));
  retry.reset();
  assert(retry.failures == 0 && retry.ready(now));
  now = UINT32_MAX - 1000;
  retry.failed(now);
  assert(!retry.ready(now + 1999) && retry.ready(now + 2000));

  std::puts("PASS: artwork body timing, bounds, cancellation, failures, retries, and rollover");
}
