#pragma once
#include <cstdint>

// Merged subset of the real CrossPointSettings: clockUtcOffsetQ (ReadingStatsUtils.cpp) and
// shouldTrackReadingStats() (SyncPending.cpp) are all ClippingStore.cpp's transitive dependencies
// actually read.
struct TestSettings {
  uint8_t clockUtcOffsetQ = 48;
  bool track = true;
  bool shouldTrackReadingStats() const { return track; }
};
inline TestSettings SETTINGS;
