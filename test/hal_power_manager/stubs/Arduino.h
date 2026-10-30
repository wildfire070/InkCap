#pragma once
#include <cstdint>
#include <initializer_list>
#include <mutex>
namespace power_test {
inline unsigned long now = 1;
inline int frequency = 160;
inline bool frequencyFails = false;
inline unsigned gaugeReads = 0;
inline uint16_t percentage = 80;
inline bool gaugeFails = false;
inline bool calibrationPending = false;
}  // namespace power_test
inline unsigned long millis() { return power_test::now; }
inline void delay(unsigned long ms) { power_test::now += ms; }
inline int getCpuFrequencyMhz() { return power_test::frequency; }
inline bool setCpuFrequencyMhz(int value) {
  if (power_test::frequencyFails) return false;
  power_test::frequency = value;
  return true;
}
constexpr int INPUT = 0;
inline void pinMode(int, int) {}
