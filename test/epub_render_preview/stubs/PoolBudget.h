#pragma once

#include <cstddef>

namespace MemoryBudget {
inline constexpr size_t EPUB_LAYOUT_INTERNAL_RESERVE = 0;
inline bool canAllocatePsram(size_t) { return true; }
inline bool canAllocateInternal(size_t, size_t, size_t = 0) { return true; }
}  // namespace MemoryBudget
