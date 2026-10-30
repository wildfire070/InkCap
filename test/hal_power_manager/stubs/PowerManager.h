#pragma once
namespace freeink {
struct PowerManager {
  static void powerDownRailsForSleep() {}
  static void waitForPowerButtonRelease() {}
  static void armPowerButtonWakeup() {}
};
}  // namespace freeink
