#pragma once
// This host tool defines ARDUINO_ARCH_ESP32 for other reasons (see
// CMakeLists.txt) but never runs on real ESP-IDF, so there's no real
// sdkconfig.h to include. Leaving CONFIG_IDF_TARGET_ESP32C3 undefined makes
// TableLayoutPolicy::compactFirstTarget resolve to its non-C3 (false) branch,
// matching what an x86/arm64 host actually is.
