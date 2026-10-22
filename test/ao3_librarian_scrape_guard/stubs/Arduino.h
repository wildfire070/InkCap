#pragma once

#include <cstdint>

inline uint32_t millis() {
  static uint32_t clock = 0;
  return ++clock;
}

inline void yield() {}

struct FakeEsp {
  uint32_t getFreeHeap() const { return 100000; }
  uint32_t getMaxAllocHeap() const { return 80000; }
};

inline FakeEsp ESP;
