#pragma once

#include <stdint.h>

enum class DashboardPage : uint8_t { Player, Photos, Telemetry };

inline DashboardPage nextDashboardPage(DashboardPage current, bool forward, bool piMode) {
  const uint8_t count = piMode ? 3 : 2;
  const uint8_t index = static_cast<uint8_t>(current);
  return static_cast<DashboardPage>((index + (forward ? 1 : count - 1)) % count);
}
