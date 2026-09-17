#pragma once

#include <cstddef>

namespace MemoryBudget {
constexpr size_t EPUB_PSRAM_RESERVE = 0;
constexpr size_t EPUB_INFLATE_INTERNAL_RESERVE = 0;
constexpr size_t EPUB_FONT_INTERNAL_RESERVE = 0;
constexpr size_t EPUB_LAYOUT_INTERNAL_RESERVE = 0;
inline bool canAllocatePsram(size_t) { return true; }
inline bool canAllocateInternal(size_t, size_t, size_t = 0) { return true; }
}  // namespace MemoryBudget
