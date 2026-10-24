#pragma once
#include <HalStorage.h>

// Only used by ClippingStore's streaming archive scan (nextClippedEpub), which this test's
// round-trip scenarios never reach.
namespace FsHelpers {
inline bool directoryIterationFailed(const HalFile&) { return false; }
}  // namespace FsHelpers
