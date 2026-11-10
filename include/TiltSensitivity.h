#pragma once

#include <cstdint>

namespace TiltSensitivity {
// Prefixed (not LOW/NORMAL/HIGH) because Arduino.h's own LOW/HIGH macros
// (digital pin levels) would otherwise textually rewrite these enumerators --
// a real build-breaking collision whenever Arduino.h is included before this
// header, as HalTiltSensor.h does.
enum Value : uint8_t { TILT_LOW = 0, TILT_NORMAL = 1, TILT_HIGH = 2 };
}
