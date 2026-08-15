#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Allocation-free decoder and evaluator for settings-driven multi-touch scalar
// adjustments. Coordinates must already be in logical screen space.
namespace SwipeAdjustment {

enum class Axis : uint8_t { Vertical, Horizontal };
enum class Target : uint8_t { Brightness, Warmth };
enum class Binding : uint8_t {
  Off = 0,
  TwoFingerVertical,
  TwoFingerHorizontal,
  ThreeFingerVertical,
  ThreeFingerHorizontal,
  FourFingerVertical,
  FourFingerHorizontal,
  Count,
};

struct DecodedBinding {
  uint8_t fingers = 0;
  Axis axis = Axis::Vertical;
};

struct Swipe {
  uint8_t fingers = 0;
  int startX = 0;
  int startY = 0;
  int endX = 0;
  int endY = 0;
  unsigned long durationMs = 0;
};

inline bool decodeBinding(const uint8_t raw, DecodedBinding& decoded) {
  switch (static_cast<Binding>(raw)) {
    case Binding::TwoFingerVertical:
      decoded = {2, Axis::Vertical};
      return true;
    case Binding::TwoFingerHorizontal:
      decoded = {2, Axis::Horizontal};
      return true;
    case Binding::ThreeFingerVertical:
      decoded = {3, Axis::Vertical};
      return true;
    case Binding::ThreeFingerHorizontal:
      decoded = {3, Axis::Horizontal};
      return true;
    case Binding::FourFingerVertical:
      decoded = {4, Axis::Vertical};
      return true;
    case Binding::FourFingerHorizontal:
      decoded = {4, Axis::Horizontal};
      return true;
    case Binding::Off:
    case Binding::Count:
      return false;
  }
  return false;
}

inline bool decodeBinding(const Binding binding, DecodedBinding& decoded) {
  return decodeBinding(static_cast<uint8_t>(binding), decoded);
}

inline bool bindingsConflict(const uint8_t first, const uint8_t second) {
  DecodedBinding decoded;
  return decodeBinding(first, decoded) && first == second;
}

inline bool bindingsConflict(const Binding first, const Binding second) {
  return bindingsConflict(static_cast<uint8_t>(first), static_cast<uint8_t>(second));
}

inline int clampPercent(const int value) { return std::clamp(value, 0, 100); }

inline bool targetAvailable(const Target target, const bool frontlightPresent, const bool hasColorTemperature) {
  return frontlightPresent && (target == Target::Brightness || hasColorTemperature);
}

inline bool evaluate(const uint8_t bindingRaw, const Swipe& swipe, const int screenWidth, const int screenHeight,
                     int& signedDelta) {
  DecodedBinding binding;
  if (!decodeBinding(bindingRaw, binding) || swipe.fingers != binding.fingers || screenWidth < 2 || screenHeight < 2 ||
      swipe.durationMs > 2000)
    return false;

  const int primary = binding.axis == Axis::Vertical ? swipe.startY - swipe.endY : swipe.endX - swipe.startX;
  const int cross = binding.axis == Axis::Vertical ? swipe.endX - swipe.startX : swipe.endY - swipe.startY;
  const int axisSize = binding.axis == Axis::Vertical ? screenHeight : screenWidth;
  const int primaryAbs = std::abs(primary);
  const int crossAbs = std::abs(cross);
  const int minDistancePx = std::max(60, static_cast<int>(std::ceil(static_cast<float>(axisSize) * 0.06f)));
  if (primaryAbs < minDistancePx || primaryAbs * 2 < crossAbs * 3) return false;

  const float distance = static_cast<float>(primaryAbs) / static_cast<float>(axisSize - 1);
  const unsigned long velocityDuration = std::max<unsigned long>(swipe.durationMs, 80);
  const float speed = distance * 1000.0f / static_cast<float>(velocityDuration);
  const float gain = std::clamp(0.5f + 0.5f * speed, 0.5f, 1.5f);
  const int magnitude = std::clamp(static_cast<int>(std::lround(100.0f * distance * gain)), 1, 100);
  signedDelta = primary < 0 ? -magnitude : magnitude;
  return true;
}

inline bool evaluate(const Binding binding, const Swipe& swipe, const int screenWidth, const int screenHeight,
                     int& signedDelta) {
  return evaluate(static_cast<uint8_t>(binding), swipe, screenWidth, screenHeight, signedDelta);
}

inline bool applyDelta(uint8_t& value, const int signedDelta) {
  const uint8_t next = static_cast<uint8_t>(clampPercent(static_cast<int>(value) + signedDelta));
  const bool changed = next != value;
  value = next;
  return changed;
}

}  // namespace SwipeAdjustment
