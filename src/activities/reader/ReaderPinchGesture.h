#pragma once

#include <cstdint>

// Fixed-size one-step recognizer. It is intentionally independent from input,
// fonts, and time so it can be host-tested without reader activity state.
class ReaderPinchGesture {
 public:
  enum class Action : uint8_t { None, Increase, Decrease };

  static constexpr uint8_t MIN_DISTANCE_CHANGE_PX = 20;
  static constexpr uint8_t SCALE_PERCENT = 110;

  bool isActive() const { return active_; }

  void reset() {
    active_ = false;
    stepApplied_ = false;
    startDistanceSq_ = 0;
  }

  // Returns exactly one action per two-contact sequence. Call reset when the
  // touch controller reports fewer than two contacts.
  Action update(const int x1, const int y1, const int x2, const int y2) {
    const uint32_t distanceSq = separationSq(x1, y1, x2, y2);
    if (!active_) {
      active_ = true;
      stepApplied_ = false;
      startDistanceSq_ = distanceSq;
      return Action::None;
    }
    if (stepApplied_) return Action::None;

    constexpr uint64_t scaleSquared = static_cast<uint64_t>(SCALE_PERCENT) * SCALE_PERCENT;
    constexpr uint64_t percentSquared = 100ULL * 100ULL;
    constexpr uint64_t minDistanceChangeSq = static_cast<uint64_t>(MIN_DISTANCE_CHANGE_PX) * MIN_DISTANCE_CHANGE_PX;

    const uint64_t current = distanceSq;
    const uint64_t start = startDistanceSq_;
    if (current > start && current * percentSquared >= start * scaleSquared && current - start >= minDistanceChangeSq) {
      stepApplied_ = true;
      return Action::Increase;
    }
    if (current < start && current * scaleSquared <= start * percentSquared && start - current >= minDistanceChangeSq) {
      stepApplied_ = true;
      return Action::Decrease;
    }
    return Action::None;
  }

 private:
  static uint32_t separationSq(const int x1, const int y1, const int x2, const int y2) {
    const int64_t dx = static_cast<int64_t>(x2) - x1;
    const int64_t dy = static_cast<int64_t>(y2) - y1;
    return static_cast<uint32_t>(dx * dx + dy * dy);
  }

  bool active_ = false;
  bool stepApplied_ = false;
  uint32_t startDistanceSq_ = 0;
};
