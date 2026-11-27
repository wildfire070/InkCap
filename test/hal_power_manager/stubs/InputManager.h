#pragma once
#include <cstdint>
class InputManager {
 public:
  static constexpr uint8_t MAX_TOUCH_CONTACTS = 5;
  bool isPowerButtonPhysicallyPressed() const { return false; }
  bool isAdcLadderPhysicallyPressed() { return false; }
};
