#include <cassert>
#include <initializer_list>
#include "../src/ui/DashboardPage.h"

int main() {
  assert(nextDashboardPage(DashboardPage::Player, true, true) == DashboardPage::Photos);
  assert(nextDashboardPage(DashboardPage::Photos, true, true) == DashboardPage::Telemetry);
  assert(nextDashboardPage(DashboardPage::Telemetry, true, true) == DashboardPage::Player);
  assert(nextDashboardPage(DashboardPage::Player, false, true) == DashboardPage::Telemetry);
  assert(nextDashboardPage(DashboardPage::Telemetry, false, true) == DashboardPage::Photos);
  assert(nextDashboardPage(DashboardPage::Photos, false, true) == DashboardPage::Player);
  for (bool forward : {false, true}) {
    assert(nextDashboardPage(DashboardPage::Player, forward, false) == DashboardPage::Photos);
    assert(nextDashboardPage(DashboardPage::Photos, forward, false) == DashboardPage::Player);
  }
}
