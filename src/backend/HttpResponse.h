#pragma once

#include <cstring>

// HTTPClient in Arduino ESP32 2.x can wait forever when getString() sees
// unknown length (-1) on a persistent connection. Responses defined to have
// no body must bypass that reader, even without a Content-Length header.
template <typename Http>
auto readHttpResponseBody(Http &http, const char *method, int code) -> decltype(http.getString()) {
  if (code < 200 || code == 204 || code == 304 || std::strcmp(method, "HEAD") == 0) return {};
  return http.getString();
}
