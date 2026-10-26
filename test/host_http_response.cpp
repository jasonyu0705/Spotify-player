#include "../src/backend/HttpResponse.h"

#include <cassert>
#include <cstdio>
#include <string>

struct FakeHttp {
  bool noBodyOnPersistentConnection = false;
  int reads = 0;
  std::string getString() {
    // Models the ESP32 library's hang on a bodyless keep-alive response.
    assert(!noBodyOnPersistentConnection);
    ++reads;
    return "{\"is_playing\":true}";
  }
};

int main() {
  FakeHttp empty;
  empty.noBodyOnPersistentConnection = true;
  assert(readHttpResponseBody(empty, "GET", 204).empty());
  assert(readHttpResponseBody(empty, "PUT", 204).empty());
  assert(readHttpResponseBody(empty, "POST", 204).empty());
  assert(readHttpResponseBody(empty, "GET", 304).empty());
  assert(readHttpResponseBody(empty, "HEAD", 200).empty());
  assert(readHttpResponseBody(empty, "GET", -1).empty());
  assert(readHttpResponseBody(empty, "GET", 100).empty());
  assert(empty.reads == 0);
  FakeHttp data;
  assert(!readHttpResponseBody(data, "GET", 200).empty());
  assert(!readHttpResponseBody(data, "POST", 400).empty());
  assert(!readHttpResponseBody(data, "PUT", 403).empty());
  assert(data.reads == 3);
  std::puts("PASS: bodyless persistent responses skip the reader; JSON and errors are read");
}
