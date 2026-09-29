#pragma once

#include <memory>
#include <type_traits>
#include <utility>

#include "HalStorage.h"

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  if (fake::fail(fake::failAlloc)) return nullptr;
  return std::make_unique<T>(std::forward<Args>(args)...);
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  if (fake::fail(fake::failAlloc)) return nullptr;
  return std::make_unique<T>(count);
}

// Real Memory.h's non-ESP32 fallback (see lib/Memory/Memory.h) -- pulled in
// transitively via Ao3CompactIndexRecord.h's <Memory.h>, which this stub
// otherwise shadows for the whole translation unit.
inline bool psramHeapAvailable() { return false; }
