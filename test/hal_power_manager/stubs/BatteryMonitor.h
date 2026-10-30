#pragma once
#include <Arduino.h>
class BatteryMonitor {
 public:
  static bool loadDesignCapacity() { return power_test::calibrationPending; }
  bool readPercentageChecked(uint16_t& out) const {
    ++power_test::gaugeReads;
    if (power_test::gaugeFails) return false;
    out = power_test::percentage;
    return true;
  }
  uint16_t readPercentage() const {
    ++power_test::gaugeReads;
    return power_test::percentage;
  }
};
